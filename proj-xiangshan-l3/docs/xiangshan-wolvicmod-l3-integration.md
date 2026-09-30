# wolvicmod ZhuJiang L3 模型：与 XiangShan RTL 的集成

> 面向香山开发者的集成说明。我们用 C++ 周期精确建模框架 **wolvicmod** 完整建模了
> kunminghu-v3 的 ZhuJiang L3（Ring + 分布式 DongJiang HNF + S/HI 桥），经 DPI-C
> 整体替换 SoC 中的 `Zhujiang` RTL 实例，在**不改动 emu.cpp、香山侧仅 +83 行**
> 的前提下跑通 coremark 系统级仿真：**difftest 全程零失配，cycleCnt 与 RTL
> 逐拍完全相等**。
>
> 本文档说明：集成边界、香山侧集成了什么、DPI 契约、如何构建复现、调试排障与
> 已知限制。模型本身的层次与实现见 `wolvicmod-zhujiang-model.md`，ZhuJiang 概念
> 背景见 `zhujiang-primer.md`，验证方法与测试数据见 `verification-report.md`。

## 0. 这是什么，能用来做什么

wolvicmod 是一个 C++ 周期精确建模框架（模块化端口/寄存器语义对齐 Chisel，但模型
就是普通 C++）。本工作用它对 ZhuJiang L3 做了**逐拍周期精确**的行为建模，验证强度
达到"与 RTL 任意拍不可区分"（证据链见 `verification-report.md`）。替换进香山后得到
的是一个**快速可迭代的 L3 实验平台**：

- **架构探索**：改 L3 微架构（队列深度、仲裁策略、CM 池规模、目录结构……）只需改
  C++，分钟级重编，不需要走 chisel/firtool 流程；
- **性能研究**：模型周期精确，cycleCnt 即真实性能指标，且 C++ 侧可任意插桩
  （统计计数、trace、断言）；
- **参考模型**：模型与 RTL 双向对拍的基础设施都在，可作为 RTL 回归的独立参照。

## 1. 集成边界：ZhuJiang ↔ XiangShan 接口

本节给出替换边界的协议与参数事实。除特别注明外，引用的文件路径相对于
`proj-xiangshan-l3/XiangShan/`（kunminghu-v3 分支）。

### 1.1 路线背景

香山官方预留了 `--external-llc` 外挂接口（`src/main/scala/top/ExternalLLC.scala:228`
的 BlackBox `ExternalLLCWrapper`），但**仅兼容 OpenLLC 模式**（且 NumCores ≤ 2）——
它替换的是集中式 OpenLLC，接口是带 L-credit 流控和 linkactive/sysco 链路状态机的
标准 CHI `PortIO`。本工作建模对象是 ZhuJiang（环形 NoC + 分布式 HNF），因此选择
**路线 B：直接替换 ZhuJiang 实例**——接口是纯 valid/ready 的 `DecoupledPortIO`，
无链路层状态机，明显更好建模；代价是要处理 zhujiang 自定义 flit 格式
（DAT.DBID 16 位等坑，见 §1.3）与分布式 HNF 语义。

### 1.2 两种 LLC 实现对照

香山 `Makefile:53` 默认 `LLC=OpenLLC`，可选 `ZhuJiang`（`--llc` 参数 →
`ArgParser.scala:90` → `LLCConfig`）：

| | OpenLLC（默认） | ZhuJiang |
|---|---|---|
| 本质 | 集中式 L3：`XSCache` 的 `openLLC` 包（独立 Slice/MainPipe/Directory，**非** coupledL2 复用） | 完整环形 NoC：Ring + 分布式 HNF（LLC 按 bank 拆到环上节点） |
| 拓扑 | 每 tile 一条 CHI 链路点对点直连 L3；单 SN 口经 OpenNCB（CHI→AXI4 桥）出内存 | tile 经 decoupled CHI 接环上 CC 节点；内存/DMA/配置都是环上节点 |
| 实例化点 | `top/Top.scala:334-343` | `top/Top.scala:378-382` + 拓扑 `top/ZhuJiangNoCTopology.scala` |
| DefaultConfig | 32MB = 4 bank × 8MB，16 路，8192 sets/bank，带 client directory（snoop filter） | 32MB / 16 路，2 bank（HF 节点）× 2 HNF |
| 时钟域 | 全系统单时钟 `io.clock`，无异步桥 | 同左 |

