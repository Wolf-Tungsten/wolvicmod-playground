# wolvicmod ZhuJiang L3 模型：与 XiangShan 的集成方案

> 面向香山开发者的集成说明。我们用 C++ 周期精确建模框架 **wolvicmod** 完整建模了
> kunminghu-v3 的 ZhuJiang L3（Ring + 分布式 DongJiang HNF + S/HI 桥），经 DPI-C
> 整体替换 SoC 中的 `Zhujiang` RTL 实例，在**不改动 emu.cpp、香山侧仅 +83 行**
> 的前提下跑通 coremark 系统级仿真：**difftest 全程零失配，cycleCnt 与 RTL
> 逐拍完全相等**。
>
> 本文档说明：香山侧集成了什么、DPI 契约、如何构建复现、验证证据与已知限制。
> 实施手册（含调试排障）见同目录 `p5-dpi-integration.md`。

## 0. 这是什么，能用来做什么

wolvicmod 是一个 C++ 周期精确建模框架（模块化端口/寄存器语义对齐 Chisel，但模型
就是普通 C++）。本工作用它对 ZhuJiang L3 做了**逐拍周期精确**的行为建模，验证强度
达到"与 RTL 任意拍不可区分"（§5 证据链）。替换进香山后得到的是一个**快速可迭代的
L3 实验平台**：

- **架构探索**：改 L3 微架构（队列深度、仲裁策略、CM 池规模、目录结构……）只需改
  C++，分钟级重编，不需要走 chisel/firtool 流程；
- **性能研究**：模型周期精确，cycleCnt 即真实性能指标，且 C++ 侧可任意插桩
  （统计计数、trace、断言）；
- **参考模型**：模型与 RTL 双向对拍的基础设施都在，可作为 RTL 回归的独立参照。

## 1. 香山侧集成：一个开关 + 一个 BlackBox

所有香山侧改动都在 `wolvicmod-l3` 分支（XiangShan `57cb07415`，difftest
`4cd9de795`），共 5 文件 +83 行：

### 1.1 开关

```bash
make emu CONFIG=DefaultConfig LLC=ZhuJiang WOLVIC_ZJ=1 ...
```

`WOLVIC_ZJ=1`（Makefile）→ `--wolvic-zj`（ArgParser）→ `UseWolvicZjKey`
（`top/WolvicZjBB.scala`）。有 `require(!useWolvicZj || isZhuJiang)` 防呆——
该模型是 ZhuJiang 的模型，只在 `LLC=ZhuJiang` 下合法。

### 1.2 替换点（`src/main/scala/top/Top.scala:377-394`）

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

### 1.3 BlackBox 端口（`top/WolvicZjBB.scala`）

| 端口 | 类型 | 对应 ZhuJiang 边界 |
|---|---|---|
| `rn[i]` | `Flipped(DecoupledPortIO)`（xscache，纯 valid/ready 六通道） | 各 tile L2 的 CHI 口 |
| `ddrc` | `VerilogAXI4Record` | memAXI（`zhujiangMemMaster`，id 6b / addr 49b） |
| `peri[i]` | `VerilogAXI4Record` | cfgAXI（`zhujiangCfgMasters`，id 3b / addr 49b） |

两个容易踩的对齐点（已在代码中处理）：

- **CHI bundle 参数**：ZhuJiang 模式下 tile 侧 decoupledCHI 关 DataCheck/Poison；
  BlackBox 端口必须用同参数 elaborate（例化处 `bbParams` 做了 `alter`），否则
  端口位宽不匹配；
- **链路管理**：`DecoupledPortIO` 不含 linkactive/sysco——模型按链路常通处理，
  与 ZhuJiangBridge 的用法一致。

### 1.4 SV/C++ 不进香山仓库：注入机制

BlackBox 只有端口声明，实现经 difftest 既有/新增机制在**构建期**注入：

