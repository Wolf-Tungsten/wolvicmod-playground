# Wolvicmod 仿真引擎规划：Flat 调度

> 姊妹篇：《Wolvicmod 框架规划》（下称《框架规划》，引用其章节号为 §x.y）；
> 性能数据出处：`proj-xiangshan-l3/docs/verification-report.md` §3（原《拆解》`perf-breakdown.md` 已于 2026-09-30 文档整理时并入该处，原文可从 git 历史检索）。
> 本文档只涉及**引擎**（elaboration 之后的执行机制），不改变任何建模 API 的语义。
>
> **状态注记（2026-09-29）**：本文是 Flat 调度的规划文档，其实施与后续优化（别名化、
> 快速提交、成员位图提交、类型聚类）均已完成；随后代码压缩把 Flat 固化为**唯一引擎**，
> 文中"可回退 Push/Poll/Full"的 SchedMode 设计与全部 A/B 逃逸开关已删除
> （Full 仅作为 trace 模式的内部路径保留）。见《拆解》§15。

## 1. 背景：我们站在哪

经过《拆解》§7–§10 的四轮优化（脏驱动求值 → 推送派发 → 相位二推送化 + 边沿方向过滤），
wolvicmod 引擎在 ZhuJiang 孤立回放（coremark 全程 316,748 排）上的状态是：

| 指标 | 数值 |
|---|---|
| wolvic 侧墙钟 | 61.4–64.6 s（~199 µs/排） |
| 对 verilated RTL（4.25 s）的差距 | ~14.5×（从 20.6× 收窄） |
| 相对全量求值（491 s）的累计加速 | ~7.8× |

当前热点（《拆解》§10 profile）：

| 热点 | self% | 性质 |
|---|---|---|
| `Module::eval()` 调度循环 | 24.2% | 逐 action 弹出 + 虚调用 |
| `markDirtyBool()` 扇出推送 | 21.8% | 每次写信号遍历监视者表、操作就绪队列 |
| `AssignAction<bool>::run()` 等琐碎 action | ~4.6% | 纯赋值被包在虚函数里反复调用 |
| 推送路径容器操作 + `notifyIntent` | ~6.6% | 队列节点、push_back |
| 大结构体 `operator==` | ~2.5% | 实体粒度判等的直接成本 |

**结论：剩余成本的约一半不是"算"，而是"调度与派发"——这是引擎层的结构性开销，
无法在现有"逐 action 推送"范式内消除，需要换执行范式。**

## 2. 设计约束与不变量

1. **语义不变**：拓扑序执行、NBA 提交（相位二）、round 收敛判定、SCC 迭代语义、
   边沿方向过滤、优先级链顺序——全部沿用《框架规划》§4.3/§5.2 的既有不变量；
2. **模型写法解耦**：模型代码一行不改获得收益；
3. **可回退**：Flat 是 `SimState::SchedMode` 的新成员，旧路径（Push/Poll/Full）
   保留，trace/audit 模式继续强制 Full（《框架规划》§5.2 的分流不变）；
4. **等价可验证**：必须通过既有等价全家桶（§5）。

## 3. Flat 引擎：静态序 + 活性位图

### 3.1 核心思想

verilator 的运行时本质不是"生成了代码"，而是：**编译期定死的执行顺序 +
trigger 位图按位检查**，没有队列、没有按依赖逐 action 派发。wolvicmod 的
`execOrder`（elab/graph.h）已经是拓扑序——扫描它就是按拓扑序执行。Flat 引擎把
推送范式替换为：

- `actBits[]`：每 action 一个 bit 的活跃位图（`vector<uint64_t>`），SCC 组整体占一个 bit；
- 每个实体的扇出表从"监视者 action 列表"改为 elaboration 期预计算的
  **`(word, bit)` 对扁平数组**；写入点（`markDirty`/`markDirtyBool`）遍历该数组做
  `actBits[w] |= b`；
- 相位二同理：每条 priority chain 一个 bit，intent 假→真时置位（替代 CommitQueue）。

位 OR 天然幂等——**去重免费**（现状靠 intent 标志去重，但查表和队列操作照付）。

### 3.2 eval 循环

