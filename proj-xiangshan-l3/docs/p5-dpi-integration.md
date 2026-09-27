# P5 DPI-C 集成手册：wolvicmod L3 模型接入香山 emu

本文档记录 wolvicmod ZhuJiang L3 模型（`WolvicZjTop`）经 DPI-C 替换香山
kunminghu-v3 SoC 中真实 `Zhujiang` RTL、跑通 coremark 系统级仿真的完整方案：
集成接口、构建流程、运行生命周期、实验复现命令与调试排障。

验收基线（2026-09-27，coremark-2-iteration）：

```
Core 0: HIT GOOD TRAP at pc = 0x80001ca0
Core-0 instrCnt = 663,692, cycleCnt = 316,801, IPC = 2.094981   # 与 RTL ZhuJiang 逐拍完全相等
Host time spent: 875,816ms（EMU_THREADS=16）
```

涉及三处仓库改动（均在 `wolvicmod-l3` 分支 / 主仓 main）：

| 仓库 | 提交 | 改动 |
|---|---|---|
| 主仓 playground | `657a9d0` | `proj-xiangshan-l3/dpi/`（SV 薄壳 + C++ glue）、`verify/dpi/` 冒烟台、`tests/test_comb_audit.cpp`、`Makefile` 的 `emu WOLVIC=1` 目标 |
| XiangShan 子模块 | `57cb07415` | `--wolvic-zj` 开关 + BlackBox `WolvicZjBB` + Top.scala 接线 + Makefile `WOLVIC_ZJ=1` |
| difftest 子模块 | `4cd9de795` | Makefile 增加 `USER_CXXFILES/USER_CXXFLAGS/USER_LDFLAGS` 透传钩子（3 行） |

---

## 1. 集成结构总览

```
┌────────────────────────── XiangShan SoC（chisel → Verilog）──────────────────────────┐
│                                                                                      │
│  core_with_l2 (tile) ──decoupledCHI──┐                                               │
│                                      │   ┌───────────────────────────────────────┐   │
│                                      ├──▶│ WolvicZjBB（chisel BlackBox）          │   │
│  zhujiangMemMaster (memAXI) ◀────────┤   │  = dpi/sv/WolvicZjBB.sv（DPI 薄壳）    │   │
│                                      │   │    │ import "DPI-C" wolvic_zj_step    │   │
│  zhujiangCfgMasters (cfgAXI) ◀───────┘   └────┼──────────────────────────────────┘   │
└───────────────────────────────────────────────┼──────────────────────────────────────┘
                                                 ▼ 每 posedge 一次调用
                              ┌───────────────────────────────────────┐
                              │ libwolviczj_dpi.a（C++ glue）          │
                              │   dpi/csrc/wolvic_zj_dpi.cpp           │
                              │   dpi/csrc/wolvic_zj_pack.h（打包布局） │
                              │      │                                 │
                              │      ▼                                 │
                              │ libzjmodel.a → WolvicZjTop（wolvicmod）│
                              └───────────────────────────────────────┘
```

**替换点**：`XiangShan src/main/scala/top/Top.scala:380` 附近。`--wolvic-zj` 开启时
（要求 `LLC=ZhuJiang`，`Top.scala` 有 `require`），`Zhujiang` 实例**与**
`connectCHIToZhuJiang` 在 l_soc 侧例化的 SocketDevSide + flit remap 一起被
`WolvicZjBB` 整体替换——模型内部本就包含这两者，边界恰好对齐。

**边界三件套**（与 P4b `WolvicZjTop` 模型边界完全一致）：

- `rn[i]`：各 tile L2 的 xscache `DecoupledPortIO`（纯 valid/ready 六通道，无
  linkactive/sysco——模型按常通链路处理）
- `ddrc`：memAXI（`zhujiangMemMaster`，S 节点桥出口，id 6b / addr 49b）
- `peri[0]`：cfgAXI（`zhujiangCfgMasters`，HI 节点桥出口，id 3b / addr 49b）

## 2. 集成接口

### 2.1 chisel BlackBox（香山侧）

`XiangShan/src/main/scala/top/WolvicZjBB.scala`：

