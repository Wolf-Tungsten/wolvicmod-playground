# proj-xiangshan-l3：XiangShan L3 Cache 建模项目

目标：用 wolvicmod 建模 XiangShan（昆明湖 V3）的 L3 cache，通过 DPI-C 集成到 XiangShan RTL 模型中，最终成功运行 coremark 仿真。

本文档记录项目环境搭建、已验证的构建/仿真命令，以及对 XiangShan L3 边界的探查结论。所有文件路径相对于 `XiangShan/`（即 `proj-xiangshan-l3/XiangShan/`，kunminghu-v3 分支，基线 commit `aa6b520`）。

---
（本文档为 README 瘦身前的完整内容存档：环境搭建、coremark 基线、L3 边界探查、路线选型。当前跑法见 ../README.md。）

## 1. 环境搭建（已完成 ✅）

### 1.1 仓库结构

```
proj-xiangshan-l3/
├── Makefile          # 一键入口：test / cosim [M=模块] / emu [LLC=ZhuJiang] / coremark / emu-clean
├── XiangShan/        # git submodule: OpenXiangShan/XiangShan, kunminghu-v3 分支
├── prefab/           # 项目侧时序原语库（XiangShan 生态特有元件，namespace zj::prefab，§1.4）
├── tests/            # 项目侧 prefab 的 doctest 单测（test_prefab_*.cpp）
├── verify/           # 项目侧预制菜 vs XiangShan 生态 RTL 的 Verilator 对拍（refgen 生成 SV + cosim，§1.4 末）
├── docs/             # 建模规划文档（wolvicmod-zhujiang-hierarchy.md 等）
├── CMakeLists.txt    # 项目侧构建（add_subdirectory ../wolvicmod；tests 逐文件独立 ctest 条目）
└── README.md         # 本文档

# 全部构建产物在 build/ 下（已入 .gitignore）：build/ 是 CMake 构建目录，
# build/verify/ 是对拍产物根（sv/obj/bin/mill/日志）。
```

```bash
# 在 playground 仓库根目录执行
git submodule add -b kunminghu-v3 git@github.com:OpenXiangShan/XiangShan.git proj-xiangshan-l3/XiangShan
cd proj-xiangshan-l3/XiangShan
make init    # 初始化全部子模块（含 ready-to-run、XSCache 及其嵌套的 OpenNCB/ZhuJiang）
```

### 1.2 系统依赖

| 工具 | 版本 | 备注 |
|---|---|---|
| OpenJDK | 21 | 已验证可用 |
| mill | launcher 自动切换 | XiangShan `.mill-version` 要求 0.12.17，launcher 自动下载 |
| Verilator | 5.047 | 已验证可用 |
| libsqlite3-dev | — | **必须手动安装** `sudo apt install libsqlite3-dev`（emu 编译 chisel_db/perfCCT 需要） |
| zlib1g-dev / libzstd-dev | — | 常规已装 |
| RISC-V 工具链 | 不需要 | 使用 ready-to-run 预编译二进制 |

### 1.3 环境变量

```bash
export NOOP_HOME=<绝对路径>/proj-xiangshan-l3/XiangShan   # 构建与运行 emu 都需要
# NEMU_HOME / AM_HOME 不需要（使用 ready-to-run 的预编译 riscv64-nemu-interpreter-so）
```

机器配置参考：32 核 / 186GB RAM，DefaultConfig emu 全量构建约 25~35 分钟。

### 1.4 项目侧时序原语库（prefab/）