两种配置的 coremark 基线数据（cycleCnt/IPC/host time）见 `verification-report.md`。

### 1.3 接口定义：`DecoupledPortIO`

定义于 `XSCache/src/main/scala/xscache/chi/LinkLayer.scala:99-102`：

```scala
class DecoupledPortIO extends Bundle {
  val tx = new DecoupledDownwardsLinkIO          // req/rsp/dat，均为 DecoupledIO
  val rx = Flipped(new DecoupledUpwardsLinkIO)   // rsp/dat/snp，均为 DecoupledIO
}
```

- 每通道是 **DecoupledIO（valid/ready + 结构化 flit Bundle）**，不是标准 CHI 的
  `flitpend/flitv/flit/lcrdv` 四信号链路层；
- **没有** L-credit、**没有** linkactive/sysco 链路状态机——永远 RUN 态，比标准
  CHI 接口明显好建模；
- flit Bundle：`CHIREQ/CHISNP/CHIDAT/CHIRSP` 见
  `xscache/chi/Message.scala:428/481/511/557`。

**连接机制**（`Top.scala:552` → `XSCache/src/test/scala/ZhuJiangBridge.scala:76-150`，
注意在 test 目录，靠 test classpath 编入）：

1. 六条通道逐一 valid/ready 对接；
2. **字段级 flit 重映射**：`xscache.chi.CHIREQ/...` ↔ `zhujiang.chi.ReqFlit/...`
   （两侧是两套独立 Bundle 定义）；
3. require 校验仅 4 条：CC 节点类型、`socket=="sync"`、无 DataCheck、无 Poison。

**nodeID 机制**：tile 侧 nodeID 在 ZhuJiang 模式下不进 flit（coupledL2 decoupled
路径 SrcID 恒为 0）；真正的 SrcID 由 CC 路由器注入环时盖章
（`xijiang/router/base/BaseRouter.scala:208-210`），仅保留设备侧 aid（低 3 位）。
回程路由靠 TgtID。

### 1.4 参数对齐：无协商，配置约定 + 硬编码

两侧两套独立常量定义，对接处无位宽 require，一致性靠编译期配置锁死：

| 参数 | 值 | 对齐机制 |
|---|---|---|
| CHI Issue | E.b | `SoC.scala:164` 强制，xscache 侧查表锁定 nodeID=11、TXNID=12 |
| nodeID | 11 位（8 nid + 3 aid） | `ZhuJiangNoCTopology.scala:17-18` 硬编码 |
| 地址 / 数据 | 48 位 / 256 位 | 两侧各自硬编码 |
| DataCheck / Poison | 关闭 | `L2Top.scala:130` + `Top.scala:112` 强制关 |
| ⚠️ DAT.DBID | zhujiang 侧 **16 位** vs xscache 侧 12 位 | 不对齐！桥里靠 Chisel 隐式零扩展混过去（`ZhuJiangBridge.scala:214,232`） |

L2 行为约束：`bufferableNC=false, endpointOrderNC=true, enableL2Flush=false`
（`Configs.scala:415-423`）。

### 1.5 在 XiangShan 外实现 ZhuJiang 兼容模块的硬约束清单

1. CHI E.b flit 格式（`zhujiang/chi/Flit.scala:27-126` 的字段布局）；
2. nodeIdBits = 11（8 nid + 3 aid）、TxnID=12、RSP.DBID=12、**DAT.DBID=16**、
   地址 48 位、数据 256 位、无 DataCheck/Poison；
