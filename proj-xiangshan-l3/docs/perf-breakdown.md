# wolvicmod L3 版 emu 性能拆解（性能优化基线）

> 目的：记录 wolvicmod ZhuJiang L3 版 emu 相对纯 RTL 版的性能差距的**精确拆解**，
> 作为后续性能优化的基线与决策依据。所有数据同机 fresh 测量（32 核 / 186GB，
> Verilator 5.047，coremark-2-iteration + difftest）。
>
> 测量窗口说明：full = coremark 全程（316,801 拍）；50k = 前 5 万拍 boot 窗口
> （`-C 50000`，两侧同一窗口，可比）。

## 1. 速度基线总表

| 配置 | 线程 | 每拍 wall | 全程 host time | 备注 |
|---|---|---|---|---|
| RTL ZhuJiang | 8 | 1.00 ms | **316s** | full；两次独立复现 313s/316s |
| wolvicmod L3 | 16 | 2.67 ms | 876s | full；**wolvic 版最优点**（优化①前） |
| wolvicmod L3 | 8 | 3.83 ms | 1212s | full（优化①前） |
| wolvicmod L3 | 1 | 11.65 ms | ~3690s（推算） | 50k 实测 582.5s 推算（优化①前） |
| **wolvicmod L3（优化①后）** | 1 | **9.13 ms** | ~2890s（推算） | **50k 实测 456.3s**；dirty-eval + 去间接调用 |
| wolvicmod L3（优化①+②） | 1 | 9.04 ms | — | 50k 实测 452.0s/452.8s；边界拆分，收益噪声级（§7） |
| **wolvicmod L3（优化①+②+③）** | 1 | **8.93 ms** | — | **50k 实测 446.6s**；推送派发，IPC 0.842220（§8） |
| RTL ZhuJiang | 1 | 7.23 ms | ~2290s（推算） | 50k 实测 361.6s 推算 |

所有运行 cycleCnt 均为 316,801，指令流一致——差异纯粹是仿真宿主开销。

> **优化①结果（§6）**：wolvic 1T 11.65 → **9.13 ms/拍**，与 RTL 1T（7.23）
> 的差距从 4.42 收窄到 **1.90 ms/拍**（1.61× → 1.26×），符合 §4 方向①
> 原预期的 ~9 ms/拍——主贡献不是 dispatch 瘦身，而是脏驱动跳过调度
> （L3 模型每拍大部分逻辑静止，全被跳过）；模型侧 A/B 实测 2.87×。
> 剩余差距主要落边界结构桶（方向 2）。
>
> **优化③结果（§8）**：调度循环自身的轮询扫描反转成推送派发，模型侧再降
> 1.99×；emu 1T 9.04 → **8.93 ms/拍**，与 RTL 1T 的差距收窄到 **1.70 ms/拍**。

## 2. 核心拆解：1T 对 1T（无多线程同步干扰）

wolvic 版比纯 RTL 多出的部分 = 11.65 − 7.23 = **4.42 ms/拍**（1.61×）。
按 perf 符号归组 + 生成代码 zhujiang 信号归因（1569 个生成函数，覆盖 97.6% 采样）：

| 构成 | ms/拍 | 占多出部分 | 归属 |
|---|---|---|---|
| wolvicmod 模型 + DPI glue | ~2.5 | ~55% | **wolvicmod 框架可优化** |
| BlackBox 边界结构成本 | ~2.4 | ~55% | Verilator 侧 |
| （减去）省掉的 L3 RTL 工作 | −0.9 | — | RTL 版需付、wolvic 版省掉 |
| **净多出** | **~4.4** | | 对账：6.44+2.4+2.5≈11.9 ✓；6.44+0.9≈7.5 ✓ |

两侧共有的 SoC 基线 = **6.44 ms/拍**（RTL 侧 sequent 3.89 + comb 2.55，已扣除
zhujiang 归因份额 0.55+0.34）。

### 2.1 模型侧 2.5 ms/拍：框架调度粒度主导

- `wolvicmod::Module::eval()` 调度循环本身：**5.6%**（0.66 ms/拍）——遍历展平
  动作表 + 逐动作虚调用；
- 其余 ~15% 散布在**几千个** `AssignAction<T>::run()` / `UpdateAction<T>::run()` /
  `std::_Function_handler::_M_invoke`——每信号一个 action 对象、一次
  `std::function` 间接调用，单个 <0.4%，聚合是大头。
- 模型/glue 全部已是 **-O3**（CMAKE_BUILD_TYPE=Release，18 个源文件逐个核对）——
  优化空间在框架结构，不在编译选项。

### 2.2 边界侧 ~2.4 ms/拍：不是变量重组

- **变量重组实测免费**：in_pack 收集 + DPI 调用 + 输出拷贝（`nba_sequent__TOP__21`）
  仅 **0.17%**（~15µs/拍）；VlWide 包装、C++ 侧 pack/unpack 均 ~0.00%；
- 真实成本弥漫在两个桶里（相对 SoC 基线 sequent +1.55 / comb +0.82）：
  BlackBox 端口使 L2 侧供给逻辑（如 txdat 仲裁 mux）全部保持 live 无法死码消除、
  o_out NBA 更新后下游组合逻辑每拍整体重估（额外 settle 波次）。
- 注：zhujiang 归因按函数体是否引用 zhujiang 信号计，是**下界**（跨界内联的
  L3 逻辑不含该字样），故边界成本 ±0.5 ms 不确定度。

## 3. 多线程同步损耗（叠加上述固有差距）

wolvic 版 MT 扩展性差：8T→16T 仅 1.43×（RTL 8T 近线性 7.2×）。

- **证据**：16T profile 中 ~89% 采样在 16 个 `__Vthread__nba__s0__t*` 线程函数，
  annotate 到指令级后热点是两个 `pause` 自旋环（各 44%）——Verilator MTask
  依赖计数器轮询（5 万次空转后 `sched_yield`）；
- **机制**：DPI 调用点是每拍的全局收敛点——in_pack（1107b）从 L2/memAXI/cfgAXI
  各分区收集（依赖几乎所有生产者 MTask），o_out（1443b）下一拍到处被消费
  （几乎所有 MTask 依赖它）。每拍 MTask 图坍缩成
  `[全部生产者] → [DPI] → [全部消费者]` 串行波次，同步延迟主导墙钟；
- **对照**：RTL 8T 最热线程函数 pause 也占 98.2%——自旋是 Verilator MT 常态，
  区别在于 RTL 依赖图宽而浅、无全局汇聚点，同步损耗仅占墙钟 ~10%
  （wolvic 8T 为 ~62%、16T 为 ~73%）；
- **每拍 CPU 总量**：wolvic 16T 是 RTL 8T 的 5.1 倍（perf event count
  2.4e13 vs 4.7e12 cycles/10万拍）。

## 4. 优化方向（按预期收益排序）

1. ~~**wolvicmod 框架：action 融合/粗粒度求值**~~ **✅ 已完成（§6）**——
   1T 11.65 → 9.13 ms/拍，符合原预期（~9）；剩余差距集中在方向 2；
2. **边界结构**（~2.4 ms/拍）：~~o_out 按通道拆寄存器~~ **已实验证伪（§7）**——
   边界输出的下游 comb 被调度进 NBA 区、挂时钟位每拍无条件运行，与触发粒度
   无关；剩余路径（减少 BlackBox 端口暴露的无关字段以助 DCE、或将边界 comb
   移出 NBA settle 环）均需 Verilator 侧支持，"不改 Verilator"约束下暂无
   可行大收益路径；
3. **线程数**：当前 `WOLVIC=1` 默认 EMU_THREADS=16 已是最优点（优化①前：
   1T 11.65 / 8T 3.83 / 16T 2.67 ms/拍），无需调整；更多线程受同步主导预计
   收益递减；优化①后 MT 曲线需重新测量（每拍模型耗时大降，同步占比相对
   上升，最优点可能前移）；
4. 不建议动：`--threads-dpi none` 等 Verilator 分区旋钮，风险高收益不确定。

wolvic 1T 现已 8.93 ms/拍（RTL 1T 7.23，优化③后）；剩余 1.70 ms/拍差距落在
边界结构桶（方向 2），该桶已成为唯一的大头。

## 6. 优化①实施：脏驱动求值 + 去间接调用（2026-09-28）

**结果**：wolvic 1T **11.65 → 9.13 ms/拍**（50k 实测 456.3s，difftest 通过）。
模型侧单独 A/B（test_wolvic_top_replay，31.7 万拍纯模型驱动，同一二进制
`WOLVICMOD_DIRTY_EVAL` 切换）：全量求值 491s → 脏驱动 **171s（2.87×）**。

两个正交改动（均在 wolvicmod 框架，语义不变）：

**a) 去双层间接调用**（`core/action.h`、`core/edge.h`）：compute lambda 以
具体类型存进 `AssignAction<T,F>`/`UpdateAction<T,T,F>`（不再包
`std::function<T()>`），读值→计算→落值整条链内联进唯一的虚调用边界；
EventSlot/GuardSlot 的读函数从 `std::function<bool()>` 改为无捕获函数指针；
`intentActive()` 改为非虚拟成员读。

**b) 脏驱动跳过调度**（主贡献，`core/entity.h`、`sim/engine.h`、
`elab/elaborate.h`）：

- 每个实体绑定 `dirtyGen`（值变化的代际时钟戳），每个 action 记录
  `lastRunGen`；`dirtyGen > lastRunGen` 才重跑——静止逻辑锥整片跳过；
- 变化检测四处挂点：Assign 落值（== 比较）、`Reg::commitNext`、
  `Mem::writeRow`、`In::set`；不可 == 的类型保守传播；
- `markRan` 取运行后时钟值，拓扑序保证生产者先跑，单遍即可收敛，无过度
  传播；相位 2 提交后清除链上 intent 标志（跳过时不滞留）；
- 首轮/未曾运行的 action 恒运行（`lastRunGen==0`），trace 模式回退全量；
  回退开关：`WOLVICMOD_DIRTY_EVAL=0` 或 `Module::dirtyEvalOff()`。

**语义等价性验证**（全部通过）：

- wolvicmod ctest 16/16；proj ctest 17/17；
- cosim 对拍：wolvicmod prefab 30 组 + proj 51 组配置×seed 全过；
- coremark 全程 trace 回放（316,748 拍、5,423,490 项输出检查）**0 失配**，
  且全程开启读集对账（`REPLAY_AUDIT=1`）；
- emu 5 万拍窗口 difftest 无失败。

**连带成果——审计插桩抓到模型真实 bug**：脏求值要求读集声明完整（§3.1），
全量求值下无害的 stealth read 在脏求值下会产生过期值。据此修复两处模型
缺陷（全量求值时代就已存在的隐性依赖）：

- `model/dj/frontend.cpp`：`pos_table` 的 `addr_vec2`/`alr_use_pos`/`working`
  只声明 `sets[0]` 却经 `[this]` 捕获偷读 `sets[1..3]`；
- `model/dj/replace.cpp`：`req_pos_rdy` 只声明 `req_pos_arbs[0].in_rdy` 却
  按运行时下标偷读全部 8 个仲裁器。

建议：凡启用脏求值的模型，先在 `WOLVICMOD_AUDIT` 构建下跑一遍
`auditOn()` 确认读集完备。

**优化①后模型侧剩余成本实测**（9.04 ms/拍 拆分版，perf 50k 采样，
`build/perf-wolvic-1t-split.data`）：模型+DPI glue 合计 **9.7%**
（≈0.88 ms/拍；优化①前为 ~2.5 ms/拍 ≈21%）。构成：

- `wolvicmod::Module::eval()` 调度循环独占 **7.2%**（≈0.65 ms/拍）——
  脏求值后真正的 action 执行只剩 ~2.4%（≈0.22 ms/拍），但每拍仍要遍历
  整个展平 action 表逐个检查脏戳，**跳过的扫描本身成了模型侧最大开销**；
- glue（`wzj::packOutputs`/`unpackInputs`、`wolvic_zj_step`）< 0.05%，免费；
- 占比为下界：`std::` 模板辅助函数未计入（无法按 TU 归属区分）。

注：本次 dwarf 调用图在 -O3 生成代码的深层栈上大面积 unwind 失败
（`main` 的 Children 仅 0.86%），以上均为 Self 叶子占比（可靠）；
后续采样不必再加 `--call-graph`。