`prefab/` 是 ZhuJiang 建模的**项目侧时序原语库**（命名空间 `zj::prefab`，伞头 `prefab/prefab.h`）：语义来自 XiangShan 生态（xs-utils / dongjiang）的可复用元件——`FastQueue`（`fastq.h`）、`VipArb`/`QosRRArb`/`QosFixedArb`/`Alloc`（`xsarb.h`）、`SpSram`/`DpSram`（`sram.h`），全部与参考 RTL 拍级对齐。chisel3 标准库语义的通用元件（`Valid` 通道约定、`Queue`、`FixedArb`、`RRArb`、`ValidPipe`）收在 wolvicmod 框架仓的 `include/wolvicmod/prefab/`（收录边界详见 `../wolvicmod/README.md` §4 与 `docs/wolvicmod-zhujiang-hierarchy.md` §2.2），本库直接复用。仲裁器族（含 wolvicmod 侧 FixedArb/RRArb）统一为阵列端口形态：输入侧一路 `In<std::array<Valid<T>,N>> in` + 一路 `Out<std::array<bool,N>> in_rdy`（Alloc 反向：`Out<std::array<Valid<T>,N>> out` + `In<std::array<bool,N>> out_rdy`）。

构建与单测（doctest，`-Wall -Wextra` 零警告）：

```bash
make -C proj-xiangshan-l3 test    # 一键：cmake 配置 + 构建 + ctest
# 或手动：cmake -S proj-xiangshan-l3 -B proj-xiangshan-l3/build && cmake --build proj-xiangshan-l3/build -j && cd proj-xiangshan-l3/build && ctest
```

本目录（含未来模型本体）的建模代码遵循 wolvicmod 的**同名解包约定**（lambda 解包绑定名与读集信号同名同序），见 `../wolvicmod/README.md` §3.9。

**验证组织**（2026-09-29 起收敛为两条端到端路径 + 单测回归网）：`tests/` = 语义文档与快速回归（纯 C++ doctest，毫秒级；每测试文件独立 ctest 条目，`ctest -R test_prefab_<模块>` 单跑）；`verify/` = 两条端到端路径的基建——① 孤立 L3 共栖 A/B 回放（`make zjrtl-replay`，wolvic vs verilated RTL 对同一 coremark trace 逐拍比对）；② XiangShan 集成 emu 对比（`make emu [WOLVIC=1]` + `make coremark`）。详见 `verify/README.md`。预制菜元件级 RTL 对拍基建（verify/cosim + refgen + run.sh）已于 2026-09-29 删除——其对拍结论已固化（零失配记录见 docs/ 各文档），演进中的等价性保障由路径① 承担；wolvicmod 框架仓的 verify/（chisel3 标准库元件对拍）保留不动。

---

## 2. coremark 仿真（已跑通 ✅）

### 2.0 推荐入口：项目 Makefile

```bash
make -C proj-xiangshan-l3 emu                  # 构建 emu（默认 OpenLLC；LLC=ZhuJiang 切换配置）
make -C proj-xiangshan-l3 emu WOLVIC=1         # 构建 wolvicmod L3 模型版 emu（P5，见 §2.5）
make -C proj-xiangshan-l3 coremark             # 用现有 emu 跑 coremark
```

Makefile 用 `XiangShan/build/.llc-config` 印记跟踪当前 emu 的 LLC 配置：请求的 LLC 与印记不一致（或未知）时**自动先 emu-clean** 再重建——解决了 XiangShan Makefile 不把 `--llc` 当构建依赖导致的静默复用旧 Verilog 的坑。`make coremark` 不触发配置切换，用现有 emu 直接跑。

### 2.1 OpenLLC（默认配置）

```bash
cd $NOOP_HOME
make emu CONFIG=DefaultConfig EMU_THREADS=16 -j32
./build/emu -b 0 -e 0 -i ./ready-to-run/coremark-2-iteration.bin --diff ./ready-to-run/riscv64-nemu-interpreter-so
```

结果：`HIT GOOD TRAP`，difftest 通过，cycleCnt = 296,966，IPC = 2.234885，host time ≈ 65s。

### 2.2 ZhuJiang 配置

```bash
cd $NOOP_HOME
make clean     # ⚠️ 必须！Makefile 不把 --llc 参数当构建依赖，不 clean 会静默复用旧 Verilog
make emu CONFIG=DefaultConfig LLC=ZhuJiang EMU_THREADS=8 -j32
./build/emu -b 0 -e 0 -i ./ready-to-run/coremark-2-iteration.bin --diff ./ready-to-run/riscv64-nemu-interpreter-so
```

