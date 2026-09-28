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
| RTL ZhuJiang | 8 | 1.00 µs | **316s** | full；两次独立复现 313s/316s |
| wolvicmod L3 | 16 | 2.67 µs | 876s | full；**wolvic 版最优点**（优化①前） |
| wolvicmod L3 | 8 | 3.83 µs | 1212s | full（优化①前） |
| wolvicmod L3 | 1 | 11.65 µs | ~3690s（推算） | 50k 实测 582.5s 推算（优化①前） |
| **wolvicmod L3（优化①后）** | 1 | **9.13 µs** | ~2890s（推算） | **50k 实测 456.3s**；dirty-eval + 去间接调用 |
| wolvicmod L3（优化①+②） | 1 | 9.04 µs | — | 50k 实测 452.0s/452.8s；边界拆分，收益噪声级（§7） |
| **wolvicmod L3（优化①+②+③）** | 1 | **8.93 µs** | — | **50k 实测 446.6s**；推送派发，IPC 0.842220（§8） |
| RTL ZhuJiang | 1 | 7.23 µs | ~2290s（推算） | 50k 实测 361.6s 推算 |

所有运行 cycleCnt 均为 316,801，指令流一致——差异纯粹是仿真宿主开销。

> **优化①结果（§6）**：wolvic 1T 11.65 → **9.13 µs/拍**，与 RTL 1T（7.23）
> 的差距从 4.42 收窄到 **1.90 µs/拍**（1.61× → 1.26×），符合 §4 方向①
> 原预期的 ~9 µs/拍——主贡献不是 dispatch 瘦身，而是脏驱动跳过调度
> （L3 模型每拍大部分逻辑静止，全被跳过）；模型侧 A/B 实测 2.87×。
> 剩余差距主要落边界结构桶（方向 2）。
>
> **优化③结果（§8）**：调度循环自身的轮询扫描反转成推送派发，模型侧再降
> 1.99×；emu 1T 9.04 → **8.93 µs/拍**，与 RTL 1T 的差距收窄到 **1.70 µs/拍**。

## 2. 核心拆解：1T 对 1T（无多线程同步干扰）

wolvic 版比纯 RTL 多出的部分 = 11.65 − 7.23 = **4.42 µs/拍**（1.61×）。
按 perf 符号归组 + 生成代码 zhujiang 信号归因（1569 个生成函数，覆盖 97.6% 采样）：

| 构成 | µs/拍 | 占多出部分 | 归属 |
|---|---|---|---|
| wolvicmod 模型 + DPI glue | ~2.5 | ~55% | **wolvicmod 框架可优化** |
| BlackBox 边界结构成本 | ~2.4 | ~55% | Verilator 侧 |
| （减去）省掉的 L3 RTL 工作 | −0.9 | — | RTL 版需付、wolvic 版省掉 |
| **净多出** | **~4.4** | | 对账：6.44+2.4+2.5≈11.9 ✓；6.44+0.9≈7.5 ✓ |

两侧共有的 SoC 基线 = **6.44 µs/拍**（RTL 侧 sequent 3.89 + comb 2.55，已扣除
zhujiang 归因份额 0.55+0.34）。

### 2.1 模型侧 2.5 µs/拍：框架调度粒度主导

- `wolvicmod::Module::eval()` 调度循环本身：**5.6%**（0.66 µs/拍）——遍历展平
  动作表 + 逐动作虚调用；
- 其余 ~15% 散布在**几千个** `AssignAction<T>::run()` / `UpdateAction<T>::run()` /
  `std::_Function_handler::_M_invoke`——每信号一个 action 对象、一次
  `std::function` 间接调用，单个 <0.4%，聚合是大头。
- 模型/glue 全部已是 **-O3**（CMAKE_BUILD_TYPE=Release，18 个源文件逐个核对）——
  优化空间在框架结构，不在编译选项。

### 2.2 边界侧 ~2.4 µs/拍：不是变量重组

- **变量重组实测免费**：in_pack 收集 + DPI 调用 + 输出拷贝（`nba_sequent__TOP__21`）
  仅 **0.17%**（~20ns/拍）；VlWide 包装、C++ 侧 pack/unpack 均 ~0.00%；
- 真实成本弥漫在两个桶里（相对 SoC 基线 sequent +1.55 / comb +0.82）：
  BlackBox 端口使 L2 侧供给逻辑（如 txdat 仲裁 mux）全部保持 live 无法死码消除、
  o_out NBA 更新后下游组合逻辑每拍整体重估（额外 settle 波次）。