## 7. 优化②实验：o_out 按通道拆寄存器——收益≈0，机制证伪（2026-09-28）

**假设**：o_out 单体 1443b 结构体每拍 NBA 提交，Verilator 的 NBA 变化检测按
**变量粒度**触发下游——任何字段变化唤醒全部输出消费者。拆成按通道独立寄存器
（valid/bits 再拆开）后，静止通道（boot 后的 cfgAXI、无传输拍的 mem 通道）
的下游锥应整片跳过。

**实施**（`dpi/sv/WolvicZjBB.sv`，纯 SV 内部重组，DPI ABI/C++ glue/端口全不变）：
`o_out` 拆为 3 个 tx_ready、每 CHI rx 通道 `valid_q`+`bits_q`（rsp 66b/dat 367b/
snp 102b）、mem/cfg 各 5 个握手位 + 4 个 bits 包，NBA 块逐通道 `<=`，assign 段
从各 `_q` 解包。冒烟 PASS；emu 50k difftest 通过（IPC 0.842220 一致）。

**结果**：50k 实测两轮 **452.0s / 452.8s（≈9.04 ms/拍）**，对比优化①基线
456.3s（9.13 ms/拍）——**−0.9%，收益噪声级，假设证伪**。拆分版二进制留存为
`emu-wolvic-1t-split`。

**机制证伪（生成代码实证，verilator-compile/）**：

- 边界输出的下游组合锥被 Verilator 调度进 **NBA 区**（`nba_comb__TOP__*`，如
  `rx_dat_valid_q` 的消费者在 `nba_comb__TOP__105`），而 NBA 区 comb 批次的
  门控是 `(__VnbaTriggered[1]&0x80) | (__VnbaTriggered[0]&1)`——bit0 即
  **posedge clock 位，每拍必触发**。拆分细化的是 per-信号 trigprevexpr 触发
  （act 区机制），但 act 触发向量里**根本没有任何边界信号**（grep 实证），
  边界消费者全在 NBA 区跟随时钟位无条件运行——无论单体还是拆分；
- 时序消费者（如 L2 `rxdat_pipeline` 的 RAM 写，`nba_sequent__TOP__594`）
  本就每拍执行（posedge 块语义如此）；
- 因此"o_out 每拍全量 settle"的成本**不是触发粒度问题，而是调度区域问题**：
  DPI 调用点在 `always @(posedge)` 内 → 输出只能 NBA 写 → 下游 comb 被迫
  进入 NBA settle 环并挂到时钟位上。纯 RTL ZhuJiang 无此问题（L3 输出是普通
  comb/reg，下游锥在 act 区享受细粒度触发）——**这是 BlackBox 边界的固有
  结构成本，不改 Verilator 调度器无法消除**。

**附带坑（已修）**：SV 注释以 `// Verilator` 开头会被 Verilator 预处理器当作
元注释（大小写不敏感、允许空格）转成 `/*verilator ...*/` 交给解析器，报
`syntax error, unexpected '/'`。注释文字不得以 Verilator 开头。

**结论**：方向 2（边界结构）在"不改 Verilator"约束下已证伪一条路径；剩余
~1.9 ms/拍差距的主要嫌疑转为 NBA settle 环的每拍全量执行本身（需 Verilator
侧支持输出侧细粒度触发，或将 DPI 调用移出 posedge 块——后者改变语义，不可行）。

## 8. 优化③实施：推送派发（反向依赖表 + 层级桶就绪队列）（2026-09-28）

**动机**：优化①后模型侧最大开销已不是 action 执行（2.4%），而是
`Module::eval()` 调度循环本身（7.2% ≈0.65 ms/拍）——每拍遍历整个展平
action 表逐个比代际戳，即使全场静止也是 O(动作数 × 读集宽)；且 N 条 action
读同一变量时各比各的（各自 `lastRunGen` 阈值不同，无法共享检测）。

**机制**（wolvicmod 框架：新增 `core/readyq.h`，改 `core/entity.h`、
`elab/graph.h`、`elab/elaborate.h`、`sim/engine.h`）：把轮询（poll）反转为
推送（push）——

- 变化在写入点 `==` 检测**一次**，沿 elaboration 期构建的**反向依赖表**
  （CSR：实体 → 读它的执行项列表）把依赖者推入就绪队列——"相同读变量的脏
  检测能否 merge"与"增量派发"由此统一成同一个改动：检测合并成单次分发，
  静止逻辑锥零成本；
- 就绪队列按**拓扑层级**分桶（层级 = 沿凝结 DAG 的最长路径深度），配层级
  位图与入队去重标志；相位一按层级升序排空，生产者必先于消费者、每项每轮
  至多跑一次，空层级只耗一个位图字；排空进行中的新推送只落入更高层级；
- 自通知边（组合环 SCC 组内环边）不入表——组内传播由组内稳态迭代负责；
- `markDirty` 在任何调度模式下都推送（去重保证队列有界，超集排空幂等），
  因此 full/poll/push 切换无需重置钩子；首轮在 elaboration 末尾把全部执行
  项入队（等效轮询路径的首轮必跑）；
- 模式开关：`WOLVICMOD_DIRTY_EVAL` 默认 push；`=poll` 选旧轮询路径做 A/B
  对照；`=0` 全量求值。

**抓到的框架 bug（回放与全部既有测试都过，定向回归测试识破）**：SCC 组成员
信号变化时会把组伪 id 推回**它自己所在的层级桶**，旧 drain 在桶处理前清位，
把未处理的自推项清掉但去重标志留在 1——组从此永久不再被唤醒（DataBlock 的
ready-valid 环 deq0/w_has_two 在预充完成后停止更新）。定位手段：双实例
push-vs-full 逐半拍全实体比对器（waveFormat 快照），cyc 64 分叉。修复两条：
drain 改索引循环（活大小）且清位移到桶处理之后；自通知边在构表时过滤。
回归固化在 `wolvicmod/tests/test_readyq.cpp`（SCC 重复再激活用例 + 混合模型
300 拍随机激励 push≡full 逐半拍比对；回退任一修复均可复现失败）。

**结果**：

- 模型侧 A/B（test_wolvic_top_replay，31.7 万拍纯模型驱动，同一二进制切
  环境变量）：poll 166.39s → push **83.55s（1.99×）**；累计相对全量求值
  491s 为 **5.7×**；
- emu 50k（①+②+③ 全栈）：**446.6s（8.93 ms/拍）**，difftest 通过
  （IPC 0.842220）；对比①+②基线 452.0s/452.8s 为 **−1.2%**。收益幅度
  符合预期——emu 里模型侧占比已只剩 ~9.7%，调度循环大头被消除后总账
  改善 ~0.1 ms/拍；模型侧 1.99× 的价值主要体现在后续模型规模扩大时
  （调度成本从 O(动作数) 降为 O(活跃动作数)）。

**验证**（全部通过）：wolvicmod ctest 17/17（新增 test_readyq 三模式等价
用例）、wolvicmod cosim 30 组、proj ctest 17/17、proj cosim 51 组、
coremark 全程回放 `REPLAY_AUDIT=1` 审计、emu 50k difftest。

## 9. 孤立回放对比：RTL ZhuJiang vs wolvicmod（2026-09-28）

**单位更正**：本文 §1–§8 及 pdocs §5.4 此前把 emu 每拍墙钟的单位误写为
µs，正确单位为 **ms**。证据：emu `cycleCnt` 是原始周期数——full 日志
`instrCnt=663,692 / cycleCnt=316,801 = 2.0949` 与显示 IPC 精确吻合；
且 316s/316,801 拍若按 µs 计则 8T emu 达 1MHz，物理不可能。已全部订正
（RTL 1T = 7.23 ms/拍 ≈ 138Hz，wolvic push 1T = 8.93 ms/拍 ≈ 112Hz）；
历史 commit 消息中的单位错误不回溯修改，以本节为准。回放类数字（秒级
墙钟及其 µs/row 换算）单位无误。

**方法**：把 ZhuJiang RTL 从 SoC 中单独抽出——新增
`XiangShan/src/test/scala/top/ZhujiangReplayTop.scala`（`Zhujiang` +
`SocketDevSide` + flit remap，配置经六条 alterPartial 覆盖链与 SoC 内
`zhujiang_opt` 完全一致，拓扑同为 `ZhuJiangNoCTopology` 单核环，CHI
dataCheck/poison 同关），mill elaborate（firtool 参数对齐 SoC 的
`-O=release` 等）+ verilator `-O3` 产出独立 DUT 归档。回放由**共栖 A/B
回放器**承载（`verify/zjrtl/ab_replay.cpp` → `build/zjrtl/zj_ab_replay`）：
同一二进制链接 verilated RTL 与 WolvicZjTop 裸模型（无 DPI），
`--dut=rtl|wolvic|both` 运行时选择激活侧，两侧均不开波形；solo 用于性能
剖析、both 用于等价性交叉验证。本节数字出自其前身（独立
rtl_replay.cpp + test_wolvic_top_replay.cpp，已被前者取代/后者保留为
ctest），同形状口径不变：同一份 coremark 全程 trace
（`build/trace/cm_full.txt`，316,748 拍）、同样的预滚（reset 10 拍 +
空闲 2000 拍，仅 RTL 侧）、同样的 valid 门控与逐字段比对。一键复现：
`make zjrtl-replay DUT=rtl` / `DUT=wolvic` / `DUT=both`。

**行为（本轮主要收获）**：两侧各自对同一 trace 全程 **0 失配**（RTL
5,106,742 项检查、wolvic 5,423,490 项检查）。trace 记录的是 wolvic
模型在 emu 边界上的逐拍 I/O，故 RTL 独立回放的全程一致构成
**RTL ≡ 模型**的直接证据——wolvicmod 版 ZhuJiang 的周期精确性第一次
获得 RTL 侧（而非仅模型自身确定性回放）的实证。

**性能**（同机、单线程、-O3，同形状 harness）：

| DUT | loop 墙钟 | 每拍 | 分相 / 热点 |
|---|---|---|---|
| RTL ZhuJiang（verilated 独立） | 4.07s | **12.8 µs/拍** | eval0(clk=0) 3% / eval1(posedge) 70% / harness 27% |
| wolvicmod（push 派发） | 83.55s | **263.8 µs/拍** | `Module::eval()` self 29% + `Entity::markDirty()` self 25% + 几千条 UpdateAction/AssignAction 长尾 |

harness 两侧同构（~3.5 µs/排的列哈希查找+比对）：RTL 侧占 27%，
wolvic 侧仅 ~1.3%，不影响结论。firtool 是否加 `-O=release` 对回放速度
影响 <2%（rtl 源 84M→37M，但 verilator `-O3` 才是主导）。

**交叉验证（perf stat 全程，排除测量伪影）**：每拍指令数 RTL **207k** vs
wolvic **2,794k**（13.5×）；IPC 2.55 vs 1.87（CPI 差 1.36×，虚调用/指针
跳转所致）；有效频率 6.28 vs 5.69 GHz（1.10×）。13.5×1.36×1.10 ≈ 20×
与墙钟比精确吻合——差距是**真实的每拍指令量差距**（框架逐信号 action
解释执行 vs Verilator 全设计静态调度直线代码），不是测量错误。RTL 侧
eval1(posedge) 占 70% 且 510 万项比对 0 失配，也排除"DUT 被 DCE 掏空"
式的假性快。

**共栖复现（2026-09-28，zj_ab_replay 单二进制）**：`--dut=rtl` 全程
4.24s（13.4 µs/排，eval-only 9.84 µs/排）、`--dut=wolvic` 全程 83.08s
（262.3 µs/排，eval-only 257.2 µs/排）——与上表独立 harness 数字一致
（±4% 内为频率波动）；`--dut=both` 同进程同激励驱动两侧，全程各自
0 失配（RTL 5,106,742 / wolvic 5,423,490 项），both 模式下 RTL eval
因缓存互扰从 3.12s 涨到 4.67s（wolvic 侧 81.1s≈不变），故剖析一律用
solo。wolvic 侧分相值得注意：clk0（输入传播）占 eval 的 38%
（31.3s/81.5s），RTL 侧 clk0 仅 3.5%——框架每次 eval 都走 action 图
调度，两半拍成本同量级，而 Verilator 的 clk=0 只触发输入锥。

