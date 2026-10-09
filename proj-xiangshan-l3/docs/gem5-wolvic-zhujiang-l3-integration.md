# XiangShan gem5 集成 Wolvic ZhuJiang L3

本文说明如何从一个全新的 `wolvicmod-playground` checkout 构建并运行当前
集成：单核 kmhv3、XiangShan `L2CacheWrapper`、Wolvic ZhuJiang LLC，以及
gem5 `MemCtrl + DDR4_2400_8x8`。CoreMark 使用项目锁定的 NEMU 做 difftest；
Linux 使用功能启动路径验证，因为当前 gem5/NEMU 在 OpenSBI 的 `menvcfg` CSR
语义上存在差异。

本文中的 `build/` 只表示本地生成目录。它被 `.gitignore` 忽略，不是验收文档
的一部分；验证以可重复的命令和终端输出为准，运行生成物不作为文档证据。

## 代码版本前提

父仓库必须记录一个包含 Wolvic gem5 适配的 `xs-gem5` 子模块提交。该提交至少
应包含以下路径：

```text
xs-gem5/src/mem/wolvic_zj/
xs-gem5/configs/example/kmhv3.py
xs-gem5/configs/common/CacheConfig.py
xs-gem5/configs/common/Options.py
xs-gem5/configs/common/xiangshan.py
cpp/wolvic_zj_model.cpp
cpp/wolvic_zj_model.h
```

全新 checkout 后先执行下面的检查；如果文件不存在，说明当前父仓库记录的
`xs-gem5` gitlink 还没有指向集成提交，需要先更新子模块版本，不能继续运行：

```bash
test -f xs-gem5/src/mem/wolvic_zj/WolvicZjL3.py
test -f xs-gem5/configs/example/kmhv3.py
test -f cpp/wolvic_zj_model.cpp
```

这项检查是为了避免把“只有上游 GEM5、没有 Wolvic 适配代码”的 checkout 误认为
可运行版本。

## 端到端准备

以下命令从仓库根目录开始。`--recurse-submodules` 会初始化顶层 `wolvicmod`、
`XiangShan`、`xs-gem5` 以及 XiangShan 使用的嵌套子模块；已有 checkout 可以
直接执行最后一条更新命令。

```bash
git clone --recurse-submodules <wolvicmod-playground-url> wolvicmod-playground
cd wolvicmod-playground
git submodule update --init --recursive
cd proj-xiangshan-l3
```

gem5 和项目模型的构建依赖（Ubuntu 24.04）如下：

```bash
sudo apt update
sudo apt install build-essential git cmake ninja-build m4 scons \
  zlib1g zlib1g-dev libprotobuf-dev protobuf-compiler libprotoc-dev \
  libgoogle-perftools-dev python3-dev libboost-all-dev pkg-config \
  libsqlite3-dev zstd libzstd-dev
```

如果还要重新生成 XiangShan RTL 或 `ready-to-run` workload，需要额外安装
OpenJDK 21 和 mill；本文的 gem5 端到端流程直接使用子模块中已经提供的
`XiangShan/ready-to-run/coremark-2-iteration.bin` 和 `linux.bin`，不需要重新
生成 RTL。

## 构建 Wolvic 模型和 gem5

先运行项目侧模型测试，确认顶层 `wolvicmod` 子模块和 C++20 模型接口完整：

```bash
cd proj-xiangshan-l3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)" --target wolviczj_cpp test_wolvic_zj_model
ctest --test-dir build -R '^test_wolvic_zj_model$' --output-on-failure
```

gem5 的 SCons 构建会直接把 `cpp/` 和 `model/` 源文件编译进 `gem5.opt`；模型
源文件只使用 C++20，gem5 其余代码仍使用仓库默认的 C++17：

```bash
make gem5-build
test -x xs-gem5/build/RISCV/gem5.opt
```

所有构建缓存和产物都留在 `proj-xiangshan-l3/build/` 或
`proj-xiangshan-l3/xs-gem5/build/`，不会写回模型源代码目录。

## 构建项目锁定的 NEMU reference

CoreMark 的 difftest 必须使用 `xs-gem5/util/nemu_ref/lock.json` 对应的 NEMU，
不能直接复用 XiangShan `ready-to-run/` 中给 RTL 使用的 `.so`。下面的命令把
NEMU 源码、锁定依赖、编译中间文件和发布文件全部放在项目内。

