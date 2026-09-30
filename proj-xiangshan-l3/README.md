# proj-xiangshan-l3：XiangShan ZhuJiang L3 的 wolvicmod 建模与对比

用 wolvicmod 建模 XiangShan（昆明湖 V3）的 ZhuJiang L3，与真实 RTL 做
行为/性能对比。

**所有 make 命令都在 `proj-xiangshan-l3/` 目录下执行**（playground 根目
录没有 Makefile；从根目录用 `make -C proj-xiangshan-l3 ...` 等价）。下
文一律写 `make xxx`。

两条对比路径，区别只在 RTL 侧的规模和激励来源：

| | 路径①：L3 隔离回放 | 路径②：SoC 仿真 |
|---|---|---|
| RTL 侧 | **独立 ZhuJiang L3**（`ZhujiangReplayTop`：只有 L3 模块 + 边界 socket/remap，**不含**香山核/L2/总线/外设） | **XiangShan SoC 仿真器**（difftest emu：香山核 + L2 + ZhuJiang L3 + 仿真内存与外设） |
| 激励 | 预先抓好的 trace 文件回放（秒级迭代） | 真跑 coremark 二进制 + difftest 在线比对（分钟级） |

## ① L3 隔离回放（wolvic L3 vs 独立 ZhuJiang L3 RTL，同一 trace）

按顺序三步：**造 RTL → 造 trace → 跑回放**。trace 与 RTL 都不随库
（`build/` 已入 .gitignore），删除全部 build 目录后按本流程一次跑通。

### 第 1 步：生成回放用 RTL

```bash
make replay-rtl   # mill 生成 ZhujiangReplayTop：独立的 ZhuJiang L3 RTL，
                  # 与 SoC 内例化的 zhujiang_opt 同参同源（XSCache/ZhuJiang
                  # 子模块），端口即三边界 → build/zjrtl/rtl/
```

### 第 2 步：生成 trace（`build/trace/cm_full.txt`）

trace 来自 **XiangShan SoC 仿真器（RTL 版）跑 coremark 时抓的 L3 边界
波形**——先在 SoC 里录下 L3 的输入输出，再拿到路径① 里回放：

```bash
make emu TRACE=fst   # 构建带 FST 波形的 RTL SoC emu（首次全量 ~25-35min）
make trace           # 抓全程波形 → 提取 L3 三边界 trace → 自动校验
```

`trace` 内部三步：

1. SoC emu 跑 coremark 全程并 dump FST 到 `build/trace/cm_full.fst`
   （`-b 0 -e 400000 -C 400000 --dump-wave`；coremark ~31.7 万拍自然
   HIT GOOD TRAP，FULLN 只是防死循环上限）；
2. `verify/trace/extract_top_trace.cpp`（C++ libfst 直读 FST）提取
   L3 三边界信号——L2 CHI 缝六通道 + mem AXI + cfg AXI——
   重建逐拍文本 trace（316,748 拍 × 177 列）；
3. 自动 `ctest -R wolvic_top_replay`：用新 trace 回放 wolvic 模型逐拍
   比对，验证 trace 可用。

注意：`--dump-wave` 只在 `-b/-e` 窗口内 dump；trace 里 bits 仅在
valid=1 时有意义（difftest 开 `RANDOMIZE_REG_INIT`，valid=0 时 RTL 的
bits 是随机初值，比对器按 don't-care 处理）。**RTL 侧行为变化后必须重
做本步**。

### 第 3 步：跑回放

同一份 trace 同时喂给独立 L3 RTL 和 wolvic L3 模型，逐拍对拍：

```bash
make replay DUT=both    # 等价性对拍（逐拍三边界交叉验证）
make replay DUT=wolvic  # wolvic单侧孤立计时
make replay DUT=rtl     # rtl单侧孤立计时
```

`REPLAY_AUDIT=1 <二进制>` 开读集审计。计时口径：钉核
（`taskset -c 2 <二进制> --dut=wolvic`）取两次最优；并先确认模型库
是 Release 构建（`grep CMAKE_BUILD_TYPE build/CMakeCache.txt`，空值
= -O0，会慢 ~9 倍且功能照样全绿——从零构建后必查，见
docs/perf-breakdown.md §28 构建教训二）。

前端译码级（XscChiAdapter+CcSocket）还有一条更细粒度的回放：
`make trace-front [N=20000]`（emu dump 前 N 拍 FST →
`fst2vcd | verify/trace/extract_cc_trace.py` →
`build/trace/cc_front.txt` → `ctest -R test_trace_replay`）。

## ② SoC 仿真（XiangShan SoC + wolvic L3 vs XiangShan SoC + RTL L3）

difftest 仿真器，差别只在 L3 由谁实现——RTL 还是 wolvicmod 模型
（chisel BlackBox `WolvicZjBB` + DPI 替换）：

```bash
make emu              # RTL ZhuJiang L3 版（8 线程）
make emu WOLVIC=1     # wolvicmod L3 版（8 线程，同线程可比）
make coremark         # 跑当前 emu 的 coremark + difftest
make stash-emu NAME=x # 留存二进制到 build/emu-variants/ 供对比
```

切换配置由 `.llc-config` 印记自动 clean 重建；当前对比结果（全程 coremark
+ difftest，2026-09-29）：**wolvic 272.9s vs RTL 297.6s**，cycleCnt/IPC
逐值相等（docs/perf-breakdown.md §28）。

## 依赖

Verilator 5.047、mill（launcher 自动钉 0.12.17）、OpenJDK 21、
`libsqlite3-dev`（emu 需要）、zlib/zstd。子仓库版本见下表。

## 子仓库版本

| 路径 | 分支 / 基线 | 提交 | 说明 |
|---|---|---|---|
| `wolvicmod/` | `main`（Wolf-Tungsten/wolvicmod） | 随主仓演进 | 框架仓，git submodule |
| `proj-xiangshan-l3/XiangShan/` | `wolvicmod-l3`（fork 自上游 `kunminghu-v3`，基线 `aa6b520`） | `026aaad30` | 我们的改动：BlackBox 集成（WolvicZjBB）+ ZhujiangReplayTop |
| `XiangShan/difftest/` | `wolvicmod-l3` | `4cd9de795` | 我们的改动：USER_CXXFILES/CXXFLAGS/LDFLAGS 透传钩子 |
| `XiangShan/XSCache/` |  detached（上游 `addperfevent` 系） | `300515b` | 纯上游，未改 |
| `XSCache/ZhuJiang/` | `master` | `dfcf696` | L3 本体源码（RTL 侧基准），纯上游 |
| `XSCache/OpenNCB/` | `master` | `9a83eb7` | 纯上游 |
| `XiangShan/ready-to-run/` | 上游 `nemu-ci-workloads` 系 | `4cf9983` | coremark 二进制 + nemu ref |

其余 XiangShan 嵌套子模块（rocket-chip/utility/yunsuan/ChiselAIA/
ChiselIOPMP）为上游钉版，`make init` 后勿动；`XSCache` 内嵌套的
rocket-chip/utility 不需要初始化（顶层自有副本）。

## 其它

- `make test`：单测回归网（ctest 17 条）
- 环境搭建、L3 边界探查、历史基线：`docs/xiangshan-l3-notes.md`
- 性能优化全记录：`docs/perf-breakdown.md`