**解读**：

- 孤立看，模型比 RTL 慢 **20.6×**；而 emu 内差距只有 1.24×
  （8.93/7.23 ms/拍）——SoC 其余部分淹没了 L3：emu 里模型侧占比仅
  ~9.7%（§6），wolvic 与 RTL 的总账差距（1.70 ms/拍）主要由 BlackBox
  边界结构成本（§2.2/§7）贡献，而非模型原始算力差距；
- 反向看 locality 的放大：孤立 RTL L3 仅 12.8 µs/拍，而 SoC 内 perf 对
  zhujiang 符号的归因下界达 ~0.89 ms/拍（§2）——孤立 harness 工作集小、
  全程 cache 热；SoC 内 L3 逻辑与全芯片争用 icache/dcache/分支预测，
  且归因含跨界内联份额。两侧同形状 harness、同机同编译器，横向对比
  （RTL vs 模型）不受此影响；
- wolvic 侧热点结构：coremark 下 L3 持续活跃（PosEntry/CommitEntry/
  ReplaceEntry/DataCtrlEntry/ReadEntry/VipTable 等条目状态机每拍真跑），
  push 派发可跳过的静止锥很少，成本集中在框架逐信号 action 粒度
  本身——`eval()` 调度 + `markDirty()` 写入点检测合计过半。若未来
  模型规模扩大或 L3 在系统中占比上升（多核/多 slice），这 20× 的
  原始差距会显性化，届时优化杠杆是 action 融合/按模块粗粒度求值
  （§4 方向 1 的下一阶段）。

**产物**：`build/zjrtl/{rtl,obj,zj_ab_replay}`（共栖 A/B 回放器）；perf 采样
`build/perf-zjrtl-replay.data`、`build/perf-wolvic-replay.data`。

## 10. 优化④实施：相位二推送化 + 边沿方向过滤（2026-09-28）

**动机**：优化③后 wolvic 侧回放热点前二是 `Module::eval()` self 29% +
`Entity::markDirty()` self 25%，其中藏着两块结构性浪费：

1. **相位二每轮全扫**——push 模式把相位一改成按需派发后，相位二仍逐
   priority chain 轮询 `intentActive()`（poll 时代残留，O(链数)/轮）；
2. **下降沿虚跑**——每条 Update 的读集都含时钟，时钟每半拍必翻转，于是
   每半拍全部 Update 被推送并运行一遍；但下降沿半拍 posedge 永不可能
   命中。这正是 §9 里 wolvic 侧 clk0 占 eval 38%（RTL 侧仅 3.5%）的主因。

**机制**（wolvicmod 框架，语义不变）：

**a) 相位二推送化**（`core/readyq.h` 新增 CommitQueue，`core/action.h`、
`elab/graph.h`、`sim/engine.h`）：Update 意图标志升起（假→真）时把所属
priority chain 推入提交队列，相位二只排空该队列——空转链零成本。
`committed` 缓冲提升为 SimState 成员（消除每 eval 一次的堆分配）；队列
排空仅在 trace 模式下按链注册序排序（保持提交记录顺序），正常模式无序
（链间目标不相交，顺序无关）——profile 曾抓到全量 `std::sort` 独占
5.2%，由此消除。连带：push 排空路径去掉 `markRan` 写戳（`lastRunGen`
仅 poll 路径消费，而 poll 只能在 elaboration 经环境变量进入，运行时
`dirtyEvalOn/Off` 只往返 Push/Full，无途径进入 Poll）。

**b) 边沿方向过滤**（`core/entity.h`、`core/action.h`、`core/expr.h`、
`core/module.h`、`core/reg.h`、`elab/graph.h`）：bool 写入点（`In::set`、
`Reg::commitNext`、bool Assign 落值）改走 `markDirtyBool(v)`，上报跳变
方向。注册期判定资格：某事件信号若被一条 Update **仅**用作单一方向边沿
（不同时出现在 `.reads(...)`、`.en(...)`、`.addr(...)` 或另一方向事件里），
elaboration 时该依赖从恒推反向表移入实体的方向监视表（CSR）。匹配方向
跳变才推送；反向跳变只在写入点把该 Update 的 `EventSlot::prev` 拨到新值
——既不唤醒，也不漏下一次真边沿。正确性依赖"每实体每轮至多一次跳变"
（§5.2 既有前提），故 **SCC 组内动作驱动的信号不参与过滤**（稳态迭代
可能多次改写）；无方向的通用 `markDirty`（Mem 写、不可 == 类型）对双向
监视者保守全推。poll/full 路径语义不变（poll 读集不剔除事件信号）。

**结果**（孤立回放，同一二进制 `zj_ab_replay --dut=wolvic`，coremark 全程
316,748 排，干净单机）：

| 版本 | loop 墙钟 | 每排 | eval 分相 |
|---|---|---|---|
| 优化③（push） | 83.08s / 83.55s | ~263 µs | clk0 31.3s + clk1 ~50.2s |
| **优化④** | **61.42s / 64.60s** | **~199 µs** | **clk0 9.0–9.3s + clk1 ~51–54s** |

- 模型侧 **1.30–1.36×**；clk0（输入传播半拍）**3.4×**——边沿过滤正中靶心；
  clk1（posedge 半拍，Update 本就全跑）持平，符合预期；
- 孤立回放对 RTL 的差距从 **20.6× 收窄到 ~14.5×**（RTL 4.25s）；相对全量
  求值（491s）累计 **~7.8×**；
- 热点迁移（`build/perf-wolvic-replay-opt4.data`，10 万排采样）：
  `eval()` self 29.2→**24.2%**，`markDirty` 24.8→`markDirtyBool` **21.8%**，
  posedge reader 4.7→**2.0%**；剩余大头是 eval 调度循环与写入点扇出推送
  本身，下一步杠杆仍是 action 融合/按模块粗粒度求值（§4 方向 1）。

**验证**（全部通过）：wolvicmod ctest 17/17（`tests/test_readyq.cpp` 新增
EdgeFilter 用例：posedge/negedge、In/Wire/Reg 三类事件源、事件信号兼任
plain read 的不过滤路径、双方向事件、守卫 Update、双 Update 优先级链、
时钟高/低电平期间搅动输入，push≡full 逐半拍全实体比对）、wolvicmod
cosim 30 组、proj ctest 17/17、proj cosim 51 组、coremark 全程回放
`REPLAY_AUDIT=1`（0 失配）、`--dut=both` 共栖交叉（RTL 5,106,742 /
wolvic 5,423,490 项各自 0 失配）、poll/full 模式 5 万排健全性 0 失配。

**说明**：本轮未重跑 emu 全程/50k（模型侧在 emu 中占比仅 ~9.7%，预期总账
改善为亚 0.1 ms/拍 量级）；如需 emu 端数字，重链 `libzjmodel.a` 后按 §5
复现命令测 5 万拍窗口即可。

## 11. 优化⑤实施：Flat 引擎——静态序 + 活性位图（2026-09-28）

**动机**：优化④后调度/派发仍占 ~52%（eval 24.2 + markDirtyBool 21.8 +
容器操作/notifyIntent 6.6），其中队列簿记（push/pop/去重标志/桶管理）是
推送范式的结构性成本。Flat 引擎把"逐 action 推送"换成 verilator 式
trigger 位图：`execOrder` 已是拓扑序，扫描它即按拓扑序执行；每个实体的
扇出表复用既有 CSR 反向依赖表，写入点从"操作就绪队列"退化为
`bits[pos>>6] |= 1<<(pos&63)`——**位 OR 幂等，去重免费**；相位二同理，
intent 假→真时置 chain bit（位图天然升序 = 链注册序，trace 顺序无损）。

**机制**（wolvicmod 框架，`SchedMode` 新增 `Flat` 并设为默认）：
`SimState` 加 `actBits`（每 execOrder 位置一 bit，SCC 组一位）与
`chainBits`（每 priority chain 一 bit）；`Entity::pushDeps/pushWatch` 与
`Action::notifyIntent` 按 elaboration 期绑定分流（位图或队列）；
`Module::eval()` 相位一位图 tzcnt 扫描、相位二链位图扫描；
`buildExecOrder` 增**回边断言**（生产者位置必须小于消费者，拓扑序的
一次性校验）；`WOLVICMOD_DIRTY_EVAL=push` 退回队列派发（A/B），
`dirtyEvalOn/Off` 在 Flat↔Full 间切换（Full 下写点仍置位，切回不丢脏）。

**结果**（孤立回放，同一二进制环境变量切换引擎，coremark 全程 316,748
排，taskset 干净单机，两次取区间）：

| 引擎 | loop 墙钟 | eval | 分相 | 每排 |
|---|---|---|---|---|
| Push（=优化④） | 61.48 / 61.74s | 59.9 / 60.2s | clk0 9.25 + clk1 ~50.8 | ~194 µs |
| **Flat** | **55.42 / 57.74s** | **53.8 / 56.2s** | clk0 8.4–8.7 + clk1 45.5–47.4 | **~175 µs** |
| RTL | 4.14s | 2.99s | clk0 0.11 + clk1 2.88 | 13.1 µs |

- 模型侧 **1.07–1.11×**；对 RTL 差距 14.5×→**13.4×**；相对全量求值
  （491s）累计 **~8.9×**；
- **收益低于预估（1.5–1.8×）的教训**：§10 profile 里 eval/markDirtyBool
  的 self 时间混着真活（判等比较、CSR 遍历、dirty 时钟滴答、run() 本体
  的内联归属），纯队列簿记实测只占 ~10%。位图化把簿记压到近零，但
  逐 action 虚调用与逐信号判等仍在——这两项才是 Flat 之后的主导项；
- 热点迁移（`build/perf-wolvic-replay-flat.data`，10 万排采样）：
  `markDirtyBool` 21.8→**14.0%**（队列操作已消，余为 CSR 遍历+位 OR），
  `std::sort` 5.2→**0**、`vector::push_back` 3.5→**0**（推送路径容器
  操作整体消失）；`eval()` 24.2→30.9%、`AssignAction<bool>` 4.6→5.9%
  占比上升是分母变小的相对效应；
- 审计态全程回放 361s→**286s**（REPLAY_AUDIT=1，读集对账开启）。

**验证**（全部通过）：wolvicmod ctest 17/17（test_readyq.cpp 新增
Flat≡Push≡Full 三方比对——含 SCC 重激活、边沿过滤、commit 派发、
时钟电平搅动，及 dirtyEvalOff/On 往返切换不回丢脏用例）、wolvicmod
cosim 30 组、proj ctest 17/17、proj cosim 51 组、coremark 全程回放
`REPLAY_AUDIT=1`（0 失配）、`--dut=both` 共栖交叉（RTL 5,106,742 /
wolvic 5,423,490 项各自 0 失配）。

## 12. 优化⑥实施：恒等连接别名化——elaboration 期消除 `b = a` 拷贝（2026-09-28）

**动机**：§11 之后逐 action 虚调用与逐信号判等成为主导项，而 clk0 相位的
9.25s 几乎全是 clk 树派发——一连串 `AssignAction<bool>` 恒等拷贝
（`In=In` 端口直连、层次间时钟下穿）。同型恒等连接（快速路径
`assignFrom`，`is_same_v<T, ReadValueOf<S>>` 且非 Mem）语义上就是让
目标的值永远等于源的值——**那就让目标共享源的存储**，拷贝动作本身
从图中删除，连"执行一次判等"都不必再付。

**机制**（wolvicmod 框架，`elab/flatten.h` 新增 `resolveAliases()`）：
注册期 `assignFrom` 的 Readable 分支在类型严格相同时给 Action 打
`aliasSrc_` 标记（带转换的拷贝不别名）；elaboration 期 union-find 以
**源侧为 canonical 根**，目标实体 `aliasStorageTo()` 绑定到源的
`valueStorage()`（Signal 加 `alias_` + `effective()` 访问层；Reg 暴露
`&cur_`）；消费者的 reads/targets/producerOf/watchOf/方向过滤行全部经
canon 表重挂到源实体（`buildExecOrder` 规范化）；被消除的动作不进入
execOrder——包括无边孤立平凡组件的发射处也要跳过（否则触发 Flat 的
回边断言）。别名环（`a=b; b=a`）的闭合边保留为真实拷贝动作，不别名。
`WOLVICMOD_NO_ALIAS=1` 逃逸。判等短路、边沿方向过滤、NBA、拓扑序语义
全部不变（存储共享 ⟹ 目标读到的值与"拷贝后"逐位相同；脏传播改挂源的
反向依赖行，变化信息与经拷贝转发等价）。