3. socket 类型 `"sync"`，纯 valid/ready，无 L-credit/链路状态机；
4. SrcID 由 NoC 侧注入时盖章，设备侧 flit SrcID 应为 0（aid 位保留）；
5. L2 侧行为约定：`bufferableNC=false, endpointOrderNC=true, enableL2Flush=false`。

## 2. 香山侧集成：一个开关 + 一个 BlackBox

所有香山侧改动都在 `wolvicmod-l3` 分支（XiangShan `57cb07415`，difftest
`4cd9de795`），共 5 文件 +83 行。整个集成涉及三处仓库改动：

| 仓库 | 提交 | 改动 |
|---|---|---|
| 主仓 playground | `657a9d0` | `proj-xiangshan-l3/dpi/`（SV 薄壳 + C++ glue）、`verify/dpi/` 冒烟台、`tests/test_comb_audit.cpp`、`Makefile` 的 `emu WOLVIC=1` 目标 |
| XiangShan 子模块 | `57cb07415` | `--wolvic-zj` 开关 + BlackBox `WolvicZjBB` + Top.scala 接线 + Makefile `WOLVIC_ZJ=1` |
| difftest 子模块 | `4cd9de795` | Makefile 增加 `USER_CXXFILES/USER_CXXFLAGS/USER_LDFLAGS` 透传钩子（3 行） |

集成结构总览：

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

### 2.1 开关

```bash
make emu CONFIG=DefaultConfig LLC=ZhuJiang WOLVIC_ZJ=1 ...
```

`WOLVIC_ZJ=1`（Makefile → `COMMON_EXTRA_ARGS += --wolvic-zj`）→ `--wolvic-zj`
（ArgParser）→ `UseWolvicZjKey`（`top/WolvicZjBB.scala`）。有
`require(!useWolvicZj || isZhuJiang)` 防呆——该模型是 ZhuJiang 的模型，只在
`LLC=ZhuJiang` 下合法。

### 2.2 替换点（`src/main/scala/top/Top.scala:377-394`）

`--wolvic-zj` 开启时，XSTop 中用 BlackBox `WolvicZjBB` **整体替换**：

- `Zhujiang` 实例本身，**以及**
- `connectCHIToZhuJiang` 在 l_soc 侧例化的 SocketDevSide + flit remap

——即"tile 的 decoupledCHI 口"到"memAXI/cfgAXI 口"之间的全部。这与模型的建模边界
完全一致（模型内含 socket 两半与 remap），所以接线极其直白（`Top.scala:587-603`）：

```scala
wolvicZj_opt.foreach { wj =>
  withClockAndReset(io.clock, io.reset) {
    wj.io.clock := io.clock; wj.io.reset := io.reset.asBool
    for ((core, i) <- core_with_l2.zipWithIndex)
      wj.io.rn(i) <> core.module.io.decoupledCHI.get          // tile 直连
    zjMemAxi <> wj.io.ddrc.viewAs[AXI4Bundle]                 // memAXI（S 节点方向）
    wj.io.peri.zip(zhujiangCfgMasters).foreach { ... }        // cfgAXI（HI 节点方向）
    // 模型不提供的观测信号 tie-off
    core_with_l2.foreach(_.module.io.debugTopDown.l3MissMatch := false.B)
    core_with_l2.foreach(_.module.io.l3Miss := false.B)
  }
}
```

### 2.3 BlackBox 端口（`top/WolvicZjBB.scala`）

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

| 端口 | 类型 | 对应 ZhuJiang 边界 |
|---|---|---|
| `rn[i]` | `Flipped(DecoupledPortIO)`（xscache，纯 valid/ready 六通道） | 各 tile L2 的 CHI 口 |
| `ddrc` | `VerilogAXI4Record` | memAXI（`zhujiangMemMaster`，id 6b / addr 49b） |
| `peri[i]` | `VerilogAXI4Record` | cfgAXI（`zhujiangCfgMasters`，id 3b / addr 49b） |