结果：`HIT GOOD TRAP`，difftest 通过，cycleCnt = 316,801，IPC = 2.094981，host time ≈ 313s。

**ZhuJiang 特有注意事项：**

- `ISSUE` 默认 `E.b`，恰好满足 `LLC=ZhuJiang` 的硬性要求（`Makefile:64`）
- `EMU_THREADS=16` 不可行：Verilator 报 `UNOPTTHREADS`（环形 NoC 可并行度不足），warning 按 error 处理导致 verilation 失败；`8` 已验证可干净通过（代价是仿真变慢）
- 若遇到 verilation 失败但 `build/verilator-compile/` 已残留产出，make 会误判为完成——必须 `rm -rf build/verilator-compile` 再重建

### 2.4 波形抓取与 trace 重放（P2 验收 ✅）

```bash
make -C proj-xiangshan-l3 emu LLC=ZhuJiang TRACE=fst   # 重建带 FST 波形的 emu（~23 min；只重 verilate，不重 elaborate）
make -C proj-xiangshan-l3 replay                        # 一键：dump 前 2 万拍 FST → 提取 CC 边界 trace → 重放对拍（~3 min）
make -C proj-xiangshan-l3 replay N=50000                # 换窗口长度
```

`make replay` 的流程：

1. `emu -b 0 -e N -C N --dump-wave` 抓 coremark 前端 N 拍 FST（`build/trace/cc_front.fst`）
2. `fst2vcd | verify/trace/extract_cc_trace.py` 流式过滤出 CC 边界两侧信号（L2 CHI 缝 `core_with_l2.io_decoupledCHI_*` 六通道 + socket 环侧 `zhujiang_opt.ccn_0_0x8.io_dev_*` 七通道，~193 列），重建逐拍 trace（`build/trace/cc_front.txt`）
3. `tests/test_trace_replay.cpp` 重放：输入侧逐拍驱动 wolvicmod `XscChiAdapter+CcSocket`，输出侧逐拍与 trace 比对

验收结果：前端 2 万拍（实际有效 19,965 拍，前 35 拍复位）**265,459 次比对零失配**。

注意点：

- `TRACE=fst` 与 LLC 配置一样被 `.llc-config` 印记跟踪：仅 trace 设置变化时只删 `build/verilator-compile` 重 verilate，不重新 elaborate RTL
- `--dump-wave` 只在 `[log_begin, log_end]` 窗口内 dump（`-b/-e`）；不带窗口跑会段错误（emu 未编 trace 支持时）
- trace 比对中 bits 仅在 valid=1 时有效：difftest 开 `RANDOMIZE_REG_INIT`，valid=0 时 RTL 的 bits 是随机初值（don't-care）
- `make test` 在 trace 缺失时自动跳过 `test_trace_replay`（ctest SKIP）


### 2.3 两种配置对比（coremark-2-iteration 基线）

| 配置 | cycleCnt | IPC | host time |
|---|---|---|---|
| OpenLLC | 296,966 | 2.235 | ~65s |
| ZhuJiang | 316,801 | 2.095 | ~313s（EMU_THREADS=8） |

两者 CRC 一致、difftest 均通过。ZhuJiang 周期数多 ~6.7%，符合环形 NoC + 分布式 HNF 的延迟特性。**这两组数据作为 wolvicmod L3 模型的正确性/性能基线。**

### 2.5 wolvicmod L3 模型系统级集成（P5 验收 ✅）

wolvicmod 建的 ZhuJiang 模型（WolvicZjTop）经 DPI-C 整体替换香山 SoC 中的 `Zhujiang` 实例，跑通完整 coremark：

```bash
make -C proj-xiangshan-l3 emu WOLVIC=1    # 构建 wolvic 版 emu（自动先编 libwolviczj_dpi.a；与 RTL 版共用 .llc-config 印记机制）
cd proj-xiangshan-l3/XiangShan
./build/emu -b 0 -e 0 -i ./ready-to-run/coremark-2-iteration.bin --diff ./ready-to-run/riscv64-nemu-interpreter-so
```

