# proj-xiangshan-l3：XiangShan ZhuJiang L3 的 wolvicmod 建模与对比

用 wolvicmod 建模 XiangShan（昆明湖 V3）的 ZhuJiang L3，与真实 RTL 做
行为/性能对比。两条对比路径：

## ① 孤立 L3 回放（wolvic vs verilated RTL，同一 coremark trace）

```bash
make -C proj-xiangshan-l3 zjrtl-rtl      # 一次性：生成 ZhujiangReplayTop RTL
make -C proj-xiangshan-l3 zjrtl-replay DUT=both    # 等价性对拍（逐拍三边界）
make -C proj-xiangshan-l3 zjrtl-replay DUT=wolvic  # DUT=rtl|wolvic：孤立计时
```

trace（`build/trace/cm_full.txt`，coremark 全程 31.7 万拍）已在库内；RTL
行为变化后需 `make replay-top` 再生（需先用 `make emu TRACE=fst` 构建的
emu dump FST）。`REPLAY_AUDIT=1 <二进制>` 开读集审计。

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
- `make -C proj-xiangshan-l3 replay [N=20000]`：前端译码级 trace 回放（P2）
- 环境搭建、L3 边界探查、历史基线：`docs/xiangshan-l3-notes.md`
- 性能优化全记录：`docs/perf-breakdown.md`