要点（均已在代码中处理）：

- **CHI bundle 参数**：ZhuJiang 模式下 tile 侧 decoupledCHI 关 DataCheck/Poison；
  例化时以 `bbParams` 关 `CHIDataCheckKey="none"` / `CHIPoisonKey=false`——须与
  tile 侧参数一致，否则端口位宽不匹配；
- **显式时钟**：`io.clock/io.reset` 显式连接（BlackBox 无隐含时钟）；
- **链路管理**：`DecoupledPortIO` 不含 linkactive/sysco——模型按链路常通处理，
  与 ZhuJiangBridge 的用法一致；
- **tie-off**：`debugTopDown.l3MissMatch` 与 `l3Miss` tie-off 为 `false.B`
  （模型不提供这两个观测信号）。

### 2.4 SV/C++ 不进香山仓库：注入机制

BlackBox 只有端口声明，实现经 difftest 既有/新增机制在**构建期**注入：

| 层 | 机制 | 说明 |
|---|---|---|
| SV 薄壳 | `RTL_INCLUDE=<dir>` → verilator `-y` libdir | difftest 既有机制（`handle_rtl_include_path`）；verilator 按模块名找到 `WolvicZjBB.sv` |
| C++ 静态库 | `USER_CXXFILES/USER_CXXFLAGS/USER_LDFLAGS` | **新增**（difftest `Makefile:328-330`，3 行）：`SIM_CXXFILES/SIM_CXXFLAGS/SIM_LDFLAGS += $(USER_*)`，命令行变量经 MAKEFLAGS 透传进子 make |
| 链接 | `USER_LDFLAGS` 携带三个静态库 | `libwolviczj_dpi.a` + `libzjmodel.a` + `libwolvicmod_fst.a` + `-lz`（fst 库是模型 FstDumper 依赖） |

`emu.cpp` 与 difftest 主体零改动。`USER_*` 钩子是通用的——任何外部 DPI 模型
（不只本 L3）都可以同样方式接入。

## 3. DPI 契约（SV 薄壳 ↔ C++ 模型）

如果你只想用，本节可以跳过；如果你想给 BlackBox 接自己的模型，这是需要遵守的契约。

### 3.1 DPI 函数与打包布局

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
- **打包顺序双侧一一对应**：SV 侧 `{…}` 拼接（先列占高位），C++ 侧
  `wolvic_zj_pack.h` 的 `unpackInputs/packOutputs` 从 LSB 反向读写；CHI bits 复用
  模型 `xs_flit.h` 的 `pack()/unpack()` 布局。
- **被模型裁剪的 BlackBox 字段**（xscache 扩展：REQ 的 returnNID、DAT 的
  ccID/tag、SNP 的 mpam_* 等）输入侧丢弃、输出侧绑 0——与 ZhuJiangBridge map*
  读取集一致（清单见 `WolvicZjBB.sv:17-21` 头注释）。
- **漂移兜底三件套**：SV `initial $fatal` 断言 struct 宽度；C++ `unpackInputs/
  packOutputs` 末尾检查累计位数 `!= kInW/kOutW` 即打印告警；冒烟测试台全字段断言。
  任何一侧改布局必须双侧同步，否则第一时间爆炸而不是静默错位。

### 3.2 时序约定（单调用方案）

**前提**：模型边界是**全寄存语义**（根 In → 根 Out 无纯组合路径），由常驻审计
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

- 每个 posedge 只调一次 `wolvic_zj_step`：输入是**边沿前**采样，返回值是状态提交
  **之后**的新输出，经 NBA 寄存——与真实 RTL 模块的寄存量边界完全等效；