**结果**（孤立回放，coremark 全程 316,748 排，taskset 干净单机；
alias-on/off 为**同一二进制** `WOLVICMOD_NO_ALIAS` 切换，pre-alias 列为
§11 的 flat 构建作参照）：

| 构建 | loop 墙钟 | eval | clk0 | clk1 |
|---|---|---|---|---|
| Flat（pre-alias，§11） | 55.42 / 57.74s | 53.8 / 56.2s | 8.4–8.7s | 45.5–47.4s |
| Flat+alias-off | 62.24 / 62.28s | — | 9.25s | — |
| **Flat+alias-on** | **45.02 / 45.18s** | **43.5 / 43.6s** | **1.36s** | **~42.1s** |

- **clk 树派发 6.8×**（clk0 9.25→1.36s）——clk  nets 全部别名化后，clk0
  相位只剩时钟源头的写与边沿过滤行，逐层拷贝动作整体消失；
- 净收益口径：vs pre-alias flat（55.4–57.7s）**1.23–1.28×**；同二进制
  A/B **1.38×**；对 RTL 差距 13.4×→**10.9×**；相对全量求值（491s）
  累计 **~10.9×**；
- 注意 alias-off 比 pre-alias flat 慢（62.2 vs 55.4–57.7s）：差值是
  `Signal::effective()` 间接分支在每次信号访问上的开销——别名化在关态
  也要付这层间接，开态的收益（1.38×）远超它；
- 热点迁移（`build/perf-wolvic-replay-alias.data`，10 万排采样）：
  `markDirtyBool` 14.0→**4.0%**，`AssignAction<bool>` 恒等拷贝从榜单
  消失，`eval()` 29.0%、`notifyIntent` 5.49%、`CMTask::operator==` 2.25%
  （判等比较成为所剩不多的结构性成本）。

**验证**（全部通过）：wolvicmod ctest 18/18（新增 test_alias.cpp：图消除
计数 19→5、层次/链式/Reg 源/SCC 穿越别名三方等价、退化别名环、读穿验证；
test_elab.cpp 的 execOrder 期望随消除更新）、wolvicmod cosim 30 组、
proj ctest 17/17（全程回放 loop 42.5s）、proj cosim 51 组、coremark
全程 `--dut=both` 共栖交叉：RTL 5,106,742 / wolvic 5,423,490 项各自
**0 失配**（wolvic eval 46.4s：clk0 1.374 + clk1 44.99——clk0 与孤立
A/B 的 1.36s 一致）、审计态全程回放 `REPLAY_AUDIT=1` 通过且
286s→**220s**（读集对账开启，累计 361→286→220s）。

## 13. 优化⑦实施：相位二快速提交——去虚调用 + 一个反直觉的布局教训（2026-09-28）

**动机**：§12 之后对 eval() 的 29% 自时间做指令级归因（`perf annotate`），发现
**三条间接虚调用指令独占 eval 样本的 34%（≈全程 9.9%）**：相位一位图扫描里的
`Action::run()`（16.6%）、相位二链提交里的 `finalizeTarget()`（9.5%）与
`applyIntent()`（7.9%）。样本堆在 `callq *0x18(%rax)` 这类指令上是典型的间接分支
失靶停顿：活跃动作按数据依赖序执行，目标在 500+ 个动作实例化类型间近乎随机
交替，BTB 持续失靶，每次 ~15–20 周期全部记在 call 指令上。

**机制**（wolvicmod 框架）：

1. **相位二快速提交（fast commit）**：一个优先级链的全部 Update 共享同一目标
   Reg<T>，相位二做的事本质是"把 active 成员的 intent 写进 next 槽（反序，高优先级
   最后落），再比较 cur/next 决定是否提交并标脏"——全程不需要逐成员虚调用。
   elaboration 期（`buildUpdateChains`）为符合条件的链绑定类型无关描述符
   `{cur, next, size, isBool, bitwiseEq, typedCommit}`（成员经
   `Action::publishFastIntent` 发布 intent 槽位地址）；相位二快速路径用统一字节
   操作完成应用与提交：小尺寸内联单指令拷贝/比较，大尺寸走 memcpy/memcmp——
   **调用目标对所有链、所有值类型唯一，分支预测器不再见到多样的间接目标**。
   有填充的值类型（`has_unique_object_representations_v<T>` 为假，memcmp 非精确
   判等）退化为每链一次 `typedCommit` 间接调用——目标是 per-类型（而非 per-动作）
   的静态函数，预测友好；Mem 与非平凡拷贝类型整体留在慢速虚调用路径。
   语义不变：字节相等 ⟹ 值相等 ⟹ 不脏不提交（同 `commitNext` 的判等短路）；
   bool 提交保留跳变方向上报；优先级顺序、互斥断言、trace onCommit 全保留。
   逃逸开关 `WOLVICMOD_NO_FAST_COMMIT=1`。
2. **Action 所有权改为模块级注册表**（`Module::makeAction`），替代逐动作
   `make_unique`。**反直觉教训**：最初实现为 bump arena（按注册序紧凑排布，期望
   改善扫描局部性），实测比 malloc **慢 13.5%**（46.3 vs 40.1s）——glibc malloc 的
   size-class 分箱天然让同实例化类型的动作对象相邻（同一链的成员类型必同），
   这正是相位一扫描与相位二链遍历的访问模式；注册序交错布局反而打散了它。
   bump arena 已移除，注册表仅保留所有权管理（析构在模块 teardown 时批量跑）。
3. **提交簿记瘦身**：`SimState::committed` 每提交一次 push_back（只服务于收敛
   判空与 roundCap 报错），在新代码布局下被编译器外联成独立调用，吃到 4.5%。
   改为计数器 + 前 64 条截断收集（报错信息仍能列出样本，超出部分计数），
   热路径上只剩一次自增。

**结果**（孤立回放，coremark 全程 316,748 排，taskset -c 2；on/off 为同一二进制
`WOLVICMOD_NO_FAST_COMMIT` 切换）：

| 构建 | loop 墙钟 | eval | clk0 | clk1 |
|---|---|---|---|---|
| off（虚调用提交） | 48.4 / 51.1 / 51.1s | 46.9–49.6s | 1.61s | 45.3–48.0s |
| on（快速提交） | 43.3 / 44.3 / 45.6s | 41.8–44.1s | 1.52–1.63s | 40.3–42.6s |
| **on + malloc + 簿记瘦身（终版）** | **40.1 / 41.4 / 41.6 / 43.2s** | **39.8–40.1s** | **1.43–1.50s** | **38.4–38.6s** |
| 对照：§12 别名化终版 | 45.0 / 45.2s | 43.5 / 43.6s | 1.36–1.44s | ~42.1s |

- 同二进制 A/B：快速提交 **-11.5% 墙钟**；30k 排窗口分支失靶 **-37%**
  （44.9M→28.2M），指令数 +1.4%（内联字节操作替代调用指令的预期交换）；
- 对 §12 基线累计 **45.0s → 41.4–41.6s ≈ 1.08×**；对 RTL 差距
  10.9× → **~10.0×**；相对全量求值（491s）累计 **~11.9×**；
- clk0 基本持平（相位二提交集中在 clk1）；clk1 42.1 → 38.4–38.6s；
- 注：跨二进制的指令数对比出现过 +13% 的无法完全归因差异（疑为代码排布
  代际噪声），本节结论一律以同二进制 env 切换的 A/B 为准。

**终版热点分解**（`build/perf-wolvic-replay-fc.data`，10 万排，self-time）：

| 桶 | 占比 | 内容 |
|---|---|---|
| 框架 eval 壳 | 34.7% | 相位一 `run()` 虚派发是唯一残留热点（annotate：单条 `callq` 占 eval 样本 31%）；相位二两条虚调用已从榜单消失 |
| 框架脏派发 | 10.7% | `markDirtyBool` + `notifyIntent`（写入点 CSR 扇出置位） |
| 框架守卫/边沿 | 6.0% | posedge 边沿历史求值等 |
| 框架字节提交 | 2.4% | 快速提交的 memcpy/memcmp（真活） |
| 模型 dj（目录/流水线项） | ~24.5% | CommitEntry 4.3 + TaskEntry 3.3 + CommitTask 2.6 + CMTask 2.2 + PosEntry 2.1 + PosState 1.9 + DirectoryBase 1.8 + ReplaceEntry 1.5 + DataCtrl 1.0 + 其余 |
| 模型 chi（flit 比较等） | ~6.5% | DataFlitT 2.1 + ReqFlitT 1.7 + RespFlitT 1.6 + HrqFlit 1.1（大头是结构体 `operator==`） |
| 模型 bridge + ring | ~4.8% | BridgeCm 2.1 + SingleChannelTap/VipTable/EjectBuffer/RingPipe |
| harness/libc | 4.3% | 回放器 Trace 解析与驱动，非模型 |
| 其他/内核 | ~1.5% | |

读法：框架壳 56% vs 模型真活 39%。eval 壳里只剩相位一 `run()` 虚派发一个热点，
其失靶率正比于动作类型多样性——这把下一步指向建模层：dj 各 Entry 的逐字段
Update（上表 dj 桶的大头）整项化可同时压"模型真活的壳成本"和"类型多样性"。
（勘误：本节"eval 壳 34.7%"把相位二 commitChain 的 18.2% 也算了进去，相位一
虚派发实际只有 ~9%；修正与由此产生的优化见 §14。）

**验证**（全部通过）：wolvicmod ctest 18/18、wolvicmod cosim 30 组、
proj ctest 17/17、proj cosim 51 组、coremark 全程 `--dut=both` 共栖交叉
（RTL/wolvic 各 0 失配）、审计态全程回放 `REPLAY_AUDIT=1`。

## 14. 优化⑧⑨实施：相位二成员位图提交 + 相位一类型聚类调度（2026-09-29）

**动机（归因修正）**：对 §13 终版做指令级复核时发现桶划分有误——"eval 壳
34.7%"里那个 18.2% 的 lambda 不是相位一派发，而是**相位二的 commitChain**
（eval() 里仅两个 uint32_t lambda：#1 是 Push 模式 drain 回调，Flat 下为死
代码；#2 是 commitChain，由 chainBits 扫描循环直调）。真实切分：相位一虚派发
~9%，相位二提交机制 ~20%。perf stat（30k 排）补充机制画像：分支失靶仅占分支
数 0.28%，而 **L1-dcache 失配率 12.2%**、IPC 2.26——已是内存延迟受限而非分支
预测受限。commitChain 的 annotate 无单点热点：18% 弥散在约 30 条指令上——每
条活跃链要对散落的成员 Action 对象做三遍指针追逐（数 intent → 反序拷 intent
→ 逐成员 clearIntent），每次都是潜在 L1 失靶。

**机制⑧：成员位图提交**（逃逸开关 `WOLVICMOD_NO_MEMBER_COMMIT=1`）

成员 intent 状态从成员对象里的 bool 下沉为**链描述符里的位图**：

- Chain 新增 `memberBits`（位 i = updates[i] 的 intent，注册序）与
  `intentSrcs[]`（各成员 intent blob 地址，elaboration 期随 fast commit
  一并绑定）；
- `UpdateAction::run()` 对绑定了成员位的链改用位图为唯一事实源：
  `active && !raised` → 置链位 + 成员位（位 OR 幂等，同 actBits）；
  回落清成员位。`intentActive_` 退化为 trace 镜像（trace 强制 Full，Full
  下每个动作每轮重跑刷新，语义不变）；
- 相位二零次冷 Action 解引用：读 memberBits（热）→ 为 0 直接返回（替代
  整个 activeCount 轮询遍）→ popcount 做互斥断言 → clz 降序遍历置位成员
  （= 反注册序，最高优先级最后落地）→ 从稠密 intentSrcs 字节拷贝（活跃成员
  的 intent 本轮刚被 run() 写过，L1 命中）→ `memberBits = 0` 一次存储替代
  逐成员 clearIntent → 字节判等提交照旧；