```bash
cd proj-xiangshan-l3
mkdir -p build/nemu/sources

git clone https://github.com/OpenXiangShan/NEMU.git \
  build/nemu/sources/NEMU-gem5
git -C build/nemu/sources/NEMU-gem5 \
  checkout --detach d30fff1ece9e0480146caf504660a0239b994eef

git clone https://github.com/ucb-bar/berkeley-softfloat-3.git \
  build/nemu/sources/NEMU-gem5/resource/softfloat/repo
git clone https://github.com/OpenXiangShan/LibCheckpointAlpha.git \
  build/nemu/sources/NEMU-gem5/resource/gcpt_restore
git clone https://github.com/OpenXiangShan/LibCheckpoint.git \
  build/nemu/sources/NEMU-gem5/resource/LibCheckpoint
git clone https://github.com/nanopb/nanopb.git \
  build/nemu/sources/NEMU-gem5/resource/nanopb

git -C build/nemu/sources/NEMU-gem5/resource/softfloat/repo \
  checkout --detach b51ef8f3201669b2288104c28546fc72532a1ea4
git -C build/nemu/sources/NEMU-gem5/resource/gcpt_restore \
  checkout --detach c5c2fef74133fb2b8ef8642633f60e0996493f29
git -C build/nemu/sources/NEMU-gem5/resource/LibCheckpoint \
  checkout --detach 2115c400af664ba06c57114db39a777ee7e5f1d0
git -C build/nemu/sources/NEMU-gem5/resource/nanopb \
  checkout --detach 5b070b65ae53f72c5e9b5050519933bcd4cc0eff

mkdir -p build/cache/ccache build/tmp
env CCACHE_DIR="$PWD/build/cache/ccache" TMPDIR="$PWD/build/tmp" \
  python3 xs-gem5/util/nemu_ref/build.py \
  --source build/nemu/sources/NEMU-gem5 \
  --output build/nemu/releases/d30fff1ece9e-gem5-r3-multi16g-zfa-cbo \
  --variant normal --jobs "$(nproc)"

GEM5_NEMU_REF="$(python3 xs-gem5/util/nemu_ref/resolve.py normal \
  --root build/nemu/releases)"
printf 'NEMU reference: %s\n' "$GEM5_NEMU_REF"
```

`resolve.py` 会验证 NEMU commit、配置片段、依赖 commit、寄存器布局和 `.so`
校验值；最后应打印项目内 `build/nemu/releases/.../normal/` 下的
`riscv64-nemu-interpreter-so` 路径。重复构建时请使用新的输出目录，因为构建
脚本不会覆盖已有 release。

## CoreMark：从构建到完整运行

在 `proj-xiangshan-l3/` 下执行。若上一节的变量已经离开 shell，重新解析一次：

```bash
cd proj-xiangshan-l3
GEM5_NEMU_REF="$(python3 xs-gem5/util/nemu_ref/resolve.py normal \
  --root build/nemu/releases)"
GEM5_NEMU_REF="$GEM5_NEMU_REF" make wolvic-coremark
```

`make wolvic-coremark` 会依次确保 `gem5.opt` 已构建，然后调用项目内的
`run_wolvic_zj_stage7_tests.py --only=coremark`。它固定使用：

```text
XiangShan kmhv3 / num_cpus=1
L2CacheWrapper / classic_l2=false
WolvicZjL3 / --wolvic-zj
MemCtrl / DDR4_2400_8x8
XiangShan/ready-to-run/coremark-2-iteration.bin
项目锁定的 normal NEMU reference
```

成功运行的终端和 `run.log` 应包含以下特征：

```text
CoreMark 1.0
seedcrc : 0xe9f5
crclist : 0xe714
crcmatrix : 0x1fd7
crcstate : 0x8e3a
crcfinal : 0x72be
m5_exit instruction encountered
coremark: PASS
```

运行目录默认是 `build/runs/gem5-stage7-coremark/`，仅用于本地日志、统计和
调试；删除它不会影响下一次运行。若只想先验证连线和短跑，可执行：

```bash
GEM5_NEMU_REF="$GEM5_NEMU_REF" make wolvic-coremark-short
```

## Linux：功能启动到 `/init`

Linux 当前不使用在线 NEMU difftest。原因是 OpenSBI 在执行
`csrrs a0, menvcfg, zero` 时，项目锁定 NEMU 返回 `0x80000000000000f0`，而
当前 gem5 CSR 实现返回 `0`；这发生在进入 Linux 用户态之前，与 Wolvic LLC 的
CHI/AXI/DDR4 数据通路无关。因此 Linux 验收显式关闭 difftest：

```bash
cd proj-xiangshan-l3
make gem5-build
RUN_DIR="$PWD/build/runs/gem5-linux-wolvic-nodiff"
mkdir -p "$RUN_DIR"
timeout 1800s xs-gem5/build/RISCV/gem5.opt \
  --outdir="$RUN_DIR" \
  xs-gem5/configs/example/kmhv3.py \
  --disable-difftest \
  --raw-cpt \
  --generic-rv-cpt="$PWD/XiangShan/ready-to-run/linux.bin" \
  --num-cpus=1 \
  --mem-type=DDR4_2400_8x8 \
  --wolvic-zj \
  > "$RUN_DIR/run.log" 2>&1
```