结果：`HIT GOOD TRAP`，difftest 663,692 指令零失配，**cycleCnt = 316,801 与 RTL ZhuJiang 逐拍完全相等**，host time ≈ 876s（EMU_THREADS=16——模型环不在 Verilator 内，不受 UNOPTTHREADS 限制）。

集成结构（详见 `docs/p5-dpi-integration.md` 集成手册：接口/构建/生命周期/复现/排障，`docs/wolvicmod-zhujiang-implementation-plan.md` 步骤 7 有验收数据）：

- **chisel 侧**（XiangShan 子模块 `wolvicmod-l3` 分支）：`--wolvic-zj` 开关 → BlackBox `WolvicZjBB` 替换 `Zhujiang` + SocketDevSide + flit remap，边界与 WolvicZjTop 完全一致（L2 CHI 六通道 + memAXI + cfgAXI）
- **SV 薄壳** `dpi/sv/WolvicZjBB.sv`：经 difftest `RTL_INCLUDE` 注入；每 posedge 调一次 DPI `wolvic_zj_step`（前提：`tests/test_comb_audit.cpp` 审计证明模型边界零组合穿透）
- **C++ glue** `dpi/csrc/`：静态库 `libwolviczj_dpi.a`，经 difftest `USER_CXXFILES/USER_CXXFLAGS/USER_LDFLAGS` 钩子（difftest 子模块 `wolvicmod-l3` 分支）链接注入；emu.cpp 零改动
- 调试钩子：环境变量 `WOLVIC_ZJ_TRACE=<path>` 让 glue 落每拍 in/out pack hex，可与 golden trace 对拍定位分叉拍

---

## 3. XiangShan L3 边界探查结论

### 3.1 两种 LLC 实现

`Makefile:53` 默认 `LLC=OpenLLC`，可选 `ZhuJiang`（`--llc` 参数 → `ArgParser.scala:90` → `LLCConfig`）：

| | OpenLLC（默认） | ZhuJiang |
|---|---|---|
| 本质 | 集中式 L3：`XSCache` 的 `openLLC` 包（独立 Slice/MainPipe/Directory，**非** coupledL2 复用） | 完整环形 NoC：Ring + 分布式 HNF（LLC 按 bank 拆到环上节点） |
| 拓扑 | 每 tile 一条 CHI 链路点对点直连 L3；单 SN 口经 OpenNCB（CHI→AXI4 桥）出内存 | tile 经 decoupled CHI 接环上 CC 节点；内存/DMA/配置都是环上节点 |
| 实例化点 | `top/Top.scala:334-343` | `top/Top.scala:378-382` + 拓扑 `top/ZhuJiangNoCTopology.scala` |
| DefaultConfig | 32MB = 4 bank × 8MB，16 路，8192 sets/bank，带 client directory（snoop filter） | 32MB / 16 路，2 bank（HF 节点）× 2 HNF |
| 时钟域 | 全系统单时钟 `io.clock`，无异步桥 | 同左 |

### 3.2 L2→L3 协议：CHI

- 协议为 **CHI（Issue E.b）**，边界 bundle：`XSCache/src/main/scala/xscache/chi/LinkLayer.scala:92` 的 `PortIO`（tx: req/rsp/dat；rx: rsp/dat/snp；加 linkactive/sysco 链路管理，L-credit 流控）
- 精确切分点：**`Top.scala:492-537`**。每核 `core.module.io.chi` 经 `TargetBinder` 地址分流：`0x0–0x7fffffff` 走低地址 MMIO（每核一个 mmioBridge NCB），其余进 L3
- L3→内存：OpenLLC `sn` 口 → OpenNCB → AXI4 → `soc_xbar` → XSTop 顶层 `memory` 口（256-bit AXI4）
- SimTop 仿真内存：`SimTop.scala:114-127`，`memory` 口接 `AXI4MemorySlave`（BlackBox RAM，difftest C++ 侧提供），**与 L3 实现无关，替换 L3 不影响仿真内存模型**