- 正确性要点：Flat/Push 下 Update 每轮至多执行一次（Update 无组合输出，
  必不在 SCC 内），"升起-提交-清零"握手无重入；提交后 memberBits 已清而
  intentActive_ 不再由相位二清理，故 run() 的升起检测必须读位图而非旧
  flag——否则陈旧 true 会吞掉下一次升起的 notify，漏提交；
- Mem 链与 >64 成员链保持旧轮询路径（后者现实中不存在）。

**机制⑨：类型聚类拓扑调度**（逃逸开关 `WOLVICMOD_NO_TYPE_CLUSTER=1`）

`buildExecOrder` 的 Kahn 出队纪律从 FIFO 换成**类型贪心**：就绪分量中优先
发射与上一个动作同 vtable 的（精确到模板实例化）。任何拓扑序的线性扩展都
保持"生产者位置 < 消费者位置"不变式（depPool 构建处的断言照旧兜底）；实体
节点与已消除动作无执行项，就绪即发射（只为解锁下游）；SCC 组只进到达序后备
队列并打断当前类型连跑。同型连跑让相位一 `run()` 虚调用目标高度重复（BTB
友好），且同类型 = 同 size class，malloc 分箱使对象相邻，顺带改善 D$ 预取。

实现中踩了两个调度器 bug（教训：双重入队必须**双边**惰性去重——byTag 与 seq
互不知晓对方的消费；自由节点排空可能恰好耗尽整个图，循环体尾部必须重查退出
条件），已由新增的不变式断言（发射分量数 == 总分量数）锁死回归。

**结果**（孤立回放，coremark 全程 316,748 排，taskset -c 2；on/off 为同一
二进制 env 切换）：

| 配置 | eval（30k 排） | eval（全程） |
|---|---|---|
| ⑧+⑨ on | 3.562s | 37.57 / 38.17s |
| 仅⑧ | 3.760s | 39.10s |
| 仅⑨ | 3.759s | 38.37s |
| ⑧⑨ off | 3.989s | 38.92 / 41.16s |

- 30k 排同二进制：⑧ -5.7%、⑨ -5.8%、合计 -10.7%；全程 off 侧噪声偏大，
  on 37.6–38.2 vs off 38.9–41.2；
- 对 §13 终版（39.8–40.1 eval / 41.35–41.61 loop）：eval **-4.9%**，loop
  **41.35 → 39.15s（-5.3%）**；对 491s 全量基线累计 **~12.5×**，对 RTL
  差距 ~10.0× → **~9.1×**；
- perf stat（30k，新旧二进制）：分支失靶 29.3M → 23.2M（**-21%**），指令
  -0.7%，L1 失配率 12.2% → 12.4% 持平——确认内存延迟瓶颈在模型数据本身；
- profile 迁移：commitChain 18.2% → 15.0%（成员轮询三遍已消；残值是每活跃
  链的固定开销 + typedCommit 间接调用 ~1.5%），eval() 16.5% → 14.9%（相位一
  callq 占 eval 样本 30.7% → 25.1%）。

**剩余方向**（本轮未做）：相位一 callq 残值 ~3.7% 全程，per-type run
trampoline（同型连跑段改单态直接调用循环）的收益上限也就这么多；commitChain
强制内联省调用/栈帧约 1-2%；真正的大头仍是建模层整项化（§13 末分析不变）。

**验证**（全部通过）：wolvicmod ctest 18/18；30k 排四配置（on/off/仅⑧/仅⑨）
与 push/poll/full 三遗留模式各 0 失配；coremark 全程 `--dut=both` 共栖交叉
（两侧各 0 失配）；proj ctest 17/17；wolvicmod cosim 30 组；proj cosim 51 组；
`REPLAY_AUDIT=1` 全程审计回放。

## 15. 代码压缩：单路径化——删除备选引擎与全部 A/B 逃逸开关（2026-09-29）

**动机**：§6–§14 每轮优化都带 A/B 逃逸开关与备选路径，终版框架里同时活着
Full/Poll/Push/Flat 四种调度模式与五个环境变量。实验期结束，把实测最优组合
固化为唯一路径，删死代码与热路径上的冗余机器。

**删除清单**：

- **引擎路径**：Poll（代际时钟 dirtyGen/lastRunGen 比对）与 Push（层级桶就绪
  队列 + CommitQueue）整体删除，Flat 位图派发成为唯一引擎；Full 全扫仅保留
  为 trace 模式的内部路径（trace 需要每个动作每轮状态新鲜），无用户开关；
- **开关/API**：环境变量 `WOLVICMOD_DIRTY_EVAL`、`WOLVICMOD_NO_ALIAS`、
  `WOLVICMOD_NO_FAST_COMMIT`、`WOLVICMOD_NO_MEMBER_COMMIT`、
  `WOLVICMOD_NO_TYPE_CLUSTER` 与 `Module::dirtyEvalOn/Off()`；
- **机器**：`core/readyq.h`（80 行）与 `tests/test_readyq.cpp`（210 行）整删；
  SimState 的 clock/dirtyGens/readIdxPool/readyQ/commitQ 字段；Entity 的
  dirtyGen 盖章；Action 的 lastRunGen/readIdx 绑定与 CommitQueue 推送；
  buildExecOrder 的 FIFO 调度分支（类型贪心成为唯一调度器）；
- **热路径附带收益**：`markDirty` 去掉每次写入一次的全局代际计数器 RMW
  （共享缓存行上的原子递增）；Action 对象缩小 24B。

**测试同步**：test_alias 的 no-alias 对照变体删除（A/B 对象已不存在），改为
单构建结构性断言——恒等链末端值（out1/out2）、转换拷贝（out3）、无守卫
posedge 计数器精确值、别名 SCC 的锁存行为（o 锁存 en），300 轮随机激励；
test_elab 一处动作计数同步；其余测试不动。

**结果**（coremark 全程 316,748 排，taskset -c 2，两次取区间）：

| 指标 | §14 终版 | 压缩后 |
|---|---|---|
| eval | 37.57 / 38.17s | 37.39 / **35.30s** |
| loop | 39.15 / 39.73s | 38.92 / **36.84s** |

持平偏好，符合预期——删的主要是冷代码与 elaboration 期机器，热路径收益来自
markDirty 少一次共享计数器 RMW 与更小的 Action 对象足迹。语义与算法不变，
累计对 491s 全量基线 **~12.5×**、对 RTL 差距 **~9.1×** 维持。

**验证**（全部通过）：wolvicmod ctest 17/17（test_readyq 删除后）；30k 排
0 失配；coremark 全程 `--dut=both` 共栖交叉（rtl 5,106,742 / wolvic
5,423,490 检查各 0 失配，wolvic eval 35.36s 复证）；proj ctest 17/17；
wolvicmod cosim 30 组；proj cosim 51 组；`REPLAY_AUDIT=1` 全程审计回放。

## 16. 建模层整项化：PosTable 拍平 + CommitEntry 整项（2026-09-29）

**动机**：§15 后重采样显示框架壳与模型真活约五五开，壳侧已无单点可摘；
update_run（41.6%）的大头是 dj 各 Entry 的逐字段小动作。症结在建模本身：
PosTable 系把 RTL 的三层层次（PosTable→4×PosSet→16×PosEntry，128 个模块
实例）照搬进 C 模型，端口连接、per-entry 驱动 assign、16 宽 combine 数组
合计约 **1500 个框架动作**，干的活本质上只是 64 项/bank 的三字段状态 +
s0/s1 流水 + 几个 mux——C 模型里一句 for 循环。CommitEntry 同理：112 实例
× 6 个字段寄存器，每 posedge 672 次 update 派发，每条 lambda 只是一句
`w_set ? next : reg`。

**改法**（proj-xiangshan-l3/model/dj，对外端口逐位不变）：

- **PosTable 拍平**：删 PosEntry/PosSet 两个模块类。64 项表项状态并为
  `REG(std::array<PosEntryV, 64>)`——一条 update 循环算全数组 next
  （alloc/updTag/clean 按 hn_idx 在循环内匹配，wakeup 保持 RegNext 语义
  读旧 state）；每 set 的 8 个 s1/控制寄存器整项化为
  `REG(std::array<PosSetS1, 4>)` 一条 update；s0 组合逻辑从 per-set
  assign 变 6 条数组化 assign；出口（sleep/block/hn_idx/pos_resp/wakeup/
  addr_vec2/alr_use_pos/working）改为直读寄存器数组的汇聚 assign。优先级
  序（set 优先 way 次之）、更新条件、更新时机全部与原层次版逐位对应。
- **CommitEntry 整项**：6 个字段寄存器（task/flag/state/inst/alrGet/
  respErr）合并为单 `REG(V)`，一条 update 内按各自原条件写字段（w_set
  门控组 + 每拍直通组）；约 20 处 reads/解构机械改写，lambda 体原样
  （const 引用别名保名）；`state_out` 恒等连接变提取 assign。
- cosim harness_frontend 的内部窥探点同步到新结构（intcmp 与调试转储）。

**结果**（孤立回放，taskset -c 2）：

| 阶段 | 30k eval | 全程 eval | 全程 loop |
|---|---|---|---|
| §15 压缩后 | 3.573s | 37.39 / 35.30s | 38.92 / 36.84s |
| + PosTable 拍平 | 2.911s（-18.5%） | — | — |
| + CommitEntry 整项 | 2.554s（再 -12.3%） | **25.10 / 24.58s** | **26.62 / 26.10s** |

全程 eval **-30~32%**；累计对 491s 全量基线 **~19.6×**，对 RTL 差距
~9.1× → **~5.9×**。

**验证**（全部通过）：30k 排与 coremark 全程各 0 失配（全程 5,423,490
检查）；coremark 全程 `--dut=both` 共栖交叉（两侧各 0 失配）；proj
ctest 17/17；wolvicmod cosim 30 组；proj cosim 51 组；`REPLAY_AUDIT=1`
全程审计回放。

## 17. 建模层整项化（续）：TaskBuffer 拍平 + Commit 全拍平（2026-09-29）

**动机**：§16 的延续——同一病灶的另两处。TaskBuffer（req 16 + hpr 8
实例/bank）：TaskEntry 子模块 + TaskBuffer 里 per-entry 的
init_nid/oth_rel/s0_rdy 驱动 assign（每实例 ~16 个动作）。Commit：112 个
CommitEntry 实例，每实例 ~35 个动作（命中检测、次态线网、15 路输出端口
经 combine 接仲裁器）——全部是在 C 模型里用 for 循环就能表达的逻辑。

**改法**（对外端口逐位不变；仲裁器 Alloc/VipArb/QosRR 与 BackendDecode
子模块不动）：

- **TaskBuffer 拍平**：TaskEntry 类删除；`EntryV{task, nid, retryNum,
  timeout, validD1}` 数组并为 `REG(EntryArr)` 一条 update 循环；
  init_nid/oth_rel 的 N² 统计、s0_rdy/lockIdx 的锁定判定全部进循环内联
  （N≤16 的顺序比较远比框架动作便宜）；`w_alloc_rdy_all`/`w_s0_in`/
  `w_lock_all` 由 entries 数组直算。RegNext 语义（timeout、validD1）全部
  读旧值，与原版逐位对应。
- **Commit 全拍平**：CommitEntry 类删除；112 项状态并为
  `REG(std::array<V, 112>)` 一条 update；15 路 per-entry 输出变成数组
  wire（一条 assign 循环 112 项）直喂仲裁器，per-entry rdy 从各仲裁器的
  in_rdy 数组直读；命中检测/译码回灌/六组次态逻辑全部数组化；死端口
  `state_out` 删除。广播输入（rx_rsp/cm_resp 等）变化今天本来就唤醒全部
  112 个实例的同构小 assign，数组合并后总计算量不变、派发次数 -95%+。
- cosim harness_frontend 窥探点同步（TaskEntry 寄存器 → entries 数组成员）。

**结果**（孤立回放，taskset -c 2）：

| 阶段 | 30k eval | 全程 eval | 全程 loop |
|---|---|---|---|
| §16 终版 | 2.554s | 25.10 / 24.58s | 26.62 / 26.10s |
| + TaskBuffer 拍平 | 2.227s（-12.8%） | — | — |
| + Commit 全拍平 | 1.970s（再 -11.6%） | **20.53 / 20.51s** | **22.05 / 22.03s** |

