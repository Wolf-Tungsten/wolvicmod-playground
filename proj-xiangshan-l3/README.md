# proj-xiangshan-l3：XiangShan ZhuJiang L3 的 wolvicmod 建模与对比

用 wolvicmod 建模 XiangShan（昆明湖 V3）的 ZhuJiang L3，与真实 RTL 做
行为/性能对比。两条对比路径：

## ① 孤立 L3 回放（wolvic vs verilated RTL，同一 coremark trace）

按顺序三步：**造 RTL → 造 trace → 跑回放**。

### 第 1 步：生成回放用 RTL（一次性）

```bash
make -C proj-xiangshan-l3 zjrtl-rtl   # mill 生成 ZhujiangReplayTop（与
                                      # 整机内 zhujiang_opt 同配置，端口即三边界）
```

### 第 2 步：生成 trace（`build/trace/cm_full.txt`）

trace 来自**整机 RTL emu 跑 coremark 时抓的波形**：

```bash
make -C proj-xiangshan-l3 emu TRACE=fst   # 重建带 FST 波形的 emu（只重 verilate，~23min）
make -C proj-xiangshan-l3 replay-top      # 抓全程波形 → 提取 trace → 自动校验
```

`replay-top` 内部三步：

1. emu 跑 coremark 全程并 dump FST 到 `build/trace/cm_full.fst`
   （`-b 0 -e 400000 -C 400000 --dump-wave`；coremark ~31.7 万拍自然
   HIT GOOD TRAP，FULLN 只是防死循环上限）；
2. `verify/trace/extract_top_trace.cpp`（C++ libfst 直读 FST）提取
   WolvicZjTop 三边界信号——L2 CHI 缝六通道 + mem AXI + cfg AXI——
   重建逐拍文本 trace（316,748 拍 × 177 列）；
3. 自动 `ctest -R wolvic_top_replay`：用新 trace 回放 wolvic 模型逐拍
   比对，验证 trace 可用。

注意：`--dump-wave` 只在 `-b/-e` 窗口内 dump；trace 里 bits 仅在
valid=1 时有意义（difftest 开 `RANDOMIZE_REG_INIT`，valid=0 时 RTL 的
bits 是随机初值，比对器按 don't-care 处理）。**RTL 侧行为变化后必须重
做本步**；trace 已随库提供，日常无需再生。

### 第 3 步：跑回放

```bash
make -C proj-xiangshan-l3 zjrtl-replay DUT=both    # 等价性对拍（逐拍三边界交叉验证）
make -C proj-xiangshan-l3 zjrtl-replay DUT=wolvic  # DUT=rtl|wolvic：单侧孤立计时
```

`REPLAY_AUDIT=1 <二进制>` 开读集审计。

前端译码级的 P2 回放用另一条 trace：`make replay [N=20000]`（emu dump
前 N 拍 FST → `fst2vcd | verify/trace/extract_cc_trace.py` →
`build/trace/cc_front.txt` → `ctest -R test_trace_replay`）。

## ② 整机 emu（XiangShan + wolvic L3 vs XiangShan + RTL L3，difftest）

```bash
make -C proj-xiangshan-l3 emu              # RTL ZhuJiang L3 版（8 线程）
make -C proj-xiangshan-l3 emu WOLVIC=1     # wolvicmod L3 版（8 线程，同线程可比）
make -C proj-xiangshan-l3 coremark         # 跑当前 emu 的 coremark + difftest
make -C proj-xiangshan-l3 stash-emu NAME=x # 留存二进制到 build/emu-variants/ 供对比
```

切换配置由 `.llc-config` 印记自动 clean 重建；当前对比结果（全程 coremark
+ difftest，2026-09-29）：**wolvic 272.9s vs RTL 297.6s**，cycleCnt/IPC
逐值相等（docs/perf-breakdown.md §28）。

## 其它

- `make -C proj-xiangshan-l3 test`：单测回归网（ctest 17 条）
- 环境搭建、L3 边界探查、历史基线：`docs/xiangshan-l3-notes.md`
- 性能优化全记录：`docs/perf-breakdown.md`
