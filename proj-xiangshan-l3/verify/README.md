# proj-xiangshan-l3/verify/ —— 验证基建

两条端到端测试路径（2026-09-29 清理后只保留这两条；预制菜 RTL 对拍
基建 cosim/refgen/run.sh 已删除，历史结论见 ../docs/verification-report.md
§2 验证记录）：

## 路径 1：L3 隔离回放（wolvic L3 vs 独立 ZhuJiang L3 RTL）

共栖 A/B 回放器：`zjrtl/ab_replay.cpp` 同一二进制链接两侧——verilated
**独立 ZhuJiang L3 RTL**（`ZhujiangReplayTop`：只有 L3 模块，不含香山
核与 SoC）与 WolvicZjTop 裸模型，`--dut=rtl|wolvic|both`
选择激活侧，对 `build/trace/cm_full.txt`（coremark 全程，316,748 拍）
逐拍比对三边界。

```bash
make replay-rtl            # 生成 ZhujiangReplayTop RTL（mill，一次性）
make replay DUT=both       # 对拍；DUT=rtl|wolvic 为孤立性能剖析
```

trace 再生（RTL 侧行为变化后必须重做）的完整步骤与注意事项见
`../README.md` 路径①的"生成 trace"一节（`make emu TRACE=fst` →
`make trace`：XiangShan SoC 仿真器全程 dump FST →
`trace/extract_top_trace.cpp` C++ libfst 直读 → cm_full.txt → 自动
`ctest -R wolvic_top_replay` 校验）。前端译码级回放（P2）：
`make trace-front`（→ `extract_cc_trace.py` → ctest -R trace_replay）。

## 路径 2：XiangShan SoC 仿真（wolvic L3 vs RTL L3，difftest）

```bash
make emu LLC=ZhuJiang ET=8     # RTL L3 版
make emu WOLVIC=1 ET=8         # wolvic L3 版（BlackBox + DPI 薄壳）
make coremark                  # 用现有 emu 跑 coremark + difftest
make stash-emu NAME=<变体名>   # 留存二进制到 build/emu-variants/ 供对比
```

`dpi/` 即 wolvic 版的 DPI-C glue（`wolvic_zj_step`：set 输入 → 沉定 eval
→ clk 0→1 eval 提交 → 读输出 → clk 拉回 0；守卫预过滤要求
settle-then-pulse，见框架规划 §5.2）。指纹校验与切换防呆见
../docs/verification-report.md §3.4。

## 目录

```
verify/
├── dpi/       # wolvic L3 emu 的 DPI-C glue + SV 薄壳
├── trace/     # trace 提取器（extract_top_trace.cpp / extract_cc_trace.py / fst_probe.cpp）
└── zjrtl/     # 共栖 A/B 回放器（路径 1）
```

生成物全部在 `proj-xiangshan-l3/build/` 下（已入 .gitignore）。