全程 eval **-17%**（对 §16）；累计对 491s 全量基线 **~23.9×**，对 RTL
差距 ~5.9× → **~4.8×**。两轮整项化（§16+§17）合计：eval 35.3–37.4 →
20.5s，**-44%**。

**验证**（全部通过）：30k 排与 coremark 全程各 0 失配（全程 5,423,490
检查）；`--dut=both` 共栖交叉（两侧各 0 失配）；proj ctest 17/17；
wolvicmod cosim 30 组；proj cosim 51 组；`REPLAY_AUDIT=1` 全程审计回放。

## 18. 建模层整项化（三）：剩余表项阵列全清（2026-09-29）

**动机**：§16/§17 后的 profile 复核（按类聚合）拎出所有残余的 RTL 层次
照搬表项：ReplaceEntry（64×2）、ReadEntry（64×2）、WriteEntry（32×2）、
SnoopEntry（32×2）、DataCtrlEntry（64×2）、BridgeCm（64+8）、
BeatStorage（8×2）。合计 profile 占比 ~12.5%。

**改法**（同一拍平模式，对外端口全部不变；仲裁器/队列/SRAM prefab
保留）：

- **ReplaceEntry → ReplaceCM 拍平**（replace.h/cpp）：ReplReg 并入
  `REG(std::array<ReplReg, 64>)`，一条 update 循环（十八态 FSM + 四类
  命中副作用逐句移植）；11 路 per-entry 输出变数组 wire 直喂仲裁器；
  reqPoS 矩阵的 per-entry rdy 选择合并为一条 `w_req_pos_rdy_all` 数组
  assign；`upd_id`/`resp` 两路 rdy 恒真内联。
- **CM 三件套**（cm.h/cpp）：SnoopEntry/ReadEntry/WriteEntry 三类删除，
  各并为父模块 REG 数组 + 一条 update；fire 条件中纯 state 函数的 valid
  化简；hit 检测内联进循环。
- **DataCtrlEntry → DataCM 拍平**（data.h/datacm.cpp）：64 项并为
  `REG(CtrlArr)`；9 条 combine 变 9 条数组 assign；64×3 条 rdy 回接变
  3 条数组 assign；harness_db 窥探点同步。
- **BridgeCm 拍平**（bridge/）：与前三处不同——它是两桥共享的模板。
  BridgeCm 降级为普通 C++ 载体 `CmSt<Tr>`（状态 POD）+ `CmLogic<Tr>`
  （纯静态函数组），两桥各持 `REG(std::array<CmSt, kOutst>)` + 一条
  update + ~17 条数组 wire；共享逻辑零重复。harness_bridge 5 处窥探点
  同步。
- **BeatStorage 跳过**：状态主体是 SpSram（Mem 实体）+ ValidPipe
  prefab，都必须保留为子模块，可并的只有 3 个标量 reg（8 实例），收益
  为负；且 `tests/test_datablock.cpp` 白盒单测直接例化它。

**结果**（孤立回放，taskset -c 2）：

| 阶段 | 30k eval | 全程 eval | 全程 loop |
|---|---|---|---|
| §17 终版 | 1.970s | 20.53 / 20.51s | 22.05 / 22.03s |
| + 本批四处拍平 | **1.504s（-23.7%）** | **15.50 / 15.23s（-25%）** | **16.99 / 16.74s** |

累计对 491s 全量基线 **~32×**，对 RTL 差距 ~4.8× → **~3.4×**。

**验证**（全部通过）：30k 排与 coremark 全程各 0 失配（全程 5,423,490
检查）；`--dut=both` 共栖交叉（两侧各 0 失配）；proj ctest 17/17（总耗
时 47.9s → 17.8s，提速本身亦印证）；wolvicmod cosim 30 组；proj cosim
51 组；`REPLAY_AUDIT=1` 全程审计回放。

**注**：本轮由 3 个并行子代理（CM 三件套 / DataCtrlEntry / BridgeCm）
+ 主代理（ReplaceEntry）完成；ReplaceEntry 的派发代理曾陷入工具调用死
循环（769 次重复 grep、零编辑），被人工中止后由主代理接手——与任务
本身无关。

## 19. 预制件拍平：Queue 一族融合 + ValidPipe/FastQueue 静止门控（2026-09-29）

**动机**：§18 后的共栖 A/B 回放分层 profile（wolvic solo 全程，perf 符号
按"宿主类"归因）显示引擎已收敛到 ~30%，最大单点变成预制件原语群
~23.7%——其中 wolvicmod `Queue` 一族 10.6%（每个 2 深度队列 4 条 NBA
update + 8 条组合 assign，逐拍派发+逐拍脏检测）、`ValidPipe` 3.4%
（无门控，每拍整 StageArr 拷贝）、`FastQueue` 2.3%（同为无门控整状态
拷贝）。引擎调度循环（`Module::eval`）18.2% 与脏检测簿记 ~12% 中的一
部分也是这些多余 action 的派生开销。

**改法**（端口/语义全部不变，chisel 状态机逐拍等价）：

- **Queue 拍平**（wolvicmod `prefab/queue.h` 重写）：`Mem ram` + 三个
  指针 reg 合并为单个 `REG(QState)`（ram 数组 + enq_ptr/deq_ptr/
  maybe_full）；`w_empty/w_full/w_do_enq/w_do_deq/w_ptr_chg` 五条中间
  wire 取消，空/满/fire 判断在消费点内联（共享一个 `static fire()`
  帮助函数）；4 条 NBA update 融合为 1 条，并由新增 `w_chg =
  do_enq || do_deq` 门控——静止拍 compute 与整状态提交全跳过。每实例
  动作数 **12 → 5**（deq / enq_rdy / count / w_chg / update）。原 Mem
  "写即脏"变为 REG 提交相等检测，同值写不再传播脏。
- **ValidPipe 活性门**：新增 `w_live = enq.valid || ∃stage.valid` 门控
  update——管线全空且入口静止时移位是恒等操作，整条 update 休眠。
- **FastQueue 静止门**：新增 `w_chg = w_enq_fire || w_deq_fire` 门控
  update（不 fire 时 next == st，FqState 整拷贝可省）。

**结果**（孤立回放全程 316,748 拍，taskset -c 2，两次取优）：

| 阶段 | 全程 eval | 全程 loop | 对 RTL eval 比 |
|---|---|---|---|
| §18 终版 | 15.39s | 16.90s | 5.1× |
| + 本轮 | **11.16s（-27.5%）** | **12.62s（-25.3%）** | **3.7×** |

超出事前估计（-8~12%）：Queue 自身 compute 10.6% → 4.2%，并联动摊薄了
派发循环与脏检测（action 总数下降）。层分布（perf 占比 ×墙钟）：
预制件-wolvicmod 2.49s → 0.92s（-63%），Queue 子项 10.55% → 4.19%、
ValidPipe 3.39% → 2.24%、FastQueue 2.28% → 1.07%。剩余热点前移为
HNF 业务逻辑（Directory/Commit/TaskBuffer）与 VipArb（4.1%）。

**验证**（全部通过）：30k 排与 coremark 全程各 0 失配（全程 5,423,490
检查）；`--dut=both` 共栖交叉（两侧各 0 失配）；`REPLAY_AUDIT=1` 全程
审计回放；wolvicmod ctest 17/17（含 Queue 黑盒单测 + 全元件 audit 巡
查）；proj ctest 17/17（17.8s → 13.5s）；wolvicmod cosim 30 组；proj
cosim 51 组。

**产物**：perf 采样 `build/perf-ab-layers.data`（本轮前基线）、
`build/perf-ab-flatq.data`（本轮后）；分层归因脚本
`build/bucket_layers.py`（flat 报告 → 层/宿主类聚合，用法见文件头）。

## 20. HNF 离散信号按行为语义合并：struct 化消灭独立 Wire/Reg（2026-09-29）

**动机**：§19 后的层分布显示 HNF 业务逻辑升为最大桶（26.9%），其中大量
成本不是算术本身，而是离散小信号的**每动作固定成本**（派发 + 读集打包
+ 脏检测）：directory.h 45 WIRE/18 REG、data.h 39/34、commit.h 30/8、
frontend.h 24/11、replace.h 16/1。方针（与 §19 Queue 拍平同源）：**按
行为语义把能合并的变量合并成 struct，尽量避免独立的 Wire/Reg**。

**合并准则**：同沿更新、使能同源的 reg 并为一个 struct + 一条 update；
同一组合链/读集高度重叠的 wire 并为 struct wire；单消费平凡中转线内联；
读集不相交的不硬并（防误触发）；消费方是子模块端口恒等别名的不并
（字段提取反而 +动作）；大载荷避免过触发，bool/小数组可放宽。

**改法**（端口/层级/语义全部不变；样板由主代理先做、4 个并行子代理
分模块推广）：

- **BackendDecode×2**（commit.h/cpp，样板）：7 reg + 4 wire →
  `St{decVal, mes, hnIdVal, hnId, decList, taskCode, cmtCode}` 1 reg +
  1 条 update + 4 条输出 assign，两级流水译码查表全内联。
- **DirectoryBase**（directory.h，llc 67→33 / sf 69→35 动作，全
  Directory 272→136，-50%）：`Sft`（三移位器+req 载荷）、`D3`/`D4`
  （d2→d3→d4 同使能流水寄存，d2 级中转线内联）、`Bp`、`WriD0`、
  `RamReq`、`Rec`、`Match`、`SelWay` 等 12 组；`rst_done`/`lock_tab`/
  `rsv_tab` 保留（使能各异/大数组）。
- **data 组**（184→124 动作，-33%）：BeatStorage `St{双移位+rstDone}`
  ×8 实例；DBIDPool `Rst`/`Cnt`；DataBuffer 26 update → 6（写口提交
  8 reg→`WrReg`、读控制 4 reg→`RdCtl`、两级流水 12 reg→`ChiPipe`/
  `DsPipe`）；DataCM `TaskD`/`Flags`/`Views`/`Sel` + 三条 rdy 数组
  内联进 entries update。
- **frontend 组**：Block 4 reg→`St` + 6 wire→`BlkW`（15→7 动作）；
  FrontendDecode 3 reg→`St` + 6 wire→`DecW`（15→6）。
- **replace + 收尾**：ReplaceCM `DirHits`/`ReqPosFeed` 合并 +
  `w_req_pos_out` 中转消除 + entries update 读集瘦身（27→17 reads，
  输出数组 valid ≡ state 由 switch 分支隐含）；ChiXbar 4 条 rdy 直通
  改恒等别名（14→10）；QosRRArb 删 `w_has_high` 中转（6→5 ×27 实例）；
  backend 无可并对象跳过。
- **白盒窥探点同步**（§18 先例）：`verify/cosim/harness_dir.cpp`、
  `harness_db.cpp`、`harness_frontend.cpp`；tests/ 只触端口零改动。

**结果**（孤立回放全程，taskset -c 2，两次取优）：

| 阶段 | 全程 eval | 全程 loop | 对 RTL eval 比 |
|---|---|---|---|
| §19 终版 | 11.16s | 12.62s | 3.7× |
| + 本轮 | **9.93s（-11.0%）** | **11.43s（-9.4%）** | **3.2×** |

对 §18 终版（15.39s）两轮累计 **-35%**。层分布：HNF 桶 26.9% → 24.2%
（墙钟 3.39s → 2.76s），其中 DirectoryBase 3.5% → 2.0%；引擎派发循环
16.6% → 14.0%（action 总数下降的联动收益）。剩余热点：Commit
（compute+commit 6.5%，112 项大数组逐拍提交——大数组按项脏位图是预
留方向）、VipArb 4.8%、TaskBuffer 2.6%。

**验证**（全部通过）：30k 排与 coremark 全程各 0 失配（全程 5,423,490
检查）；`--dut=both` 共栖交叉；`REPLAY_AUDIT=1` 全程审计；proj ctest
17/17（13.5s → 12.7s）；proj cosim 51 组（含三个改动的窥探点
harness）。本轮无 wolvicmod 框架侧改动，wolvicmod ctest/cosim 沿用 §19
结论。