```
round 循环:
  phase1: 按 execOrder 静态序 tzcnt 扫描 actBits；置位者直接 run()
          （下游被上游写脏后置位，天然在同一趟扫描的后面被执行）
          SCC 组 bit → 既有 runSccGroup 迭代
  phase2: 扫描 chainBits；提交逻辑（优先级、mutex 断言、trace 钩子）不变
  无提交 → 返回
```

正确性依据：位图扫描顺序 = execOrder = 拓扑序，与推送队列的层级桶出队序等价；
边沿方向过滤原样保留（方向监视者有独立的 `(word,bit)` 数组，反向跳变只拨
`EventSlot::prev` 不置位）；`Action::run()` 内的判等短路（值没变不脏下游）保留，
它就是 verilator 的 `__Vchglast__` 对应物。

**构建期断言**：非 SCC 的依赖不得构成"回边"（下游 bit 出现在已扫过的位置）——
拓扑序的正确性在 elaboration 一次性校验，运行时零成本。

### 3.3 触点

- `elab/graph.h`：旁路构建位图与扇出表，不动旧表；
- `core/entity.h`：写入点（`markDirty`/`markDirtyBool`/方向监视）按 mode 分流；
- `core/action.h`：intent 假→真时置 chain bit（替代/并行于 CommitQueue 推送）；
- `sim/engine.h`：`Module::eval()` 新增 Flat 分支。

### 3.4 实测收益（2026-09-28 落地，详见《拆解》§11）

预估 1.5–1.8×，**实测 1.07–1.11×**（孤立回放 61.5s → 55.4–57.7s，对 RTL
差距 14.5× → 13.4×，累计相对全量求值 ~8.9×）。差距的教训：旧 profile 里
eval/markDirtyBool 的 self 时间混着真活（判等比较、CSR 遍历、dirty 时钟
滴答、run() 本体归属），纯队列簿记只占 ~10%；位图化把簿记压到近零后，
主导项变为**逐 action 虚调用**与**逐信号判等**本身——这两项在"调度范式"
层面已无可压缩，进一步收益需要 action 融合/逻辑内联方向（已搁置，见 §7）。

## 4. 触点之外的配合改动

无。模型代码、建模 API、trace/audit、Poll/Full 路径均不动。

## 5. 统一验证基线

1. wolvicmod ctest（test_readyq.cpp 的等价比对扩为三方：Flat ≡ Push ≡ Full，
   逐半拍全实体比对；覆盖 posedge/negedge、In/Wire/Reg 事件源、事件兼任
   plain read、双方向事件、守卫 Update、优先级链、时钟电平期间搅动输入）；
2. wolvicmod cosim 全部组 + proj ctest + proj cosim 全部组；
3. coremark 全程回放 `REPLAY_AUDIT=1`（0 失配）；
4. `--dut=both` 共栖交叉（RTL/wolvic 各自 0 失配）；
5. 干净单机孤立回放 A/B（zj_ab_replay，两次取优），perf profile 留存
   （《拆解》§5 复现约定）；
6. 《拆解》新增一节记录机制、数字与热点迁移。

## 6. 风险登记

| 风险 | 缓解 |
|---|---|
| 位图扫描在极端稀疏工况（全图仅个位数 action 活跃）弱于推送的精确唤醒 | SchedMode 可退回 Push；实测 ZhuJiang 回放每半拍活跃 action 数以百计，不在该工况 |
| 写点扇出从队列操作变数组 OR，窄信号高频写场景可能回退 | 干净单机 A/B 实测为准，回退则默认仍用 Push |
| 每实体每轮多写破坏位图单次扫描 | SCC 组内信号不参与位图直通（沿用边沿过滤的既有排除）；构建期回边断言 |

## 7. 非目标

- 不改变建模 API 语义（§2 约束 1/2）；
- **不做 codegen / 类型系统 / DSL**：曾论证"expr 快速路径演进为可发射 IR +
  框架基础类型库（Bool/UInt/Vec/Bundle）"的路线，结论是 lambda 本就是不可分析
  的黑盒、DSL 侧与 lambda 侧的语义双解问题没有一个不丑的解法，收益路径不清晰，
  整体搁置；lambda 写法是唯一建模方式，引擎只在其外围优化调度；
- 不做字段粒度/写侧拆分等任何依赖类型内省的方向（同上，搁置）；
- 不为 trace/audit 模式优化（继续强制 Full，正确性优先）。