| 层 | 机制 | 说明 |
|---|---|---|
| SV 薄壳 | `RTL_INCLUDE=<dir>` → verilator `-y` libdir | difftest 既有机制（`handle_rtl_include_path`）；verilator 按模块名找到 `WolvicZjBB.sv` |
| C++ 静态库 | `USER_CXXFILES/USER_CXXFLAGS/USER_LDFLAGS` | **新增**（difftest `Makefile:328-330`，3 行）：`SIM_CXXFILES/SIM_CXXFLAGS/SIM_LDFLAGS += $(USER_*)`，命令行变量经 MAKEFLAGS 透传进子 make |

`emu.cpp` 与 difftest 主体零改动。`USER_*` 钩子是通用的——任何外部 DPI 模型
（不只本 L3）都可以同样方式接入。

## 2. DPI 契约（SV 薄壳 ↔ C++ 模型）

如果你只想用，本节可以跳过；如果你想给 BlackBox 接自己的模型，这是需要遵守的契约。

```systemverilog
import "DPI-C" function void wolvic_zj_step(
    input  bit [1106:0] in_pack, output bit [1442:0] out_pack);
import "DPI-C" function void wolvic_zj_peek(output bit [1442:0] out_pack);
```

**时序约定（单调用方案）**：模型边界是**全寄存语义**（无输入到输出的纯组合路径，
由模型侧的静态审计常驻担保），因此：

```systemverilog
initial begin wolvic_zj_peek(t_out); o_out = t_out; end
always @(posedge clock) begin
    if (!reset) begin
        wolvic_zj_step(in_pack, t_out);  // 读边沿前输入，返回一拍后的新输出
        o_out <= t_out;                  // NBA 提交
    end
end
```

- 每个 posedge 只调一次 `wolvic_zj_step`：输入是**边沿前**采样，返回值是状态提交
  **之后**的新输出，经 NBA 寄存——与真实 RTL 模块的寄存量边界完全等效；
- **复位期间不调用**：模型构造态即复位完成态（该语义在 golden trace 对齐阶段验证过）；
- `wolvic_zj_peek` 仅在 initial 取初态输出。

**打包布局**：in_pack 1107b / out_pack 1443b，字段顺序 SV/C++ 双侧一一对应
（先列占高位；C++ 从 LSB 反向解包）。CHI flit bits 直接复用模型侧的
`pack()/unpack()` 布局；香山侧特有但模型不用的字段（xscache 扩展的 returnNID、
ccID、mpam_* 等）输入丢弃、输出绑 0，清单见 `WolvicZjBB.sv` 头注释。
漂移防护三件套：SV `initial $fatal` 宽度断言 + C++ 累计位宽检查 + 独立冒烟
测试台全字段断言——布局错位会在第一时间爆炸，不会静默错数据。

## 3. 构建与复现

前置：XiangShan 与 difftest 子模块 checkout 到 `wolvicmod-l3` 分支；模型源码与
glue 在配套仓库（`proj-xiangshan-l3/`，主仓 playground 已固定子模块指针）。

最简路径（配套仓库 Makefile 一键封装，自动处理配置印记/库依赖）：

```bash
make -C proj-xiangshan-l3 emu WOLVIC=1      # 构建（首次 ~25 min）
cd proj-xiangshan-l3/XiangShan
timeout --signal=KILL 1200 ./build/emu -b 0 -e 0 \
  -i ./ready-to-run/coremark-2-iteration.bin \
  --diff ./ready-to-run/riscv64-nemu-interpreter-so
```

等价的香山侧手动命令（在 `NOOP_HOME` 下，展示注入参数全貌）：

```bash
make emu CONFIG=DefaultConfig LLC=ZhuJiang WOLVIC_ZJ=1 EMU_THREADS=16 -j32 \
  RTL_INCLUDE=<proj>/dpi/sv \
  USER_LDFLAGS="<proj>/build/libwolviczj_dpi.a <proj>/build/libzjmodel.a \
                <proj>/build/wolvicmod/third_party/libfst/libwolvicmod_fst.a -lz"
```

