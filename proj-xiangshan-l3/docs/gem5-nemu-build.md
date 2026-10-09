# XS-GEM5 专用 NEMU：目录安排、构建与验证

## 项目内目录约定

构建产物、依赖源码、缓存和运行日志均放在 `proj-xiangshan-l3/` 内。沿用现有 GEM5 和 RTL 的构建目录，新增的 NEMU 与运行记录集中放在项目 `build/` 下：

```text
proj-xiangshan-l3/
├── build/                         # 已有 Wolvic CMake 构建目录；继续保留
│   ├── cache/ccache/              # 本项目编译缓存
│   ├── tmp/                       # 编译临时文件
│   ├── nemu/
│   │   ├── sources/NEMU-gem5/     # 独立 NEMU checkout，resource 下放锁定依赖
│   │   ├── releases/<release>/normal/
│   │   │   ├── source/            # git archive 导出的源码及编译中间文件
│   │   │   ├── build.log
│   │   │   ├── riscv64-nemu-interpreter-so
│   │   │   ├── riscv64-nemu-interpreter-so.config
│   │   │   ├── riscv64-nemu-interpreter-so.autoconf.h
│   │   │   └── manifest.json
│   │   ├── failed/                # 失败构建留档
│   │   └── archive/               # 排查过程产物留档
│   └── runs/<run-name>/           # 新增 GEM5 仿真日志、config、stats、验收记录
├── xs-gem5/
│   ├── build/RISCV/gem5.opt       # 沿用 GEM5 原生 SCons 构建路径
│   ├── m5out/kmhv3-baseline/      # 保留此前关闭 difftest 的基线
│   └── util/nemu_ref/             # GEM5 仓库自带 lock、配置片段和构建脚本
├── XiangShan/ready-to-run/
│   └── riscv64-nemu-interpreter-so # RTL 原有 reference，不覆盖
└── docs/
```

根仓库 `.gitignore` 中的 `build/` 规则已覆盖项目构建目录；GEM5 子模块也忽略自身的构建和运行产物。版本控制保留文档及源码锁定信息，二进制和缓存留在本机项目目录。

两套 NEMU 分 checkout、分配置、分产物，运行 GEM5 时显式传入 `--difftest-ref-so`。不修改全局 `NEMU_HOME`、`GCBV_REF_SO` 或 RTL 的脚本，因此 RTL 仍使用原有 reference。

## 锁定版本

唯一版本入口为 [xs-gem5/util/nemu_ref/lock.json](../xs-gem5/util/nemu_ref/lock.json)。本次使用：

- release：`d30fff1ece9e-gem5-r3-multi16g-zfa-cbo`；
- NEMU commit：`d30fff1ece9e0480146caf504660a0239b994eef`；
- 变体：`normal`，用于普通单核，memdedup 关闭；
- 配置：`riscv64-gem5-ref_defconfig` 加 `common.config`、`scalar.config`、`normal.config`。

release 名称包含其他变体的更新标签，本次只构建 `normal`，不需要因为名称中的 `multi16g` 改成多核或 16 GiB。

| NEMU 内路径 | 上游仓库 | 锁定 commit |
| --- | --- | --- |
| `resource/softfloat/repo` | `ucb-bar/berkeley-softfloat-3` | `b51ef8f3201669b2288104c28546fc72532a1ea4` |
| `resource/gcpt_restore` | `OpenXiangShan/LibCheckpointAlpha` | `c5c2fef74133fb2b8ef8642633f60e0996493f29` |
| `resource/LibCheckpoint` | `OpenXiangShan/LibCheckpoint` | `2115c400af664ba06c57114db39a777ee7e5f1d0` |
| `resource/nanopb` | `nanopb/nanopb` | `5b070b65ae53f72c5e9b5050519933bcd4cc0eff` |

这些 resource 依赖并非全部由 NEMU submodule 管理，仅执行 `git submodule update` 不足以准备构建。

## 从新 checkout 复现

以下准备命令用于上述源码目录尚不存在的情况；本机已经完成，无需再次 clone。在 `proj-xiangshan-l3/` 下运行：

```bash
mkdir -p build/nemu/sources
git clone https://github.com/OpenXiangShan/NEMU.git build/nemu/sources/NEMU-gem5
git -C build/nemu/sources/NEMU-gem5 checkout --detach d30fff1ece9e0480146caf504660a0239b994eef

git clone https://github.com/ucb-bar/berkeley-softfloat-3.git build/nemu/sources/NEMU-gem5/resource/softfloat/repo
git clone https://github.com/OpenXiangShan/LibCheckpointAlpha.git build/nemu/sources/NEMU-gem5/resource/gcpt_restore
git clone https://github.com/OpenXiangShan/LibCheckpoint.git build/nemu/sources/NEMU-gem5/resource/LibCheckpoint
git clone https://github.com/nanopb/nanopb.git build/nemu/sources/NEMU-gem5/resource/nanopb

git -C build/nemu/sources/NEMU-gem5/resource/softfloat/repo checkout --detach b51ef8f3201669b2288104c28546fc72532a1ea4
git -C build/nemu/sources/NEMU-gem5/resource/gcpt_restore checkout --detach c5c2fef74133fb2b8ef8642633f60e0996493f29
git -C build/nemu/sources/NEMU-gem5/resource/LibCheckpoint checkout --detach 2115c400af664ba06c57114db39a777ee7e5f1d0
git -C build/nemu/sources/NEMU-gem5/resource/nanopb checkout --detach 5b070b65ae53f72c5e9b5050519933bcd4cc0eff
```

