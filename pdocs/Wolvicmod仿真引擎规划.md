# Wolvicmod 仿真引擎实现

框架文档[《Wolvicmod框架规划》](Wolvicmod框架规划.md)定义了模型的语义：实体、动作、round、`eval()` 的收敛规则。本文档说明引擎怎样实现这些语义——建图期把模块树转换成一组平面数组，仿真期的 `eval()` 在这组数组上执行。阅读本文前请先读框架文档，下表列出两边概念的承接关系。

| 概念 | 本文中的用法 | 框架文档 |
|---|---|---|
| Module、端口、Wire、Reg、Mem | 模型树中的实体类型；展平后用平面下标访问 | [§2 核心抽象](Wolvicmod框架规划.md#2-核心抽象) |
| Assign、Update、读集 | 运行时动作；读集决定反向依赖表的内容 | [§2.3–2.4](Wolvicmod框架规划.md#23-wire-与-assign) |
| 建树期、建图期、仿真期 | 生命周期三段；本文描述建图期的产物和仿真期的执行 | [§4](Wolvicmod框架规划.md#4-建图从模块树到仿真图) |
| round、两阶段执行、边沿检测 | `eval()` 的外层规则；本文给出实现它的数据结构 | [§5 仿真时间模型](Wolvicmod框架规划.md#5-仿真时间模型) |
| 脏驱动 | 实体值变化时把依赖它的动作标进位图，`eval()` 只执行被标记的动作 | 无——本文 [§1](#1-设计目标与全局图景) 定义的引擎实现策略 |

## 1. 设计目标与全局图景

实际负载中，每次 `eval()` 里大部分逻辑是静止的：空闲的队列、未命中的状态机、保持不变的 Reg。让静止逻辑参与求值是纯粹的浪费，引擎的设计目标就是把 `eval()` 的工作量收缩到"值真正发生变化所波及的部分"。

做法用一句话说：**实体的值发生变化时，把依赖它的动作标记进一张位图；`eval()` 只执行被标记的动作。** 这种做法称为脏驱动求值。它到达的不动点与框架文档定义的全量求值一致（§8 给出前提与论证），是一套纯粹的实现策略，框架文档不定义它。

建图期把模块树转换成五组运行时结构：

```text
模块树（建树期产物，带层次结构）
    │  elaborate()
    ▼
实体表 entities            每个实体占一个下标，值存在实体对象里（§2、§3）
动作表 actions             每条动作占一个下标，行为存在动作对象里（§2、§4）
执行顺序 execOrder         动作与组合环组的拓扑序（§5.1）
反向依赖表 depPool         实体 → 依赖它的执行项位置（§3.2、§5.3）
位图 actBits / chainBits   待执行的动作、待提交的更新链（§5.2、§5.3）
    │
    ▼
eval()：从位图取出待执行的动作，按 execOrder 顺序执行；
       动作执行中值发生变化，就查反向依赖表把下游动作标进位图；
       一轮下来没有任何状态变化，eval() 返回。（§7）
```

层次路径（如 `top.sr.q`）留在实体上，服务报错、trace 与波形；调度只使用平面下标。后文 §2–§5 自上而下讲这五组结构的内容，§6 讲建图期的构建流程，§7 讲 `eval()` 的执行流程，§8 汇总正确性前提与性能结论，§9 是源码文件对照。

## 2. 建树期：对象创建与所有权

建模代码在模块构造函数里运行，产出的模块树同时做两件事：组织层次，持有对象。`Module` 直接拥有三类对象：

- `entities_` 是 `std::vector<std::unique_ptr<Entity>>`。每个端口、Wire、Reg、Mem 都是一个多态实体对象，创建时绑定名称和所属模块；
- `children_` 是 `std::vector<std::unique_ptr<Module>>`，保存子模块树；
- `actions_` 是 `std::vector<Action*>`，只保存登记顺序。动作对象由模块内的 `ActionArena` 逐个 `new` 并登记析构函数，模块销毁时统一回收。逐个分配让每个 `Action*` 从创建到仿真结束保持固定地址，建图期可以直接把这些指针写进运行时结构。

建图期先把树展平成两张指针表：

```cpp
struct FlatModel {
    std::vector<Entity*> entities;  // 深度优先顺序
    std::vector<Action*> actions;   // 模块内登记顺序
};
```

展平完成三件事：

1. 按 `entities` 的位置写入 `Entity::flatIndex_`；
2. 按 `actions` 的位置写入 `Action::regIndex_`；
3. 结构检查：每个信号恰有一条驱动 Assign、根输入以外的信号都有驱动者、实体都已命名。

此后实体和动作的身份就是这两个下标。FlatModel 只起索引作用：实体的值仍在 `Signal<T>`、`Reg<T>`、`Mem<T, R>` 对象内部（§3.1），动作的行为仍在 `AssignAction<T, F>`、`UpdateAction<T, Target, F>` 对象内部（§4）。`elaborate()` 返回后实体与动作的集合固定，所有运行时结构按最终规模一次性分配（§5.3）。

## 3. 实体：值存储与唤醒绑定

### 3.1 元数据与类型化的值

`Entity` 是所有实体的公共元数据：

```cpp
class Entity {
    EntityKind kind_;       // In / Out / Wire / Reg / Mem
    Module* owner_;         // 层次路径和权限检查
    std::string name_;
    uint32_t flatIndex_;
};
```

类型化的值放在派生类里：`Signal` 持有当前值，`Reg` 持有 `cur` 和 `next` 两个槽位，`Mem` 持有按行存储的数组。lambda 直接读写原生类型 `T`，值存储因此保持具体类型，引擎内部没有 `std::variant` 之类的擦除容器。

### 3.2 写入点的唤醒绑定

"实体值变化时要唤醒哪些动作"，建图期把这个答案以几段指针的形式绑到实体上：

```cpp
const uint32_t* deps_;       // 依赖它的执行项位置，反向依赖池中的一段
uint32_t depN_;
uint64_t* actBits_;          // 待执行位图（SimState 持有）
uint64_t* edgeBits_;         // 单事件边沿事实位（§4.1）

const EdgeWatchDep* posWatch_;  // 只关心上升沿的 Update
uint32_t posWatchN_;
const EdgeWatchDep* negWatch_;  // 只关心下降沿的 Update
uint32_t negWatchN_;
```

`deps_` 列出读这个实体的全部动作在 execOrder 中的位置。值写入点的全部唤醒工作就是遍历这段数组、做位图置位：

```cpp
for (uint32_t i = 0; i < depN_; ++i)
    actBits_[deps_[i] >> 6] |= 1ull << (deps_[i] & 63);
```

位图置位是幂等的：重复标记同一个动作没有额外效果，写入点因此零簿记。所有实体的 `deps_` 都指向同一个连续数组（`SimState::depPool`，§5.3）的不同分段，写入点访问的永远是一段连续整数。

Bool 写入走 `markDirtyBool(newValue)`，写入点知道跳变方向，于是按方向分流：上升沿唤醒 `posWatch_` 里的 Update，下降沿唤醒 `negWatch_` 里的 Update。反方向的 Update 只需把它记录的信号旧值拨到新值，等真正的边沿到来再唤醒；使用 `edgeBits` 的单事件 watcher 连这一步都省掉（§4.1）。绑定了守卫的 watcher（`guardVal` 非空）在置位前先读守卫值：守卫为假意味着这条 Update 本拍必然不激活，写入点只推进它的边沿历史，跳过置位。

```cpp
struct EdgeWatchDep {
    uint32_t execPos;       // execOrder 中的执行项位置
    bool* prev;             // Update 记录的事件信号旧值
    const bool* guardVal;   // 该 Update 的电平守卫的值地址；无守卫则为空
    bool eb;                // 用 edgeBits 的一位表示边沿事实（§4.1）
};
```

跳变方向未知的写入——值类型不可比较、Mem 行写入——走 `deps_` 普通依赖，两个方向的 watcher 照常唤醒，由动作自己维护边沿历史。

### 3.3 恒等连接的别名

快速路径登记的同类型恒等 Assign（如 `dout = cnt`），语义上就是让目标的值恒等于源的值。建图期直接把目标实体的值存储指向源实体（`aliasStorageTo()`），这条 Assign 随之从执行顺序中消失：

- 按 `flatIndex` 做 union-find，为每组别名选出代表元；
- 目标实体的存储落到代表元上；
- 读取目标的依赖行改写为代表元的下标。

带类型转换的连接保留真实计算，别名环（`a=b; b=a`）保留一条真实拷贝动作。

## 4. 动作：统一索引，具体行为

调度器眼中的动作是统一接口：

```cpp
class Action {
    Kind kind_;                    // Assign / Update
    Entity* target_;
    Module* ctx_;
    uint32_t regIndex_;
    std::vector<Entity*> reads_;

    bool intentActive_;            // Update 的意图当前是否有效
    uint32_t commitChain_;
    uint64_t* commitBits_;
};
```

`reads_` 包含所有影响动作结果的实体：显式 `.reads(...)`、Update 的事件信号、使能信号、Mem 的写地址。建图期按 `reads_` 建依赖图；如果 lambda 偷读了未声明的实体，这个动作就会漏掉唤醒，读到过期值——`auditOn()`（§7）用来在调试构建下抓出这种错误。

行为保留在模板派生类里：

- `AssignAction<T, F>` 保存 `Signal<T>* target_` 和具体的 `F compute_`。`run()` 求值后与当前值比较，值变化才写入目标、唤醒下游；
- `UpdateAction<T, Target, F>` 保存事件槽、守卫、地址函数、具体的 `F compute_` 和 `T intent_`。`run()` 检测边沿、检查守卫、写入意图；目标状态的修改推迟到提交阶段（§5.2），对应框架文档的两阶段语义。

类型擦除只有一处：`execOrder` 里存 `Action*`，执行时调一次虚函数 `run()`。lambda 本身保持具体类型，调用路径上没有 `std::function`。意图为平凡可复制类型的 Reg 会额外发布 `fastIntentSrc()` 和字节数，供提交阶段批量复制（§5.2）。

### 4.1 边沿 watcher

一条 Update 如果把某个 Bool 实体只用作单一方向的边沿事件，建图期就把这条依赖从普通 `deps_` 移进实体的方向 watcher 段（§3.2），写入点按跳变方向决定是否唤醒它。

只挂一个事件的 watcher 还能再省一步：写入点在匹配方向跳变时，同时置位 `actBits` 和 `edgeBits` 中属于这个执行项的位——`edgeBits` 的那一位就是"边沿已经发生"这个事实本身，`run()` 测试并清除自己的位即可，连信号值和旧值都不用读。挂多个事件的 Update 保留旧值比较：一个位区分不出边沿来自哪个事件信号。

## 5. 执行顺序、更新链与位图

`Module::elaborate()` 创建 `std::unique_ptr<SimState>`，由根模块持有。`SimState` 汇总仿真期的全部运行时数据：执行顺序、更新链、位图和依赖池。实体与动作的所有权仍在模块树（§2），`SimState` 只存下标和指针。

### 5.1 执行顺序与组合环

```cpp
struct ExecItem {
    Action* single;       // 普通动作
    size_t group;         // single 为空时指向 groups[group]
};

struct SccGroup {
    std::vector<Action*> actions;
    std::vector<Entity*> signals;
};

std::vector<ExecItem> execOrder;
std::vector<std::unique_ptr<SccGroup>> groups;
```

`execOrder` 是组合求值阶段的执行顺序。无环动作各占一项；组合环（框架文档 §4.3 的强连通分量）收缩成一项 `SccGroup`：组被唤醒后，组内动作反复执行并比较成员信号快照，直到稳定或超过迭代上限。组内的环边由这个稳态迭代负责传播，反向依赖表只登记组的对外扇出。

### 5.2 更新链与提交

以同一状态为目标的所有 Update 按登记顺序组成一条 `Chain`：

```cpp
struct Chain {
    std::vector<Action*> updates;

    Entity* reg;          // Reg 快速提交用；Mem 或通用路径为空
    char* cur;
    char* next;
    uint32_t size;
    bool isBool;
    bool bitwiseEq;
    Action::TypedCommitFn typedCommit;

    uint64_t memberBits;  // 成员不超过 64 个时，每位记录一个成员的意图是否有效
    std::vector<const char*> intentSrcs;
    bool memberCommit;
};
```

每条链在位图 `chainBits` 里占一位：成员的意图从无效变为有效时置链位，提交阶段只处理被置位的链。链内按登记的反序应用意图——先登记的后写，覆盖其他成员的结果，这就是框架文档"注册顺序即优先级"的实现方式。

目标是 Reg、且全体成员的意图都是相同大小的平凡可复制类型时，提交可以批量完成：把有效意图按序复制进共享的 `next` 槽，再做一次 `cur`/`next` 比较。值类型没有填充字节时用逐字节比较，否则用链级 `typedCommit`（逐类型的 `operator==`）。Mem 链和非平凡类型走逐成员的通用提交路径。

### 5.3 依赖池与位图

```cpp
std::vector<uint32_t> depPool;  // 所有实体的反向依赖位置，按实体分段
std::vector<uint32_t> depOff;   // E + 1 个分段起止偏移

std::vector<uint64_t> actBits;  // execOrder 中待执行项的位图
std::vector<uint64_t> edgeBits; // 单事件边沿事实位图
std::vector<uint64_t> chainBits;// 待提交链的位图
```

实体 `i` 的反向依赖是 `depPool[depOff[i] .. depOff[i+1])`；方向 watcher 集中在 `posWatchPool` / `negWatchPool` 里，同样按实体分段。所有池在建图期一次性分配到位，此后实体里的分段指针终身有效。

## 6. 建图期的构建流程

### 6.1 图节点与边

`buildExecOrder()` 使用一段合并的节点空间：实体占 `[0, E)`，动作占 `[E, E+A)`。图里有两种边：

```text
Assign ───────> 它驱动的实体
实体/状态 ────> 读取它的动作
```

Update 的事件、守卫和地址已经在 `reads_` 里，随普通读集一起进图。被别名化移除的 Assign（§3.3）不出现在图里。

### 6.2 强连通分量与拓扑序

先用 Tarjan 算法求强连通分量，得到分量 DAG，再用 Kahn 算法输出拓扑序。输出时在就绪分量中优先选择与前一个动作具体类型相同的分量：同类动作相邻，`run()` 的虚调用目标高度重复，同类型对象又落在 malloc 的同一 size class 里彼此相邻，数据局部性随之改善。任何拓扑序的线性扩展都保持依赖顺序，这个选择只影响性能。

输出的位置即身份：

- 只含一个动作的分量变成 `ExecItem{single = action}`；
- 非平凡分量收集动作和成员信号，按 `regIndex` 排序后存成 `SccGroup`；
- 每项在 `execOrder` 中的位置同时就是它在 `actBits` / `edgeBits` 里的位号。

最后为每个实体找到唯一驱动它的 Assign，确认其位置早于所有读取者；发现反例时建图期直接报错。凭这个保证，扫描中被唤醒的下游动作一定位于还没扫到的位置（§7）。

### 6.3 反向依赖表

执行顺序确定后，遍历每个执行项的读集构建反向依赖：

1. 把读实体映射到别名代表元（§3.3）；
2. 两遍遍历生成连续数组：第一遍统计每个实体的扇出数，前缀和得到 `depOff`，第二遍填充 `depPool`；
3. 满足方向过滤条件的依赖改填 `posWatchPool` / `negWatchPool`；
4. 把每个实体的分段地址和位图基址绑定回 `Entity`。

方向过滤的条件是：读实体由组合环之外的动作驱动（环内一次稳态迭代可能多次写值，方向不可靠），且该动作只以单一边沿方向使用它——不同时作为普通读、守卫、地址或另一方向的事件出现。

### 6.4 更新链的绑定

第二遍遍历动作表，按 `target()` 把 Update 分组为链，然后逐链：

1. 在 `chainBits` 中分配位号；
2. 给每个成员绑定链位置和位图基址；
3. 目标是 Reg 且全体成员意图同尺寸、平凡可复制时，填写 `cur`、`next`、尺寸和比较策略；
4. 成员不超过 64 个时，为每个成员绑定 `memberBits` 中的一位和 `intentSrcs[i]`；
5. 其余情况保留通用提交路径。

全部构建完成后，`elaborate()` 把 `actBits` 的所有位置位：第一个 `eval()` 从全量执行开始，与全量求值的首轮一致。此后每次写入只置受影响的位。

## 7. eval() 的执行流程

仿真期的 `eval()` 只在这五组结构上工作。每轮分两段：组合求值扫描 `actBits` 执行动作，状态更新扫描 `chainBits` 提交意图：

```text
for each round:
    while actBits 中还有 1：
        pos = ctzll(actBits[word])
        清除该位
        item = execOrder[pos]
        item.single != null ? item.single->run()
                            : runSccGroup(item.group)

    while chainBits 中还有 1：
        chain = chains[ctzll(chainBits[word])]
        清除该位
        提交这条链的有效意图

    本轮没有任何状态变化 → 返回
    否则进入下一 round
```

`actBits` 的扫描顺序就是 `execOrder` 顺序。上游动作执行中写脏了下游，而下游的位置还没扫到，于是同一轮内自然被执行——这是 §6.2 "producer 早于 consumer" 保证的直接推论。

提交阶段，快速 Reg 链按 `memberBits` 从后向前复制意图，然后一次状态提交加变化检测；通用链按相同的优先级顺序逐成员调 `applyIntent()`，最后调目标的 `finalizeTarget()`。提交完成后链上全部意图位清零——`eval()` 返回时不存在滞留的有效意图，重复调用 `eval()` 保持幂等。状态值真正变化时，目标实体调 `markDirty()` 或 `markDirtyBool()`，产生下一 round 的种子位。

两个调试模式复用同一套结构：

- trace 模式需要每个动作的边沿历史，直接完整扫描 `execOrder`，单事件 watcher 回退到旧值比较；
- audit 模式在每次动作调用前后记录实际读到的对象，与声明的读集比对。

## 8. 正确性前提与性能结论

前述结构各自消除一类开销，汇总如下：

| 原本的开销 | 当前结构 |
|---|---|
| 每次写信号创建、移动和去重就绪节点 | 实体扇出连续数组 + `actBits` 幂等置位 |
| 每轮扫描没有变化的动作 | 执行项活跃位图 |
| 提交阶段扫描所有 Update | `chainBits` 只处理产生意图的链 |
| 每个 Update 单独虚调用提交 Reg | 链级 `cur`/`next` 字节复制加一次比较 |
| 时钟反向跳变唤醒所有正沿 Update | 方向 watcher；单事件再加 `edgeBits` |
| 恒等 Assign 的存储复制和一次执行 | 建图期存储别名和依赖重挂 |
| 每个实体单独保存扇出容器 | 全部整数依赖集中在连续池中 |

这些结构改变索引、唤醒和提交的实现方式；动作读到的值、Update 的优先级、round 的收敛规则与框架文档的全量求值语义一致。一致性的前提有四条，由建图期检查或驱动惯例保证：

1. 读集完整。漏写读集的动作会漏掉唤醒，`auditOn()` 用于对账（§7）；
2. 组合环之外的依赖满足 producer 早于 consumer，建图期检查（§6.2）；
3. 方向过滤和守卫预过滤要求每个实体每 round 至多跳变一次（单驱动与统一提交的自然推论），且守卫在边沿时刻保持稳态值——驱动惯例是先沉定组合输入、再驱动时钟；
4. 所有池在建图期分配到位，之后指针终身有效（§5.3）。

前提满足时，脏驱动与全量求值到达同一不动点，运行时不需要保留对应的检查。历史上引擎曾实现轮询与推送两条备选路径，位图方案在所有实测负载上不劣于二者，现为唯一路径；全量扫描只在 trace 模式内部使用。香山 ZhuJiang L3 模型的实测性能结论见 `proj-xiangshan-l3/docs/verification-report.md` §3。

## 9. 源码对应关系

| 实现内容 | 文件 |
|---|---|
| 实体元数据、变化通知、方向 watcher | `wolvicmod/include/wolvicmod/core/entity.h` |
| Action 基类、Assign/Update 派生类、意图与事件槽 | `wolvicmod/include/wolvicmod/core/action.h` |
| Module 所有权、ActionArena、外部输入 | `wolvicmod/include/wolvicmod/core/module.h` |
| FlatModel、别名解析、结构检查 | `wolvicmod/include/wolvicmod/elab/flatten.h` |
| SimState、SCC、拓扑序、依赖数组、Update 链 | `wolvicmod/include/wolvicmod/elab/graph.h` |
| 一次 eval 的位图扫描、SCC 迭代和提交 | `wolvicmod/include/wolvicmod/sim/engine.h` |
| elaborate 生命周期入口 | `wolvicmod/include/wolvicmod/elab/elaborate.h` |