- C++ 侧 `wolvic_zj_step`（`wolvic_zj_dpi.cpp:47-61`）：set 输入 → `clk.set(1);
  eval()` 提交状态 → 读输出打包 → `clk.set(0); eval()` 拉回低相；
- **复位语义**：模型构造态 = 复位完成态（golden trace 对齐阶段已验证），故 SV 侧
  reset 期间不调用 step，复位结束后模型从构造态直接起跑；
- `wolvic_zj_peek` 仅在 initial 取初态输出（全 0/空闲态）驱动边界。

## 4. 构建与运行

前置：XiangShan 与 difftest 子模块 checkout 到 `wolvicmod-l3` 分支（主仓
`657a9d0` 已记录正确指针，`git submodule update` 即可）；模型源码与 glue 在配套
仓库 `proj-xiangshan-l3/`。

### 4.1 一键命令与其展开

```bash
make -C proj-xiangshan-l3 emu WOLVIC=1      # 构建（首次 ~25 min）
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

等价的香山侧手动命令（在 `NOOP_HOME` 下，展示注入参数全貌）：

```bash
make emu CONFIG=DefaultConfig LLC=ZhuJiang WOLVIC_ZJ=1 EMU_THREADS=16 -j32 \
  RTL_INCLUDE=<proj>/dpi/sv \
  USER_LDFLAGS="<proj>/build/libwolviczj_dpi.a <proj>/build/libzjmodel.a \
                <proj>/build/wolvicmod/third_party/libfst/libwolvicmod_fst.a -lz"
```

注意点：

- `EMU_THREADS` 可以用 16：模型环不在 Verilator 内，不触发 RTL ZhuJiang 的
  `UNOPTTHREADS` 限制（RTL 版只能 8，wolvic 版反而更快）；
- glue 依赖模型的 FST 支持库，`-lz` 别漏（缺了会在最终链接报
  `fstWriter*` 未定义）；
- 香山 Makefile 不把 `--llc/--wolvic-zj` 当构建依赖，切换配置务必 clean
  （配套 Makefile 的 `.llc-config` 印记机制已自动化）。

### 4.2 冒烟测试（不经 emu 的快速自检）

```bash
bash proj-xiangshan-l3/verify/dpi/run.sh    # 预期输出 [smoke-dpi] PASS
```

standalone verilate 薄壳 + `verify/dpi/tb_wolvic_zjbb.cpp` 驱动两拍 pattern →
开 `WOLVIC_ZJ_TRACE` 读回 → 全字段断言 + out 侧抽查。改 DPI/glue/打包布局后先跑它，
几十秒定位，不用等 emu。

### 4.3 运行生命周期

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

### 4.4 实验复现与验收

```bash
# 1) 构建（首次 ~25 min：elaborate 2 min + verilate/编译 21 min；glue 秒级）
make -C proj-xiangshan-l3 emu WOLVIC=1

# 2) 跑 coremark（~15 min；timeout 防挂死，超时退出码 124/137）
cd proj-xiangshan-l3/XiangShan
timeout --signal=KILL 1200 ./build/emu -b 0 -e 0 \
  -i ./ready-to-run/coremark-2-iteration.bin \
  --diff ./ready-to-run/riscv64-nemu-interpreter-so
```

**验收判据（三条全中才算过）**：

1. `HIT GOOD TRAP`（绿色）——coremark 跑完且软件结果正确（CRC 全对）；
2. difftest 全程无失配（663,692 条指令 vs NEMU）；
3. `cycleCnt = 316,801`——与 RTL ZhuJiang golden 逐拍完全相等。

```
Core 0: HIT GOOD TRAP at pc = 0x80001ca0
Core-0 instrCnt = 663,692, cycleCnt = 316,801, IPC = 2.094981
```

参考：RTL 基线复现 `make -C proj-xiangshan-l3 emu LLC=ZhuJiang`（EMU_THREADS 自动
取 8），同命令跑 coremark 应得相同 cycleCnt、host time ≈ 313s。

回归测试：`make -C proj-xiangshan-l3 test`（ctest 17/17，含组合穿透审计与
316,748 拍全程 trace 重放）。

## 5. 调试与排障

### 5.1 调试手段

**首要武器：glue trace 对拍。** glue 内置每拍追踪（`wolvic_zj_dpi.cpp:35-43`）：

```bash
WOLVIC_ZJ_TRACE=/tmp/zj.txt WOLVIC_ZJ_TRACE_MAX=100000 \
  ./build/emu -b 0 -e 0 -i ... --diff ...