**产物**：perf 采样 `build/perf-ab-hnfmerge.data`（本轮后）；
`build/perf-ab-flatq.data`（§19 后）、`build/perf-ab-layers.data`
（§19 前基线）。

## 21. 大数组 update 全量门控：活跃度实测否决按项脏位图（2026-09-29）

**动机**：§20 后 Commit（compute+commit 6.5%）成为最大单点，候选方向是
"大数组按项脏位图"。**先做活跃度测量再定方案**（临时探针 actprobe，
测完即删）：全程 316,748 拍 ×2 HNF 实例，每条数组 update 的执行次数、
有变化的拍数、每拍变化项数。

**测量结论**（coremark 全程，100% 覆盖非抽样）：

| 数组 | 有变化的拍 | 每拍变化项 |
|---|---|---|
| Commit.entries (112) | 0.3% | 变时 1-2 项 |
| DataCM.entries (64) | 0.0% | — |
| ReplaceCM.entries (64) | 0.1% | 变时 1-2 项 |

L3 在此负载下高度静默，且 commit 的 `==` 检测已让下游 assign 在静止拍
休眠——剩余浪费是 **update 自身的每拍全数组拷贝+循环+比较**（Commit
每拍 ~3×9.4KB）。按项脏位图优化的是活跃拍内部（算完比较完才知道位
图），救不了这个；**整条 update 事前门控**才是正解（且在任何负载下
都不亏：闲时大省、忙时只多一条归约 wire）。位图保留为多核/饱和负载
下的未来选项。

**改法**：6 处数组 update 各加一条 `w_any` 归约 wire + `.en(w_any)`，
lambda 本体不动。候选条件按模块语义：

- **Commit**：`|w_set || |w_comp_ack_hit`——关键正确性点：comp_ack_hit
  不看项有效性（纯 txnID 匹配），空闲项的 alrGet 也会被击中翻转，漏掉
  它会丢状态（逐拍复现义务，不能按"看起来无害"省略）；
- **DataCM**：`∃非空闲 || (reqFire && free_sel.has)`；
- **ReplaceCM / SnoopCM / ReadCM / WriteCM**：循环体已有的
  `allocFire || state != kFree` 候选条件的归约。

**门控精度**（探针复核）：Commit update 执行 633,496 → 4,406 次
（0.7%，含 comp_ack_hit 保守唤醒；实际变化 1,786 次全保留）；
DataCM → **0 次**（全睡且 0 失配）；ReplaceCM → 737 次（99.3% 精确）。

**结果**（孤立回放全程，taskset -c 2，两次取优）：

| 阶段 | 全程 eval | 全程 loop | 对 RTL eval 比 |
|---|---|---|---|
| §20 终版 | 9.93s | 11.43s | 3.2× |
| + 本轮 | **8.04s（-19.0%）** | **9.50s（-16.9%）** | **2.5×** |

HNF 桶墙钟 2.76s → 1.42s（-49%）；Commit/DataCM/ReplaceCM/CM 三件套
从前排热点消失，新热点为 TaskBuffer 3.2%、DirectoryBase 2.7%、
PosTable 1.9%（同为数组 FSM 但候选几乎恒非空，门控不适用）。

**验证**（全部通过）：30k 排与 coremark 全程各 0 失配（全程 5,423,490
检查）；`--dut=both` 共栖交叉；`REPLAY_AUDIT=1` 全程审计；proj ctest
17/17（12.7s → 10.3s）；proj cosim 51 组。

**产物**：perf 采样 `build/perf-ab-gated.data`（本轮后）。

## 22. 仲裁器/SRAM prefab 拍平：VipArb 中转链内联 + SRAM 单链内联（2026-09-29）

**动机**：§21 后 prefab 层剩余两个未拍平对象——VipArb（9 动作/实例，
~45 处使用、含 QosRRArb 内含的 2 个，实例 ~60-80）与 SpSram/DpSram
（~15 动作/实例）。RRArb（5 动作且 w_out_fire 已是门）、Alloc/
FixedArb（纯组合 3 动作）评估后跳过。

**改法**（端口/语义不变，全部机械内联）：

- **VipArb**（xsarb.h）：`w_vip_req`/`w_other_v`/`w_out_fire` 三条单消费
  中转并入 `w_move`（直读 vip/in/out/out_rdy 一次融合扫描），`w_next_vip`
  并入 vip.update 的 compute——9 → **5 动作/实例**（3 输出 + 门 + update）。
- **QosArb**：`w_has_high` 中转消除，4 个消费方直读 `arb_hi.out.valid`
  （同 §20 dj/qosrr.h 的处理）。
- **SpSram**：`w_fire`/`w_read_fire` 单链内联（write_fire 融合为一条；
  读 fire 并入 holdpipe/cappipe enq；intv 的 fire 判定直读 req/req_rdy）。
  `w_addr` 保留（Mem update 的 `.addr()` 需实体）。intv/rst 标量 update
  **不加门**——门本身是一条新 action，省下的标量 compute/比较是平凡
  成本，净收益为负（与 §21 大数组门控不同，那里省的是每拍 3×9.4KB）。
- **DpSram**：`w_raddr`（rreq.bits 直通）与 `w_bmask`（单消费）并入
  cappipe.enq（BypassWrite 掩码逻辑 if constexpr 内联）。

**结果**（孤立回放全程，taskset -c 2，两次取优）：

| 阶段 | 全程 eval | 全程 loop | 对 RTL eval 比 |
|---|---|---|---|
| §21 终版 | 8.04s | 9.50s | 2.5× |
| + 本轮 | **7.82s（-2.7%）** | **9.28s（-2.3%）** | **2.5×** |

收益小于事前估计（4-7%）：perf 复核显示 VipArb 桶墙钟仅 0.50s → 0.44s
——其成本大头是 3 条输出 assign（chosen/out/in_rdy 各自 N 路扫描）在
输入变化时的**真实计算**，派发开销占比小；拍平消掉的 4 条中转 wire 只
是次要成本。SRAM 桶持平（同理）。-2.7% 主要来自动作数下降对引擎派发
循环/守卫桶的联动摊薄。**教训**：拍平的收益与"中转成本占比"成正比，
输出侧多路扫描类 prefab 的拍平空间天然有限。

**验证**（全部通过）：30k 排与 coremark 全程各 0 失配（全程 5,423,490
检查）；`--dut=both` 共栖交叉；`REPLAY_AUDIT=1` 全程审计；proj ctest
17/17；proj cosim 51 组。

**产物**：perf 采样 `build/perf-ab-arbflat.data`（本轮后）。

## 23. 静止门推广：TaskBuffer / PosTable / DirectoryBase 全覆盖（2026-09-29）

**动机**：§21 只门控了 6 处数组 update，当时把 TaskBuffer/PosTable/
DirectoryBase 判为"候选几乎恒非空，门控不适用"——这与 §21 自己的测量
（coremark 下 L3 ~98% 拍静默）矛盾，是错误判断。这三者正是 §22 后的
HNF 前排热点（TaskBuffer 3.2%、DirectoryBase 2.7%、PosTable 1.9%）。
本轮补齐，门控覆盖 HNF 全部数组/流水状态 update。

**改法**（同 §21 模式：w_any 归约 wire + `.en()`，lambda 本体不动）：

- **TaskBuffer**（×2）：`∃(state≠kFree || validD1) || ∃alloc`。旁路排查
  出 `validD1` 是 state 的一拍延迟、每拍自清——末项释放后的清零拍也
  必须唤醒，否则 validD1 滞留引发 othRel 旁路误触发（本轮最易漏点）。
- **PosTable**（s1/entries 两条各一门）：候选含 `upd_tag`/`clean` 的
  hnIdx 纯匹配命中（不看表项有效性，同 §21 comp_ack_hit 教训）与
  `wakeup` 自清脉冲项。
- **DirectoryBase**（×4 实例）：`sft` 门 = 移位器任一级非零 || d0 fire；
  `d3`/`d4` 取精确使能（req(D2)/req(D3) 位）；`bp` 门补 `bp.d1d4` 自清
  脉冲；`lock_tab`/`rsv_tab` 候选与原 lambda 内部条件逐项相同。

**结果**（孤立回放全程，taskset -c 2，两次取优）：

| 阶段 | 全程 eval | 全程 loop | 对 RTL eval 比 |
|---|---|---|---|
| §22 终版 | 7.82s | 9.28s | 2.5× |
| + 本轮 | **6.63s（-15.2%）** | **8.05s（-13.3%）** | **2.2×** |

HNF 桶墙钟 1.32s → 0.48s（-64%），HNF 从第二大桶降为与 CcSocket 同
量级。新热点全部是"真实工作"：引擎派发 18.8%、prefab 扫描/移位
（VipArb/Queue/ValidPipe/SRAM）21%、Ring 9.3%、harness 14.7%。

**验证**（全部通过）：30k 排与 coremark 全程各 0 失配（全程 5,423,490
检查）；`--dut=both` 共栖交叉；`REPLAY_AUDIT=1` 全程审计（覆盖门控最
易错的"门关闭但状态应变"场景）；proj ctest 17/17（10.5s → 9.1s）；
proj cosim 51 组。

**产物**：perf 采样 `build/perf-ab-gateall.data`（本轮后）。

## 23b. 静止门推广（续）：Ring / CcSocket / AXI 桥全覆盖（2026-09-29）

**改法**：同 §23 模式推广到 HNF 之外——Ring 200 条 update（30 个
ChannelTap ×（2 SingleTap + 2 EjectBuffer + 2 VipTable）+ 20 RingPipe）、
CcSocket 12 条（PdcTx/Rx）、桥 3 条（AxiDataBuffer 64 槽、snode cms 64
项、hinode cms 8 项）。旁路排查出的典型陷阱（全部计入候选）：
SingleChannelTap 的 `out.valid/rsvd_valid` 自清打拍；VipTable 指针悬空
自走（上拍 rel 清掉指针项后本拍指针照常搬移）；EjectBuffer 的
`upd_rel != w_enq_rdy` 双方向精确门；AxiDataBuffer 的 `icn.valid` 广播
命中（不看 ctrl_valid，comp_ack_hit 同类）；cms 的 `wk_vld_reg` 自清。

**结果**（孤立回放全程，taskset -c 2，两次取优）：

| 阶段 | 全程 eval | 全程 loop | 对 RTL eval 比 |
|---|---|---|---|
| §23 终版 | 6.63s | 8.05s | 2.2× |
| + 本轮 | **5.00s（-24.6%）** | **6.28s（-22.0%）** | **1.7×** |

Ring 桶 9.3% → 6.6%（墙钟 -55%），AXI 桥 5.5% → 0.6%（-92%）。层分布
前列收敛为：引擎（派发 15.0% + 脏检测/边沿/守卫 ~18.7%）、prefab 真实
扫描工作（22.3%）、harness（18.8%）、HNF（7.3%）。**模型全部业务状态
update 现已尽数带静止门**（仅存 dongjiang.cpp 三个标量 reg，按 §22
教训不值得加门）。

**验证**（全部通过）：30k 排与 coremark 全程各 0 失配（全程 5,423,490
检查）；`--dut=both` 共栖交叉；`REPLAY_AUDIT=1` 全程审计；proj ctest
17/17（9.1s → 7.2s）；proj cosim 51 组。

**产物**：perf 采样 `build/perf-ab-gatering.data`（本轮后）。

## 24. 路线记录：粒度税与"写细、跑粗"（2026-09-29 讨论定调）

**背景判断**（数据支持）：当前模型与 RTL 1:1 信号级对应，对时钟精确建
模而言粒度过细。引擎 33.7% 开销是**粒度税**——按信号/action 数线性
计费的运行时固定开销，与每拍真实计算量无关。Verilator 把同样的细粒度
在编译期消化（拓扑排序 + 内联成直线代码）；wolvicmod 为保留 lambda
化高层次描述，把税放在运行时逐拍支付（派发/脏检测/边沿/守卫，静止
时照付）。§19/§22 拍平收益即直接证据：删掉只为镜像 RTL 结构的中转
信号，省下的主要是引擎侧开销。

**约束修正**：cmodel 的真实契约是**边界信号行为逐拍一致**；内部 1:1
对应只是开发期调试资产（cosim 探针把失配定位压缩到分钟级），不应构成
运行时约束。细粒度降级为 **debug 编译模式**（探针/cosim 开关，默认关
闭、零运行时成本）——"写细、跑粗"。