构建也从 `proj-xiangshan-l3/` 开始，使用仓库自带脚本：

```bash
PROJECT_ROOT="$PWD"
mkdir -p build/cache/ccache build/tmp
env CCACHE_DIR="$PROJECT_ROOT/build/cache/ccache" \
    TMPDIR="$PROJECT_ROOT/build/tmp" \
    MAKEFLAGS='__NOT_DEFINED=' \
    python3 xs-gem5/util/nemu_ref/build.py \
      --source build/nemu/sources/NEMU-gem5 \
      --output build/nemu/releases/d30fff1ece9e-gem5-r3-multi16g-zfa-cbo \
      --variant normal --jobs 32

python3 xs-gem5/util/nemu_ref/resolve.py normal --root build/nemu/releases
```

`CCACHE_DIR` 与 `TMPDIR` 仅作用于此次构建；不依赖项目外缓存。`MAKEFLAGS='__NOT_DEFINED='` 用 GNU Make 命令行变量覆盖构建脚本设置的环境变量：该锁定 NEMU 的 `scripts/git.mk` 实际在 `__NOT_DEFINED` 非空时启用自动 Git 提交，置空后关闭。无需修改上游源码。

脚本拒绝覆盖已有 `normal/`。本机 release 已构建完成，重复构建应选新的输出根目录，并保留 `<release>/normal/` 两层结构，例如 `build/nemu/rebuilds/<run-name>/<release>/normal/`；调用 `resolve.py` 时相应调整 `--root`。源码或配置升级应跟随 GEM5 的 lock 更新。

脚本会检查配置项、编译共享库并验证 `DIFFTEST_REG_SIZE=1376`；`resolve.py` 再校验 manifest、配置片段和产物 SHA。仅寄存器区大小相同不足以保证所有 ABI 字段语义一致，因此必须使用配套的锁定配置并运行 difftest。

## GEM5 完整 CoreMark 验证

在 `proj-xiangshan-l3/xs-gem5/` 下运行。每次回归选择新的输出目录以保留历史记录：

```bash
GEM5_NEMU_REF="$(python3 util/nemu_ref/resolve.py normal --root ../build/nemu/releases)"
RUN_DIR=../build/runs/kmhv3-coremark-difftest-20261003
mkdir -p "$RUN_DIR"
env PYTHONDONTWRITEBYTECODE=1 TMPDIR="$PWD/../build/tmp" \
  timeout 180s ./build/RISCV/gem5.opt \
    --outdir="$RUN_DIR" \
    configs/example/kmhv3.py \
    --raw-cpt \
    --generic-rv-cpt=../XiangShan/ready-to-run/coremark-2-iteration.bin \
    --num-cpus=1 \
    --mem-type=DDR4_2400_8x8 \
    --difftest-ref-so="$GEM5_NEMU_REF" \
    > "$RUN_DIR/sim.log" 2>&1
```

这里未关闭 difftest，也没有指令上限；`timeout` 仅限制宿主机运行时间。`--difftest-ref-so` 优先于环境变量，因此不会误用 RTL reference。普通 `normal` 变体不加 `--enable-mem-dedup`。

2026-10-03 本机结果：

| 检查项 | 结果 |
| --- | --- |
| NEMU 编译器 | GCC/G++ 13.3.0，Ubuntu 24.04 |
| NEMU `.so` SHA256 | `51967fe76ac30db2996c21949405fff0c9843eeeace1b10a1d3f94f7f12ca6ad` |
| ABI / manifest | `DIFFTEST_REG_SIZE=1376`；`resolve.py` 校验通过 |
| GEM5 difftest | `enable_difftest=true`；日志确认 `FCSR comparison enabled` |
| 拓扑 | 单核 kmhv3，`L2CacheWrapper`，原有 gem5 L3，`MemCtrl` + `DDR4_2400_8x8` |
| CoreMark | 2 次迭代完整结束；`m5_exit`；进程返回 0 |
| 提交指令数 / tick | `663614` / `284238810` |
| CPU 周期 / IPC | `853571` / `0.777456` |
| DRAM 读取 | 200 次，共 12800 字节 |

提交指令数、tick、周期、IPC 和 DRAM 读取统计与此前关闭 difftest 的基线一致。RTL reference 的 SHA256 仍为 `9188e7dda30709962f0bfbc1dd87cea73d88a18c92c30892f93ed529c22def43`。

上述命令产生的日志、统计和 manifest 都属于本地 `build/` 目录，不纳入文档引用或版本控制。复现 Wolvic ZhuJiang LLC 的完整 CoreMark 和 Linux 流程，请按 [集成说明](gem5-wolvic-zhujiang-l3-integration.md) 执行；其中 CoreMark 会显式解析本节构建的 `normal` NEMU reference。