### 3.3 关键发现：`--external-llc` 官方外挂接口 ⭐

`src/main/scala/top/ExternalLLC.scala:228` 定义了 BlackBox **`ExternalLLCWrapper`**。开启 `--external-llc` 后整个 OpenLLC 被替换，Verilog 中露出待集成方实现的模块接口：

```scala
class ExternalLLCWrapper extends BlackBox {
  val io = IO(new Bundle {
    val clock = Input(Clock()); val reset = Input(Bool())
    val rn      = Vec(NumCores, Flipped(new PortIO))  // CHI RN 口，接各 tile 的 L2
    val rnNodeId = Output(Vec(NumCores, UInt(11.W)))
    val ddrc    = new VerilogAXI4Record(...)  // AXI4 master → 内存（经 soc_xbar 到顶层 memory 口）
    val peri    = new VerilogAXI4Record(...)  // AXI4 master → 外设
    val imsic   = new VerilogAXI4Record(...)  // AXI4 master
  })
}
```

- 使用点：`Top.scala:130-133`（实例化，替代 OpenLLC）、`Top.scala:492-502`（CHI 连接）、`ExternalLLC.scala`（LazyModule 包装）
- 限制：**仅兼容 OpenLLC 模式、NumCores ≤ 2**；仿真外设总线多出 BootSram（`0x37f00000` 起 1MB）/Control（`0x20000000–0x2fffffff`）地址空间约定（`src/test/scala/top/SimMMIO.scala:63-65`）
- AXI4 侧拍平为标准 `awvalid/awid/awaddr/...` 信号（`utils/VerilogAXI4Record.scala`），DDRC 侧 ID 宽 14、内部 6

**这是 wolvicmod L3 模型最自然的落点**：wolvicmod 建 CHI RN 口的 L3 行为模型，DPI-C 桥接到 `ExternalLLCWrapper` 端口。

### 3.4 备选替换点

直接在 `Top.scala:492-537` 的 CHI 连接点替换 `chi_openllc_opt` 为自己的 Chisel/BlackBox 模块（`rn`/`sn` 口定义见 `XSCache/src/main/scala/openLLC/OpenLLC.scala:37-46`），内存侧复用 OpenNCB 桥或直发 AXI4 到 `misc.soc_xbar`。

---

## 4. ZhuJiang ↔ XiangShan 接口细节

### 4.1 接口定义：`DecoupledPortIO`

定义于 `XSCache/src/main/scala/xscache/chi/LinkLayer.scala:99-102`：

```scala
class DecoupledPortIO extends Bundle {
  val tx = new DecoupledDownwardsLinkIO          // req/rsp/dat，均为 DecoupledIO
  val rx = Flipped(new DecoupledUpwardsLinkIO)   // rsp/dat/snp，均为 DecoupledIO
}
```

- 每通道是 **DecoupledIO（valid/ready + 结构化 flit Bundle）**，不是标准 CHI 的 `flitpend/flitv/flit/lcrdv` 四信号链路层
- **没有** L-credit、**没有** linkactive/sysco 链路状态机——永远 RUN 态。比标准 CHI 接口明显好建模
- flit Bundle：`CHIREQ/CHISNP/CHIDAT/CHIRSP` 见 `xscache/chi/Message.scala:428/481/511/557`

**连接机制**（`Top.scala:552` → `XSCache/src/test/scala/ZhuJiangBridge.scala:76-150`，注意在 test 目录，靠 test classpath 编入）：

1. 六条通道逐一 valid/ready 对接
2. **字段级 flit 重映射**：`xscache.chi.CHIREQ/...` ↔ `zhujiang.chi.ReqFlit/...`（两侧是两套独立 Bundle 定义）
3. require 校验仅 4 条：CC 节点类型、`socket=="sync"`、无 DataCheck、无 Poison