```scala
class WolvicZjBB(numCores, ddrcParams, periParams)(implicit p) extends BlackBox {
  val io = IO(new Bundle {
    val clock = Input(Clock())
    val reset = Input(Bool())
    val rn   = Vec(numCores, Flipped(new DecoupledPortIO))       // tile 直连
    val ddrc = new VerilogAXI4Record(ddrcParams)                 // memAXI
    val peri = Vec(periParams.size, new VerilogAXI4Record(...))  // cfgAXI
  })
  override val desiredName = "WolvicZjBB"
}
```

要点：

- 例化时以 `bbParams` 关 `CHIDataCheckKey="none"` / `CHIPoisonKey=false`——须与
  ZhuJiang 模式下 tile 侧 decoupledCHI 的参数一致，否则端口宽度对不上；
- `io.clock/io.reset` 显式连接（BlackBox 无隐含时钟）；
- `Top.scala` 接线：tile 直连（模型内含 remap/socket），AXI 经
  `VerilogAXI4Record.viewAs[AXI4Bundle]` 对接；`debugTopDown.l3MissMatch` 与
  `l3Miss` tie-off 为 `false.B`（模型不提供这两个观测信号）；
- `Makefile` 透传：`WOLVIC_ZJ=1` → `COMMON_EXTRA_ARGS += --wolvic-zj`。

### 2.2 DPI 函数与打包布局（`dpi/`）

两个 DPI 函数（`dpi/sv/WolvicZjBB.sv:23-30` 声明，`dpi/csrc/wolvic_zj_dpi.cpp` 实现）：

```systemverilog
import "DPI-C" function void wolvic_zj_step(
    input  bit [1106:0] in_pack,    // 边沿前输入采样
    output bit [1442:0] out_pack);  // 提交后的新输出
import "DPI-C" function void wolvic_zj_peek(output bit [1442:0] out_pack);  // 初态输出
```

- **in_pack = 1107 位**：L2 CHI tx 三通道（valid + bits：REQ 118b / RSP 66b /
  DAT 367b）+ rx 三通道 ready + memAXI 输入侧（awready/wready/arready + b/r 返回）
  + cfgAXI 同构。
- **out_pack = 1443 位**：tx 三通道 ready + rx 三通道（valid + bits）+
  memAXI/cfgAXI 输出侧（aw/w/ar 全字段 + bready/rready）。
- 打包顺序双侧一一对应：SV 侧 `{…}` 拼接（先列占高位），C++ 侧
  `wolvic_zj_pack.h` 的 `unpackInputs/packOutputs` 从 LSB 反向读写；CHI bits 复用
  模型 `xs_flit.h` 的 `pack()/unpack()` 布局。
- **漂移兜底三件套**：SV `initial $fatal` 断言 struct 宽度；C++ `unpackInputs/
  packOutputs` 末尾检查累计位数 `!= kInW/kOutW` 即打印告警；冒烟测试台全字段断言。
  任何一侧改布局必须双侧同步，否则第一时间爆炸而不是静默错位。
- 被模型裁剪的 BlackBox 字段（xscache 扩展：REQ 的 returnNID、DAT 的 ccID/tag、
  SNP 的 mpam_* 等）输入侧丢弃、输出侧绑 0——与 ZhuJiangBridge map* 读取集一致
  （清单见 `WolvicZjBB.sv:17-21` 注释）。

### 2.3 时序约定（单调用方案）

**前提**：模型边界零组合穿透（根 In → 根 Out 无纯组合路径），由常驻审计
`tests/test_comb_audit.cpp`（框架展平图遍历）在 ctest 中担保。因此可以在每个
posedge 只做一次调用而等效 RTL 的全寄存边界：

```systemverilog
// WolvicZjBB.sv:352-362
initial begin wolvic_zj_peek(t_out); o_out = t_out; end
always @(posedge clock) begin
    if (!reset) begin
        wolvic_zj_step(in_pack, t_out);  // act region：读边沿前输入，返回新输出
        o_out <= t_out;                  // NBA 提交，边界输出对齐寄存量语义
    end
end
```

C++ 侧 `wolvic_zj_step`（`wolvic_zj_dpi.cpp:47-61`）：set 输入 → `clk.set(1);
eval()` 提交状态 → 读输出打包 → `clk.set(0); eval()` 拉回低相。