- 注：zhujiang 归因按函数体是否引用 zhujiang 信号计，是**下界**（跨界内联的
  L3 逻辑不含该字样），故边界成本 ±0.5 µs 不确定度。

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
   1T 11.65 → 9.13 µs/拍，符合原预期（~9）；剩余差距集中在方向 2；
2. **边界结构**（~2.4 µs/拍）：~~o_out 按通道拆寄存器~~ **已实验证伪（§7）**——
   边界输出的下游 comb 被调度进 NBA 区、挂时钟位每拍无条件运行，与触发粒度
   无关；剩余路径（减少 BlackBox 端口暴露的无关字段以助 DCE、或将边界 comb
   移出 NBA settle 环）均需 Verilator 侧支持，"不改 Verilator"约束下暂无
   可行大收益路径；
3. **线程数**：当前 `WOLVIC=1` 默认 EMU_THREADS=16 已是最优点（优化①前：
   1T 11.65 / 8T 3.83 / 16T 2.67 µs/拍），无需调整；更多线程受同步主导预计
   收益递减；优化①后 MT 曲线需重新测量（每拍模型耗时大降，同步占比相对
   上升，最优点可能前移）；
4. 不建议动：`--threads-dpi none` 等 Verilator 分区旋钮，风险高收益不确定。

wolvic 1T 现已 8.93 µs/拍（RTL 1T 7.23，优化③后）；剩余 1.70 µs/拍差距落在
边界结构桶（方向 2），该桶已成为唯一的大头。

## 6. 优化①实施：脏驱动求值 + 去间接调用（2026-09-28）

**结果**：wolvic 1T **11.65 → 9.13 µs/拍**（50k 实测 456.3s，difftest 通过）。
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

**优化①后模型侧剩余成本实测**（9.04 µs/拍 拆分版，perf 50k 采样，
`build/perf-wolvic-1t-split.data`）：模型+DPI glue 合计 **9.7%**
（≈0.88 µs/拍；优化①前为 ~2.5 µs/拍 ≈21%）。构成：

- `wolvicmod::Module::eval()` 调度循环独占 **7.2%**（≈0.65 µs/拍）——
  脏求值后真正的 action 执行只剩 ~2.4%（≈0.22 µs/拍），但每拍仍要遍历
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

**结果**：50k 实测两轮 **452.0s / 452.8s（≈9.04 µs/拍）**，对比优化①基线
456.3s（9.13 µs/拍）——**−0.9%，收益噪声级，假设证伪**。拆分版二进制留存为
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
~1.9 µs/拍差距的主要嫌疑转为 NBA settle 环的每拍全量执行本身（需 Verilator
侧支持输出侧细粒度触发，或将 DPI 调用移出 posedge 块——后者改变语义，不可行）。

## 8. 优化③实施：推送派发（反向依赖表 + 层级桶就绪队列）（2026-09-28）

**动机**：优化①后模型侧最大开销已不是 action 执行（2.4%），而是
`Module::eval()` 调度循环本身（7.2% ≈0.65 µs/拍）——每拍遍历整个展平
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
- emu 50k（①+②+③ 全栈）：**446.6s（8.93 µs/拍）**，difftest 通过
  （IPC 0.842220）；对比①+②基线 452.0s/452.8s 为 **−1.2%**。收益幅度
  符合预期——emu 里模型侧占比已只剩 ~9.7%，调度循环大头被消除后总账
  改善 ~0.1 µs/拍；模型侧 1.99× 的价值主要体现在后续模型规模扩大时
  （调度成本从 O(动作数) 降为 O(活跃动作数)）。

**验证**（全部通过）：wolvicmod ctest 17/17（新增 test_readyq 三模式等价
用例）、wolvicmod cosim 30 组、proj ctest 17/17、proj cosim 51 组、
coremark 全程回放 `REPLAY_AUDIT=1` 审计、emu 50k difftest。

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
`perf-wolvic-1t-split.data`（优化①+② 1T 5万拍，带 dwarf 调用图，1.9GB）。

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
用指纹确认二进制血统（本优化期间曾因此误把 RTL 变体的 7.69 µs/拍记为 wolvic
成绩，后由指纹检查识破）：

```bash
strings -a <emu> | grep -c DataCM.scala   # wolvic 版 = 0；RTL 版 > 100
nm <emu> | grep -c wolvic_zj_step         # wolvic 版 = 3（含调用点）；RTL 版 = 0
cat XiangShan/build/.llc-config           # wolvic 版 = "ZhuJiang-wolvic trace="
```

正确重建入口是 `make -C proj-xiangshan-l3 emu WOLVIC=1 [ET=<线程数>]`——
印记不匹配时自动 clean + 重新 elaborate。