启动成功的判断只依赖日志中的功能标志：

```bash
grep -E \
  'OpenSBI v1\.4|Linux version 6\.1\.83|Run /init as init process|Hello, XiangShan!|m5_exit' \
  "$RUN_DIR/run.log"
```

应看到 OpenSBI、Linux 6.1.83、`Run /init as init process`、
`Hello, XiangShan!` 和 `m5_exit`。这条命令同样把 `config.ini`、`stats.txt` 和
运行日志留在项目内，但这些文件是本地生成物，不需要提交到仓库。

## 集成拓扑

启用 `--wolvic-zj` 后，缓存和内存连接为：

```text
XiangShan kmhv3
    │
    ├── L1 I/D cache
    │       │
    │   L1ToL2Bus
    │       │
    └── XiangShan L2CacheWrapper
            │
        L2ToL3Bus (tol3bus)
            │                         ┌── MMIO Bridge ── UART/PLIC/CLINT
            ├── WolvicZjL3 ────────────┤
            │       │                  └── iobus
            │       │
            │   gem5 membus
            │       │
            │   MemCtrl + DDR4_2400_8x8
            │
            └── MMIO requests bypass Wolvic through tol3bus
```

实际端口关系是：

```text
L2CacheWrapper.xbar.mem_side
    → tol3bus.cpu_side
    → tol3bus.mem_side
    → WolvicZjL3.cpu_side
    → WolvicZjL3.mem_side
    → membus.cpu_side_ports
    → DDR4 memory controller
```

UART、CLINT 和 PLIC 地址经 `tol3bus → wolvic_mmio_bridge → iobus` 转发，不进入
Wolvic 的 CHI/AXI 内存范围。

## 适配边界

Wolvic C++ 模型由 `cpp/wolvic_zj_model.{h,cpp}` 提供周期接口，gem5 适配层位于
`xs-gem5/src/mem/wolvic_zj/`。适配层接收 gem5 timing `Packet`，驱动 CHI，再由
AXI 桥把模型的 32B beat、32/64B burst 转换为 gem5 内存请求。

| gem5 请求 | CHI 行为 |
|---|---|
| `ReadReq`、`ReadCleanReq`、`ReadSharedReq`、`ReadExReq` | `ReadOnce`，两个 32B `CompData`，必要时 `CompAck` |
| `UpgradeReq`、`SCUpgradeReq` | `ReadUnique` 获取 ownership |
| `WriteReq` | `WriteUniquePtl`，按 byte-enable 发送两个数据 beat |
| `WritebackDirty`、`WritebackClean` | `ReadUnique + CompAck` 后发送 `WriteBackFull + CopyBackWrData` |
| `InvalidateReq`、`CleanEvict` | 在 gem5 侧完成，不访问 Wolvic 内存通道 |

当前上游 gem5 Packet 一次只保留一个未完成事务；AXI 侧仍可有多个 outstanding
burst。gem5 `MemCtrl + DDR4_2400_8x8` 提供实际内存延迟，AXI 桥负责握手、retry、
ID、beat 和返回顺序。

## 固定配置和限制

`--wolvic-zj` 强制以下条件：

```text
num_cpus       == 1
ruby           == false
classic_l2     == false
l2cache        == true
l3cache        == true
cacheline_size == 64
ci             == 0
```

当前路径使用 XiangShan aligned `L2CacheWrapper`，不使用 Ruby `RISCV_CHI`
endpoint。多核 snoop、共享目录和完整多核一致性、cfgAXI、DVM、DataCheck、Poison、
Garnet 与 Wolvic Ring 的统一建模仍未接入。Wolvic 内部 Ring、HNF directory、SRAM、
CHI/AXI transaction state 目前不支持中途 checkpoint 保存和恢复。

不支持跨 64B cache line、uncacheable/secure、atomic/functional port、DVM、普通
snoop 以及未实现 CHI 命令；遇到这些情况会明确报错。事务和 AXI 默认有
`watchdog_cycles=100000` 的超时保护。

## 相关入口

- 项目总览和原有 RTL 对比流程：`proj-xiangshan-l3/README.md`
- 项目构建和回归入口：`proj-xiangshan-l3/Makefile`
- GEM5 专用 NEMU 锁定、构建与 ABI 校验：`docs/gem5-nemu-build.md`
- Wolvic C++ 周期接口：`docs/gem5-wolvic-cpp-interface.md`
- Wolvic ZhuJiang 与 XiangShan RTL 集成背景：`docs/xiangshan-wolvicmod-l3-integration.md`