**复位语义**：模型构造态 = 复位完成态（P4b golden trace 对齐时已验证），故 SV 侧
reset 期间不调用 step，复位结束后模型从构造态直接起跑。

## 3. 构建流程

### 3.1 注入机制（emu.cpp 零改动）

| 层 | 机制 | 说明 |
|---|---|---|
| SV 薄壳 | difftest `RTL_INCLUDE` → verilator `-y` libdir | BlackBox 只有声明，verilator 按模块名从 libdir 找到 `WolvicZjBB.sv` |
| C++ glue | difftest `USER_CXXFILES/USER_CXXFLAGS/USER_LDFLAGS` | 新增钩子（`difftest/Makefile:328-330`），命令行变量经 MAKEFLAGS 透传进子 make |
| 链接 | `USER_LDFLAGS` 携带三个静态库 | `libwolviczj_dpi.a` + `libzjmodel.a` + `libwolvicmod_fst.a` + `-lz`（fst 库是模型 FstDumper 依赖） |

### 3.2 一键命令与其展开

```bash
make -C proj-xiangshan-l3 emu WOLVIC=1
```

展开（`proj-xiangshan-l3/Makefile:61-80`）：

1. `.llc-config` 印记检查：`WOLVIC=1` 对应印记 `ZhuJiang-wolvic trace=`，与现有
   emu 不符时自动 clean/重 verilate（与 RTL 版 emu 互不串味）；
2. `cmake --build build --target wolviczj_dpi`：编译 glue 静态库（全局 `-fPIC`，
   emu 以 `-pie` 链接所需；`CMakeLists.txt:26-29`）；
3. `make -C XiangShan emu CONFIG=DefaultConfig LLC=ZhuJiang EMU_THREADS=16
   WOLVIC_ZJ=1 RTL_INCLUDE=<proj>/dpi/sv USER_LDFLAGS="…三个静态库 -lz"`：
   - elaborate（mill，~2 min，仅首次或 scala 改动后）→ `build/rtl/*.sv` 中
     `WolvicZjBB` 以 extmodule 出现；
   - verilate + 编译（~21 min 首次全量；RTL 未变时增量）。
   - 注意 wolvic 版 `EMU_THREADS=16`：环已移出 Verilator，不受 RTL ZhuJiang 的
     `UNOPTTHREADS` 限制（RTL 版只能 8）。

### 3.3 冒烟测试（不经 emu 的快速自检）

```bash
bash proj-xiangshan-l3/verify/dpi/run.sh    # 预期输出 [smoke-dpi] PASS
```

standalone verilate 薄壳 + `verify/dpi/tb_wolvic_zjbb.cpp` 驱动两拍 pattern →
开 `WOLVIC_ZJ_TRACE` 读回 → 全字段断言 + out 侧抽查。改 DPI/glue/打包布局后先跑它，
几十秒定位，不用等 emu。

## 4. 运行生命周期

| 阶段 | 动作 |
|---|---|
| 进程启动 | glue 静态初始化惰性发生：首次调用时 `new WolvicZjTop` + `elaborate()` + `ci.set(0)`（`wolvic_zj_pack.h:32-40` 单例）。构造态 = 复位完成态 |
| 仿真 t=0 | SV `initial` 调 `wolvic_zj_peek`：不推进时钟，取初态输出（全 0/空闲态）驱动边界 |
| reset 期间 | 不调用 step，模型保持构造态 |
| 每 posedge | SV 采 in_pack（边沿前值）→ `wolvic_zj_step` → C++ set 输入 → clk 0→1 eval（提交全部寄存器、组合稳态）→ packOutputs → NBA 提交 o_out → clk 拉回 0 |
| coremark 结束 | 软件写 tohost → emu `HIT GOOD TRAP` 正常退出，模型无需清理（进程结束即回收） |
| difftest | 与模型无感知：difftest 比的是 core 提交指令流，L3 被替换只要行为周期等价即可 |

多线程：emu 开 `--threads-dpi all`，DPI 调用点串行执行；glue 内互斥锁
（`wzj::mtx()`）为防御性兜底。

## 5. 实验复现

前置：`XiangShan` 与 `difftest` 子模块在 `wolvicmod-l3` 分支（主仓
`657a9d0` 已记录正确指针，`git submodule update` 即可）。