注意点：

- `EMU_THREADS` 可以用 16：模型环不在 Verilator 内，不触发 RTL ZhuJiang 的
  `UNOPTTHREADS` 限制（RTL 版只能 8，wolvic 版反而更快——876s vs RTL 的
  线程数受限路径）；
- glue 依赖模型的 FST 支持库，`-lz` 别漏（缺了会在最终链接报
  `fstWriter*` 未定义）；
- 香山 Makefile 不把 `--llc/--wolvic-zj` 当构建依赖，切换配置务必 clean
  （配套 Makefile 的 `.llc-config` 印记机制已自动化）。

**验收判据（三条全中）**：

```
Core 0: HIT GOOD TRAP at pc = 0x80001ca0
Core-0 instrCnt = 663,692, cycleCnt = 316,801, IPC = 2.094981
```

`HIT GOOD TRAP` + difftest 零失配 + **cycleCnt = 316,801 与 RTL ZhuJiang
基线逐拍相等**。

## 4. 模型速览（黑盒里装的是什么）

`WolvicZjTop`（C++，wolvicmod 框架）= kunminghu-v3 ZhuJiang 的完整建模：

```
rn CHI ──▶ XscChiAdapter ──▶ CcSocket（PDC token）──▶ HomeWrapper ──┐
              （协议裁剪/重映射）                        （ChiBuffer/选址）│
                                                                     ▼
  memAXI ◀── SNodeAxiBridge ◀── Ring（10 站 RouterStop，EjectBuffer/注入仲裁）──▶ DongJiang HNF ×2
  cfgAXI ◀── HiNodeAxiLiteBridge ◀──┘                        （FE/Backend/Directory/DataBlock/ChiXbar 全量）
```

建模忠实度策略：每级打拍位置、每个队列深度、每个仲裁点、全部 CM 状态机与
防死锁三件套（环 rsvd 令牌、EjectBuffer VIP 末槽、目录 lockTable）均与 RTL 一致；
时钟门控的上电横扫冻结也建了模（`woken` 语义）。详见配套仓库
`docs/wolvicmod-zhujiang-hierarchy.md` 与 `docs/dongjiang-semantics.md`。

## 5. 验证证据链（为什么可以信任周期精确性）

| 层级 | 内容 | 规模 |
|---|---|---|
| 原语对拍 | wolvicmod 预制件 vs Chisel 生成 RTL（verilator 同激励） | 858 万拍 / 4155 万比对零失配 |
| 模块对拍 | Ring/socket/两桥/DongJiang 五子模块 vs 香山生成 RTL | 每模块数百万至上亿比对零失配 |
| 全程重放 | coremark 全程 golden trace（RTL 边界抓取）重放进模型 | 316,748 拍 × 542 万比对零失配 |
| 系统级 | 替换进 SoC 跑 coremark | difftest 663,692 指令零失配，cycleCnt 逐拍相等 |

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
| 香山侧改动（分支 `wolvicmod-l3`） | `src/main/scala/top/WolvicZjBB.scala`、`Top.scala:377-394,587-603`、`ArgParser.scala`、`Makefile`（`WOLVIC_ZJ`） |
| difftest 钩子（分支 `wolvicmod-l3`） | `difftest/Makefile:328-330`（`USER_CXXFILES/USER_CXXFLAGS/USER_LDFLAGS`） |
| SV 薄壳 / DPI glue（配套仓库） | `proj-xiangshan-l3/dpi/sv/WolvicZjBB.sv`、`proj-xiangshan-l3/dpi/csrc/` |
| 模型源码 | `proj-xiangshan-l3/model/`（顶层 `wolvic_zj_top.h`）+ `wolvicmod/`（框架） |
| 实施手册（排障/冒烟/复现细节） | `proj-xiangshan-l3/docs/p5-dpi-integration.md` |