# 每行：<cyc> in <1107b hex> out <1443b hex>，cyc 从 0 起（复位后第一拍）
```

与 golden trace `build/trace/cm_full.txt`（RTL 全程列式 trace，cyc 从 35 起）对拍：
第一个分叉拍即模型/集成 bug 现场。两者格式不同（pack hex vs 列式），比较时按
`wolvic_zj_pack.h` 的 unpack/pack 顺序解码 hex 到列。

**冒烟先行**：任何 DPI 侧改动先 `bash verify/dpi/run.sh`，比 emu 快三个数量级。

**FST 探针**：`verify/trace/fst_probe.cpp` 可查 RTL 侧任意信号波形。

### 5.2 已知坑（全部踩过）

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

## 6. 已知限制与扩展方向

- **配置面**：当前验证的是 `DefaultConfig` 单 tile（1 个 rn 口）、ZhuJiang 32MB/
  2 HNF 配置。chisel BlackBox 已按 `Vec(numCores)` 参数化，但 SV 薄壳与打包布局
  目前按单 rn 例化——多 tile 需要扩展 SV 壳与 pack 表（模型内部拓扑参数化，
  改动集中在打包层）；
- **CHI 特性子集**：按香山实际使用面建模（DataCheck/Poison 关闭、无 linkactive
  管理、无 DVM/MPAM 语义）；若上游打开这些开关，模型需相应扩展，打包层的
  字段裁剪清单也要同步；
- **观测信号**：`l3Miss`/`l3MissMatch` tie-off 为 0，性能计数器未建模——若
  top-down 分析需要这些，可在模型里补统计导出；
- **调试钩子**：glue 支持 `WOLVIC_ZJ_TRACE=<path>` 每拍落 in/out pack，可与
  golden trace 对拍定位分叉拍（配套仓库有全套 trace 提取/重放工具链）。

## 7. 相关位置速查

| 内容 | 位置 |
|---|---|
| 香山侧改动（分支 `wolvicmod-l3`，提交 `57cb07415`） | `src/main/scala/top/WolvicZjBB.scala`、`Top.scala:377-394,587-603`、`ArgParser.scala`、`Makefile`（`WOLVIC_ZJ`） |
| difftest 钩子（分支 `wolvicmod-l3`，提交 `4cd9de795`） | `difftest/Makefile:328-330`（`USER_CXXFILES/USER_CXXFLAGS/USER_LDFLAGS`） |
| 主仓 playground（提交 `657a9d0`） | `proj-xiangshan-l3/dpi/`（SV 薄壳 + C++ glue）、`verify/dpi/` 冒烟台、`tests/test_comb_audit.cpp`、`Makefile` 的 `emu WOLVIC=1` 目标 |
| SV 薄壳 / DPI glue（配套仓库） | `proj-xiangshan-l3/dpi/sv/WolvicZjBB.sv`、`proj-xiangshan-l3/dpi/csrc/` |
| 模型源码 | `proj-xiangshan-l3/model/`（顶层 `wolvic_zj_top.h`）+ `wolvicmod/`（框架） |
| 模型层次/实现文档 | `wolvicmod-zhujiang-model.md` |
| ZhuJiang 概念背景 | `zhujiang-primer.md` |
| 验证方法与测试报告 | `verification-report.md` |