```bash
# 1) 构建（首次 ~25 min：elaborate 2 min + verilate/编译 21 min；glue 秒级）
make -C proj-xiangshan-l3 emu WOLVIC=1

# 2) 跑 coremark（~15 min；timeout 防挂死，超时退出码 124/137）
cd proj-xiangshan-l3/XiangShan
timeout --signal=KILL 1200 ./build/emu -b 0 -e 0 \
  -i ./ready-to-run/coremark-2-iteration.bin \
  --diff ./ready-to-run/riscv64-nemu-interpreter-so
```

**验收判据**（三条全中才算过）：

1. `HIT GOOD TRAP`（绿色）——coremark 跑完且软件结果正确（CRC 全对）；
2. difftest 全程无失配（663,692 条指令 vs NEMU）；
3. `cycleCnt = 316,801`——与 RTL ZhuJiang golden 逐拍完全相等。

参考：RTL 基线复现 `make -C proj-xiangshan-l3 emu LLC=ZhuJiang`（EMU_THREADS 自动
取 8），同命令跑 coremark 应得相同 cycleCnt、host time ≈ 313s。

回归测试：`make -C proj-xiangshan-l3 test`（ctest 17/17，含组合穿透审计与
316,748 拍全程 trace 重放）。

## 6. 调试手段

**首要武器：glue trace 对拍。** glue 内置每拍追踪（`wolvic_zj_dpi.cpp:35-43`）：

```bash
WOLVIC_ZJ_TRACE=/tmp/zj.txt WOLVIC_ZJ_TRACE_MAX=100000 \
  ./build/emu -b 0 -e 0 -i ... --diff ...
# 每行：<cyc> in <1107b hex> out <1443b hex>，cyc 从 0 起（复位后第一拍）
```

与 golden trace `build/trace/cm_full.txt`（P4b 提取的 RTL 全程列式 trace，cyc 从
35 起）对拍：第一个分叉拍即模型/集成 bug 现场。两者格式不同（pack hex vs 列式），
比较时按 `wolvic_zj_pack.h` 的 unpack/pack 顺序解码 hex 到列。

**冒烟先行**：任何 DPI 侧改动先 `bash verify/dpi/run.sh`，比 emu 快三个数量级。

**FST 探针**：`verify/trace/fst_probe.cpp` 可查 RTL 侧任意信号波形。

## 7. 已知坑（全部踩过）

1. **链接缺 fst 符号**：glue 经 zjmodel 引入 FstDumper，`USER_LDFLAGS` 必须带
   `libwolvicmod_fst.a -lz`。若 emu 已 verilate 过，`build/verilator-compile/
   VSimTop.mk` 的 LDFLAGS 是首次 verilate 时烘焙的——改 Makefile 的 USER_LDFLAGS
   不会自动重生 VSimTop.mk，须 `rm -rf build/verilator-compile` 重 verilate
   （或确认 RTL 未变时手工 sed 该行）。
2. **Verilator 缓冲按 32 位字**：`svBitVecVal` 是 uint32，缓冲大小
   `ceil(nbits/32)`；fromSv/toSv 必须按 nbits 截，按 uint64 字数组翻倍读写即
   越界（smoke 揪出的真 bug）。同理 `bit_pack.h::setWide` 末字跨界已修。
3. **elaborate 前不可 set**：wolvicmod 框架要求 `elaborate()` 之后才能驱动端口；
   模型单例在构造时已 elaborate，勿在更早的静态初始化期触碰。
4. **clang/g++ 混编**：本机 Verilator 按 clang 配置（verilated.mk 内置 clang
   flags），而模型头用 GCC 扩展（C++20 推导返回类型先用后定义）clang 拒编。
   模式：模型/glue/tb 翻译单元 g++ 预编，verilated 支持文件 clang，clang 链接
   （同 libstdc++ ABI，无兼容问题）。emu 构建由 difftest 工具链自动处理，
   仅自建冒烟/工具时需手工遵守（`verify/dpi/run.sh` 范本）。
5. **宽度漂移静默错位**：SV 与 C++ 打包布局是手工对应的契约——改了任何一侧，
   三处断言（SV `$fatal`、C++ drift 检查、smoke 断言）应当先响；若它们没响而
   行为异常，先怀疑断言本身被绕过。