**两条路线（乘法关系，非替代）**：

- **A. 引擎侧**（一次性，全模型受益）：
  - **A① 模块级静止跳过**：per-update 静止门（§21-§23b）提升到
    per-module/cluster 脏跳过——§23 实测各数组活跃度 0~0.3%，整拍无
    事是常态，模块任一输入/内部脏才求值，否则整个模块的派发/守卫/
    边沿全跳。不动求值架构，改动集中在 engine 调度层。
  - **A② 模块内 action 融合**：elab 后对模块内 update lambda 拓扑排
    序、融合成单一 eval 函数（类 Verilator eval），派发 15% 直接消
    失，脏检测粗化到模块粒度。框架级大改（sim/engine.h + action/expr
    体系），需框架自身测试兜回归。
- **B. 模型侧**（逐热点、边界等价的行为级重写）：prefab 22% 的"真实
  工作"只是在 RTL 形状算法下真实——ValidPipe 移位 O(深度)、Queue/
  VipArb 扫描 O(N)、SRAM 全阵列读写；行为级等价物可做到 O(活跃项)，
  边界时序逐拍不变。这是引擎做任何事都拿不到的**算法复杂度收益**。
  每个重写以全程 0 失配 + `REPLAY_AUDIT=1` 守边界契约。
- **不在路线内**：harness ~19%（只影响回放器 wall time，不影响 emu
  用途的模型速度）；整 L3 行为级重构（放弃时钟级对应，仅当 L3 仅作
  背景负载时合理，当前 L3 是被研究对象，不适用）。

**当前预算分配**（perf-ab-gatering，§23b 终版，全程 5.00s eval）：

| 层 | 占比 | 主要构成 |
|---|---|---|
| 引擎（A 的目标） | 33.7% | Flat 调度 15.0 + 脏检测 9.2 + 边沿 4.2 + 守卫 4.0 + 其它 1.4 |
| prefab（B 的目标） | 22.3% | zj 11.3（VipArb 5.7 / SpSram 2.6 / FastQ 1.3 / DpSram 1.3）+ wv 11.0（Queue 6.3 / ValidPipe 3.4 / RRArb 0.9） |
| harness | 18.8% | 驱动/解析/比对 10.7 + IO(printf/getc) 8.1 |
| 业务逻辑 | 15.5% | HNF 7.3 + Ring 6.6 + AXI桥 0.6 + CcSocket 0.6 + 其它 0.4 |
| libc 待归因 | 7.0% | 含 memmove 1.5（部分疑为 SRAM 阵列拷贝） |

**执行顺序**：A① 先行（便宜、确定性高）→ B 从 perf 榜首 prefab 试点
（Queue 6.3% / VipArb 5.7%）→ A② 框架大改最后。两条线互不阻塞。

## 25. 引擎 A①实施：守卫预过滤 watch 派发（2026-09-29）

**量化先行**（新增 `dbg/stats.h`，`WV_STATS=1` 环境变量启用，30k 排）：
每拍 update 激活 **1087 条**，其中边沿命中但守卫为假、空跑退出
**900 条（82.8%）**；真火 165 条；读集脏唤醒（无边沿）仅 21 条；
assign 激活仅 64 条/拍。**每拍成本几乎全是 update 的集体空跑唤醒**——
clk posedge 时 `markDirtyBool` 对全部 posWatch 项置位（O(N) 位图写），
扫描逐条虚派发、边沿检测、守卫读取，83% 发现无事可做退出。这就是
"引擎 34%"的主体，也是 §24 粒度税的精确形态。

**改法**（wolvicmod 框架，`b1750c9` 之后）：方向监视表项
`EdgeWatchDep` 增带守卫（实体 + 读取函数，`UpdateAction` 的 GuardSlot
经 `Action::EdgeWatch` → `buildExecOrder` 监视池一路下绑）。bool 写入
点匹配方向跳变时先读守卫：假则不置位、不派发，仅把 `prev` 拨到新值
（与 `run()` 被唤醒后所做的事逐字相同，边沿就此消费）。`markDirty()`
（方向未知）不过滤。

**正确性条件**：守卫在时钟脉冲时刻保持稳态值（settle-then-pulse 惯例：
输入 poke → eval 沉定 → clk poke）。排查全部驱动方：回放器 /
test_trace_replay / test_wolvic_top_replay 本就合规；**emu DPI glue 原
为单 eval 同批 poke（输入与 clk 同批，守卫读必为旧值），补上沉定
eval**；三个单测文件（sram/datablock/ring）的 poke-then-edge 由
`test_prefab_common.h` 的 `edge()` 统一前置沉定 eval 修复（该 header
自己的注释本就写着"每拍 = set 输入 → comb → edge"，违规的是测试）。
框架规划文档 §5.2 已同步记录该不变量。

**结果**：update 激活 1087 → 187 条/拍（edgeOnly 27.0M → **0**；
edge+guard 4,967,845 一条不差——证明只消去了空跑）。孤立回放全程：

| 阶段 | 全程 eval | 全程 loop | 对 RTL eval 比 |
|---|---|---|---|
| §23b 终版 | 5.00s | 6.28s | 1.7× |
| + 本轮 | **1.97s（-60.5%）** | **3.19s（-49.2%）** | **0.57×（首次快过 RTL）** |

累计（15.39s 基线 → 1.97s）：**-87.2%**。

**验证**（全部通过）：30k 与全程各 0 失配（5,423,490 检查）；
`--dut=both` 交叉；`REPLAY_AUDIT=1` 全程；wolvicmod ctest 17/17；
proj ctest 17/17（修复测试驱动后）。

**产物**：`build/perf-ab-guardwatch.data/.txt`（本轮后，含 dwarf 调用图）。

## 26. B 线试点：VipArb valid 拆分（SoA）+ B 线重定性（2026-09-29）

**改法**（`prefab/xsarb.h`，文件内自闭合，接口不动）：VipArb 的
chosen / in_rdy / w_move / vip.update 原本直接扫 `InArr`（跨步读
Valid<T>，任一路 bits 翻动即全员唤醒）。新增密集 valid 拷贝线 `w_v`，
四个动作改读 `w_v`：bits 翻动不再唤醒它们，扫描变连续 bool；`out.valid`
取 `in[chosen].valid`（与全路归约等价：chosen 恒指向首个 valid 路）；
`in_rdy` 改单索引写（`rdy[chosen]` 唯一可能置位）。`in` 的读者只剩
w_v 抽取与 out（bits 本就要随数据动）。

**结果**：全程 eval 1.97s → **1.84s（-6.7%）**，0 失配（验证链同 §25，
cosim 51 组含 viparb 对拍）。

**B 线重定性**（本轮 perf 归因的重要修正）：§25 落地前的"prefab 真实
扫描 22%"其实大部分仍是粒度税——它坐在 prefab 模板实例化的
UpdateAction 里，是被时钟唤醒的空跑：Queue 6.27% → 0.6%、Ring 6.6% →
0.2%（§25 后）。**§24 对 B 线"22% 真实工作"的估计作废**；A① 与 B 不
是乘法而是大部分重叠。B 线剩余真实空间 ≈ 4.8%（zj prefab 4.2% + wv
0.6%），Queue 惰性 bits / SRAM 按址访问的接口手术收益覆盖不了风险，
本轮起挂起。

**当前层分布**（perf-ab-soa，全程 eval 1.84s）：

| 层 | 占比 | 备注 |
|---|---|---|
| harness | 32.6% | 回放器固有（驱动/比对 16.3 + printf/getc IO 16.3），与 emu 无关 |
| 引擎 | ~36% | 脏检测 15.9（含预过滤环每 posedge ~1087 次守卫读）+ Flat 调度 11.7 + 守卫 4.7 + 边沿 1.5 + 其它 2.9 |
| libc 待归因 | 12.0% | 真载荷拷贝/比较（flit、数组） |
| HNF 业务 | 7.2% | 真实计算 |
| prefab | 4.8% | 真实扫描残余 |

**下一步候选**（按当前数据排序）：① 引擎脏检测 15.9%——预过滤环本身
每 posedge 读 ~1087 个守卫，可做层次化（簇守卫，A② 方向）；② Flat
调度 11.7%；③ harness IO（printf/getc 16%，缓冲化即可，但只影响回放
器 wall time）。

**产物**：`build/perf-ab-soa.data/.txt`（本轮后）。

## 5. 数据产物与复现

**留存的二进制**（对比实验免重建，`make stash-emu NAME=<变体名>` 约定）：

```
proj-xiangshan-l3/build/emu-variants/emu-wolvic-1t        # wolvicmod L3, EMU_THREADS=1（优化①前）
proj-xiangshan-l3/build/emu-variants/emu-rtl-1t           # RTL ZhuJiang, EMU_THREADS=1
proj-xiangshan-l3/build/emu-variants/emu-wolvic-1t-dirty  # wolvicmod L3 + 优化①（脏驱动求值）
proj-xiangshan-l3/build/emu-variants/emu-wolvic-1t-split  # 优化① + 边界 o_out 拆分（§7，收益≈0）
proj-xiangshan-l3/build/emu-variants/emu-wolvic-1t-push   # 优化①+②+③（推送派发，§8）
```

**perf 数据**（`proj-xiangshan-l3/build/`）：`perf-wolvic-1t.data`（1T 5万拍）、
`perf-rtl-1t.data`、`perf-wolvic.data`（16T 10万拍）、`perf-rtl.data`（8T 10万拍）、
`perf-wolvic-1t-split.data`（优化①+② 1T 5万拍，带 dwarf 调用图，1.9GB）；
孤立回放采样（§9）：`perf-zjrtl-replay.data`、`perf-wolvic-replay.data`；
引擎变体采样：`perf-wolvic-replay-flat.data`（§11 Flat）、
`perf-wolvic-replay-alias.data`（§12 Flat+别名化）。

注意：perf.data 按绝对路径解析符号，二进制被覆盖后须用
`perf report --symfs=<dir>` 指向留存副本（先把留存二进制按记录的绝对路径
铺进 symfs 目录树）。

**复现命令**：

```bash
# 构建某变体（以 wolvic 1T 为例；emu.mk 默认变量见 proj Makefile）
cd proj-xiangshan-l3/XiangShan && export NOOP_HOME=$PWD
make emu CONFIG=DefaultConfig LLC=ZhuJiang EMU_THREADS=1 WOLVIC_ZJ=1 \
  RTL_INCLUDE=$PWD/../dpi/sv \
  USER_LDFLAGS="$PWD/../build/libwolviczj_dpi.a $PWD/../build/libzjmodel.a \
                $PWD/../build/wolvicmod/third_party/libfst/libwolvicmod_fst.a -lz" -j32

# 测速（5 万拍窗口）
./build/emu -b 0 -e 0 -C 50000 -i ./ready-to-run/coremark-2-iteration.bin \
  --diff ./ready-to-run/riscv64-nemu-interpreter-so

# 采样
perf record -F 999 -o build/perf-<变体>.data ./build/emu -b 0 -e 0 -C 50000 ...

# 留存
make -C proj-xiangshan-l3 stash-emu NAME=<变体名>
```

**变体血统校验（务必在采数前做）**：`build/rtl` 不随 `WOLVIC_ZJ` 参数自动再生，
绕过 proj Makefile（直接 `make -C XiangShan emu`）可能拿到 stale RTL 变体——
`rm -rf build/verilator-compile` 只重 verilate，不会重新 elaborate RTL。采数前
用指纹确认二进制血统（本优化期间曾因此误把 RTL 变体的 7.69 ms/拍记为 wolvic
成绩，后由指纹检查识破）：

```bash
strings -a <emu> | grep -c DataCM.scala   # wolvic 版 = 0；RTL 版 > 100
nm <emu> | grep -c wolvic_zj_step         # wolvic 版 = 3（含调用点）；RTL 版 = 0
cat XiangShan/build/.llc-config           # wolvic 版 = "ZhuJiang-wolvic trace="
```

正确重建入口是 `make -C proj-xiangshan-l3 emu WOLVIC=1 [ET=<线程数>]`——
印记不匹配时自动 clean + 重新 elaborate。