**nodeID 机制**：tile 侧 nodeID 在 ZhuJiang 模式下不进 flit（coupledL2 decoupled 路径 SrcID 恒为 0）；真正的 SrcID 由 CC 路由器注入环时盖章（`xijiang/router/base/BaseRouter.scala:208-210`），仅保留设备侧 aid（低 3 位）。回程路由靠 TgtID。

### 4.2 参数对齐方式：无协商，配置约定 + 硬编码

两侧两套独立常量定义，对接处无位宽 require，一致性靠编译期配置锁死：

| 参数 | 值 | 对齐机制 |
|---|---|---|
| CHI Issue | E.b | `SoC.scala:164` 强制，xscache 侧查表锁定 nodeID=11、TXNID=12 |
| nodeID | 11 位（8 nid + 3 aid） | `ZhuJiangNoCTopology.scala:17-18` 硬编码 |
| 地址 / 数据 | 48 位 / 256 位 | 两侧各自硬编码 |
| DataCheck / Poison | 关闭 | `L2Top.scala:130` + `Top.scala:112` 强制关 |
| ⚠️ DAT.DBID | zhujiang 侧 **16 位** vs xscache 侧 12 位 | 不对齐！桥里靠 Chisel 隐式零扩展混过去（`ZhuJiangBridge.scala:214,232`） |

L2 行为约束：`bufferableNC=false, endpointOrderNC=true, enableL2Flush=false`（`Configs.scala:415-423`）。
ZhuJiangParams 独立于 OpenLLCParams；DefaultConfig 各配一份 32MB/16 路，`--l3-cache-size` 两者同改（`ArgParser.scala:197-210`）。

### 4.3 实现语言

- **ZhuJiang 纯 Chisel**：`XSCache/ZhuJiang/src/main` 下 105 个 .scala，零 Verilog。三包结构：`zhujiang/`（顶层+参数）、`xijiang/`（Ring 路由器）、`dongjiang/`（HNF，LLC slice 所在）
- 无外部 Verilog BlackBox（仅测试设施含 DPI，集成时关闭）；SRAM 用全芯片统一的 Chisel SRAM 模板
- coupledL2 的 CHI 口为**原生实现**（非 TL→CHI 转换桥）：slice/MSHR 直接产生 CHI 语义通道；OpenLLC 模式多过一层 `LinkMonitor` 做 decoupled→L-credit 封装（`CoupledL2.scala:889-904`），ZhuJiang 模式直接引出

### 4.4 在 XiangShan 外实现 ZhuJiang 兼容模块的硬约束清单

1. CHI E.b flit 格式（`zhujiang/chi/Flit.scala:27-126` 的字段布局）
2. nodeIdBits = 11（8 nid + 3 aid）、TxnID=12、RSP.DBID=12、**DAT.DBID=16**、地址 48 位、数据 256 位、无 DataCheck/Poison
3. socket 类型 `"sync"`，纯 valid/ready，无 L-credit/链路状态机
4. SrcID 由 NoC 侧注入时盖章，设备侧 flit SrcID 应为 0（aid 位保留）
5. L2 侧行为约定：`bufferableNC=false, endpointOrderNC=true, enableL2Flush=false`

---

## 5. 技术路线选型参考

| 路线 | 接口 | 建模难点 | 优势 |
|---|---|---|---|
| A：`--external-llc` + ExternalLLCWrapper | 标准 CHI E.b `PortIO`（L-credit + 链路状态机） | L-credit 流控、linkactive/sysco 状态机 | 官方预留接口，改动最小；标准协议文档完备 |
| B：替换 ZhuJiang | `DecoupledPortIO`（纯 valid/ready） | zhujiang 自定义 flit 格式（DBID 16 位等坑）；分布式 HNF 语义 | 接口简单，无链路层状态机 |

两条路线的 L3 模型都必须处理：多 bank 交织（bit[6] 起 bank bits）、snoop filter/目录一致性（单核 Exclusive、多核 Non-inclusive）、与 difftest 的协同（仿真内存模型不变）。
