# Wolvicmod：显式结构 CModel 框架规划

---

## 1. 设计动机：建模粒度的选择

本文档规划一个电子系统级（ESL）的行为级建模与仿真框架，目标对象是 **高性能 CPU、NPU 这类内部紧耦合的微结构**：模块间存在密集的零延迟互馈（流水线级间握手、旁路前递、互斥仲裁）。技术路线的核心是**建模粒度**的选择——结构精确到什么级别，行为简洁到什么级别。现有方案分布在这条轴的两端。

#### RTL：结构与行为都过细

RTL 的结构粒度精确到每一根线、每一个比特，行为也必须位级展开。以"对 8 个元素排序"为例：RTL 中需要实例化排序网络（Batcher 8 输入网络含 19 个比较交换单元）或编写多周期状态机，而行为级模型中是一次 `std::sort` 调用。仿真层面同样过细：一条 32 位总线的单次传输对应数十个位级翻转事件的调度开销，行为级模型中同一传输是一次函数调用内的对象传递。对微结构验证而言，位级精度超出了需要——把行为粒度粗化到宿主语言级别，换来建模速度与仿真速度两个优势基础，这是 CModel 路线成立的依据。

#### SystemC：结构信息缺失

SystemC 是 CModel 的事实标准，行为粒度确实自由了，但结构粒度被一起丢掉：模块行为由进程（`SC_THREAD` / `SC_METHOD`）承载，敏感表仅声明进程的唤醒条件，不声明进程的读写对象；进程间执行次序由事件内核在运行时动态决定。而零延迟互馈在紧耦合微结构中密集出现——流水线级间握手、旁路前递、仲裁逻辑互相等待对方的组合输出——其正确性依赖建模者对 delta cycle 次序的人工安排。结构信息恰恰是这类设计中最不能丢的东西：丢掉它，框架就无法静态检查，错误暴露为运行时行为偏差而非可定位的报错。

#### Wolvicmod：中间粒度

结构粒度取依赖关系机器可读所需的最小粒度：模块显式声明状态、信号、每条计算的读集与目标，不多（不到位级）、不少（不丢依赖）；行为粒度停留在宿主语言表达力最强的级别：每条计算是一个黑盒 lambda，体内任意 C++。本框架称这种形态为结构化 CModel：

> 结构显式声明，行为黑盒保留。

本文档余下部分按如下组织：§2 定义核心抽象（实体与计算动作），§3 给出建模 API，§4 与 §5 分别规定 elaboration 与仿真语义，§6 讨论调试与观察。

---

## 2. 核心抽象

Wolvicmod 的概念体系包含五种结构实体（Module、端口、Wire、Reg、Mem）与两种计算动作（Assign、Update）。其中 Wire 与端口合称**信号**——无状态的组合连线；Reg 与 Mem 合称**状态**——时序存储。信号与状态的类型参数 `T` 可以是任意 C++ 类型（`std::vector`、STL 容器、用户自定义类均可），计算直接在原生类型上求值。

### 2.1 概念总览

| 概念 | 类别 | 职责 |
|---|---|---|
| `Module` | 结构实体 | 树状层次节点，持有信号、状态与子模块 |
| `In<T>` / `Out<T>` | 结构实体 | 模块边界的端口，语义上是 Wire 的边界形态 |
| `Wire<T>` | 结构实体 | 组合信号，无状态 |
| `Reg<T>` | 结构实体 | 寄存器，单值状态载体 |
| `Mem<T, R>` | 结构实体 | 存储器，R 行 T 类型的状态阵列 |
| Assign | 计算动作 | 组合计算：连续生效，驱动 Wire 与端口 |
| Update | 计算动作 | 时序计算：边沿触发，驱动 Reg 与 Mem |

### 2.2 Module 与端口

#### Module

Module 是层次的组织单元：持有 Wire、Reg、Mem、端口与子模块，全体模块构成一棵有根树。连线限定为三种模式：兄弟子模块间、父到子（下行）、子到父（上行）。

#### 端口

`In<T>` / `Out<T>` 是模块边界的端口，语义上为组合连线，与 Wire 相同。满足三条规则：

- 子模块的 `In` 由父模块的 Assign 驱动，子模块内只读；
- 子模块的 `Out` 供父模块读取；
- 父模块只能读写直接子模块的端口，不能越级访问。

### 2.3 Wire 与 Assign

#### Wire

组合逻辑信号，满足静态单赋值（SSA）特性：每根 Wire 恰被一条 Assign 驱动。

#### Assign

Assign 是 Wire 的唯一驱动方式，由显式读集和计算 lambda 定义：

- 显式读集声明它读取的 Wire / Reg 集合；
- 计算 lambda 是用任意 C++ 代码表达的读集纯函数。

语义上可类比于 Verilog 的连续赋值 `assign`。

### 2.4 Reg、Mem 与 Update

#### Reg

单值时序状态载体，用于建模寄存器等标量状态部件。

#### Mem

`Mem<T, R>` 是按行寻址的状态阵列：R 行、每行类型为 T，用于建模存储器。读口给出已提交（committed）的内容；写以行为单位，只在提交点生效，未写的行保持。与 Reg 的整体次态替换不同，Mem 的每次写入只触及一行，提交成本与存储器容量无关。

#### Update

Update 是更新 Reg 与 Mem 状态的唯一方式。每条 Update 的构成为 { 显式读集，计算 lambda，事件列表，优先级 }：

- 显式读集和计算 lambda 的语义同 Assign；
- 事件列表：关联一组边沿事件（例如 `posedge(clk)` / `negedge(rst_n)` 等），任一边沿触发时，Update 生效；
- 优先级：多个 Update 可同时作用于同一状态，注册顺序即优先级。

Update 的语义按目标分为两种形态：

- 目标是 Reg：激活时以 lambda 的返回值作为次态，整体替换；
- 目标是 Mem：注册时另经 `.addr(...)` 指定写入行，激活时把 lambda 的返回值写入该行，其余行保持。多条 Update 作用于同一 Mem 时按优先级应用：写不同行互不干扰，写同一行由高优先级覆盖。

---

## 3. 建模 API

建模在模块构造函数中完成，由两步构成：声明结构成员（端口、Wire、Reg、Mem、子模块），再为每根 Wire 与每个端口注册 Assign、为每个 Reg 与 Mem 注册 Update。结构成员经 Module 的创建方法声明——`createIn` / `createOut` / `createWire` / `createReg` / `createMem` / `createChildModule`，创建时给出名字，供诊断与波形使用（§6.1）；`IN` / `OUT` / `WIRE` / `REG` / `MEM` / `SUB` 六个宏是对应的一行简写。本节通过五个递进的案例给出全部写法：§3.1 组合逻辑，§3.2 时序逻辑，§3.3 层次与参数化，§3.4 存储器，§3.5 elaboration 与外部驱动。

### 3.1 组合逻辑：32 位加法器

以最简单的组合逻辑——32 位加法器——给出 Assign 的完整写法：

```cpp
struct Adder : Module {
    IN(uint32_t, a);
    IN(uint32_t, b);
    OUT(uint32_t, sum);

    Adder() {
        sum.assign().reads(a, b) = [](auto src) {
            auto [a, b] = src;
            return a + b;
        };
    }
};
```

#### 一般形式

Assign 的注册链为：

```cpp
target.assign().reads(s1, s2, ...) = [](auto src) { /* 任意 C++ */ };
```

其中 `target` 为 Wire、本模块的 `Out` 或子模块的 `In`；`s1, s2, ...` 为读集信号。

注册链逐段对应 §2 的概念：

- `sum.assign()`：为 `sum` 注册一条 Assign。Wire 与端口都以 `.assign()` 起始；
- `.reads(a, b)`：显式读集，实参顺序决定 lambda 收到值的顺序；
- `=` 右侧是计算 lambda：接收一个 `std::tuple<const Ts&...>` 类型的参数（`Ts` 由 `reads(...)` 的实参类型定型），体内用结构化绑定为各读集元素命名，参数名约定为 `src`。

该写法有三重编译期校验：

1. 结构化绑定的名字数与读集大小不一致，编译失败；
2. lambda 不能以该 tuple 类型调用（`is_invocable` 校验），编译失败；
3. 读集以 `const&` 传入，lambda 内写读集信号，编译失败。

lambda 体内的局部变量是临时值，只在本次求值内存在；需要被多条动作共享、或需要在调试中观测的中间值，应显式声明为 Wire 并注册 Assign。

#### 表达式快速路径

单行表达式可以省略 lambda，直接由 `operator=` 注册——运算符表达式、恒等连接、常量赋值都是单行表达式。快速路径由 `Wire` / `Reg` 的运算符重载实现：运算符不求值，注册期把表达式展开为等价的 Assign，读集自动取表达式中出现的信号。

上面的加法器用快速路径重写，完整模块如下：

```cpp
struct Adder : Module {
    IN(uint32_t, a);
    IN(uint32_t, b);
    OUT(uint32_t, sum);

    Adder() {
        sum = a + b;
    }
};
```

恒等连接与常量赋值写法同理：

```cpp
dout = cnt;              // 恒等连接
req  = 0;                // 常量：零读集
```

快速路径的一般形式为 `target = 单行表达式;`。

### 3.2 时序逻辑：带复位与使能的计数器

```cpp
struct Counter : Module {
    IN(Bool, clk);
    IN(Bool, rst_n);
    IN(Bool, en);
    OUT(uint32_t, dout);
    REG(uint32_t, cnt);

    Counter() {
        // 复位：先注册，优先级高
        cnt.update().on(negedge(rst_n)) = 0;

        // 计数：clk 上升沿且 en 有效时自增
        cnt.update().on(posedge(clk)).en(en).reads(cnt) = [](auto src) {
            auto [c] = src;
            return c + 1;
        };

        dout = cnt;
    }
};
```

案例为 `cnt` 注册了两条 Update：第一条是复位，采用 §3.1 的常量快速路径；第二条是计数，`.on(posedge(clk))` 指定触发边沿，`.en(en)` 是电平守卫——使能无效时该 Update 不激活，`cnt` 保持原值。同一 Reg 的多条 Update 同时被激活时，按 §2.4 的规定取注册顺序：复位先注册，优先级高于计数。末尾的 `dout = cnt` 是恒等连接快速路径。

#### 一般形式

Update 的注册链为：

```cpp
reg.update().on(e1, e2, ...).en(g).reads(s1, s2, ...) = [](auto src) { /* 任意 C++ */ };
```

`reg` 为本模块的 Reg。各段按 `on → en → reads → lambda` 的固定词序书写，`reads` 紧贴 lambda，因为读集决定 lambda 的参数类型。快速路径的一般形式为 `reg.update().on(...)[.en(g)] = 单行表达式;`。

与 Assign 相比，Update 的注册链增加两段：

- `.on(...)`：事件列表，必填。对应 §2.4 的边沿事件集合，任一边沿触发时该 Update 更新 Reg。边沿事件的参数可以是任意 Bool 信号——输入端口、Reg 的输出（分频器）、Wire（门控时钟），时钟没有特殊地位；
- `.en(g)`：电平守卫，可选。守卫不满足时该 Update 不激活，Reg 保持原值。守卫对整条 Update 生效；它不是边沿事件，不能写进 `.on(...)`。

#### 多边沿事件的写法

一条 Update 可以挂多个边沿，由 lambda 读取电平自行判别命中的是哪个边沿。同一计数器用这种写法实现，完整模块如下：

```cpp
struct Counter : Module {
    IN(Bool, clk);
    IN(Bool, rst_n);
    IN(Bool, en);
    OUT(uint32_t, dout);
    REG(uint32_t, cnt);

    Counter() {
        cnt.update().on(posedge(clk), negedge(rst_n)).reads(cnt, rst_n, en)
            = [](auto src) {
                auto [c, rn, en] = src;
                if (rn == 0) return 0;        // 复位分支，不受 en 影响
                return en ? c + 1 : c;        // 使能只门控计数分支
            };

        dout = cnt;
    }
};
```

该写法与两条 Update 的版本语义等价，关键是使能写进了 lambda：只门控计数分支，复位分支不受影响。注意此处不能把 `.en(en)` 挂在 Update 上——守卫对整条 Update 生效，会把复位也一并门控；需要分支级的使能控制时，使能只能作为普通读集信号进入 lambda。

尽管两种写法语义等价，实践中建议采用两条 Update 的分开写法：每个边沿的职责一目了然，优先级由注册顺序显式给出，无需在 lambda 内判别边沿来源，更直观。

### 3.3 层次与参数化：移位寄存器与排序流水线

前两节的模块都是平铺的。本节回答两个问题：如何用子模块组织层次，如何参数化模块的结构。案例是一个参数化的 N 拍移位寄存器，以及把它嵌入顶层的排序流水线。

```cpp
// 子模块：参数化 N 拍移位寄存器
template <class T, int N>
struct ShiftReg : Module {
    using Stages = std::array<T, N>;   // 类型含逗号时先取别名，再进宏

    IN(T, din);
    IN(Bool, clk);
    OUT(T, dout);
    REG(Stages, q);                    // 一条 Reg 持有全部 N 级状态

    ShiftReg() {
        q.update().on(posedge(clk)).reads(din, q) = [](auto src) {
            auto [d, q] = src;
            Stages next;
            next[0] = d;
            for (int i = 1; i < N; ++i) next[i] = q[i - 1];
            return next;
        };
        dout.assign().reads(q) = [](auto src) {
            auto [q] = src;
            return q[N - 1];          // 元素访问走完整形式
        };
    }
};

// 顶层：排序 + 3 拍流水延迟
struct SortPipeline : Module {
    using Vec = std::vector<int>;
    using SR  = ShiftReg<Vec, 3>;

    IN(Vec, din);
    IN(Bool, clk);
    OUT(Vec, dout);
    REG(Vec, sorted);                 // 第 0 拍：din 排序后寄存
    SUB(SR, sr);                      // 子模块：再延迟 3 拍

    SortPipeline() {
        sorted.update().on(posedge(clk)).reads(din) = [](auto src) {
            auto [v] = src;
            auto w = v;
            std::sort(w.begin(), w.end());
            return w;
        };

        sr.din = sorted;              // 下行：驱动子模块的 In
        sr.clk = clk;                 // 时钟分发同理

        dout = sr.dout;               // 上行：读子模块的 Out
    }
};
```

移位寄存器的全部 N 级状态放在一条 `Reg<std::array<T, N>>` 中：Reg 的类型参数可以是任意 C++ 类型，这里是 STL 容器，移位只是 lambda 内的普通 for 循环。`reads(din, q)` 同时读输入端口与寄存器自身——次态由现态计算，这是时序逻辑的常规形态。`dout` 的取值是 `q[N - 1]`，元素访问超出单行表达式的范围，故用完整形式注册 Assign。

顶层的组建展示了层次的工作方式：子模块经 `SUB` 宏（`createChildModule`）创建，声明方式与信号、状态一致，使用上与普通成员无别——`sr.din`、`sr.dout` 照常访问。三条注册语句覆盖连线三模式中的两条——`sr.din = sorted` 是下行（父模块的 Assign 驱动子模块的 `In`），`dout = sr.dout` 是上行（读子模块的 `Out`）；第三种模式即兄弟子模块间的连线，由这两条组合而成：父模块读一个子模块的 `Out`、驱动另一个子模块的 `In`，无需额外的连线设施。

行为：`din` 在 `clk` 上升沿被采样、排序后写入 `sorted`，此后每拍在移位寄存器中前进一级，3 拍后到达 `dout`——从采样到输出共 4 拍。

两点结论：

- 参数化直接来自宿主语言：模块是普通模板类，修改延迟级数或数据类型只是修改模板实参，无需专门的 generate 语法；
- 层次不引入新的写法：子模块内部与顶层的建模方式完全一致，层次只是组织方式。

### 3.4 存储器：单写端口数据存储器

前三节的状态都是 Reg。当状态规模大到按行寻址时（如数据存储器），整体替换次态的写法不再适用，改用 Mem：

```cpp
struct DataMem : Module {
    IN(Bool, clk);
    IN(Bool, wen);
    IN(uint32_t, waddr);
    IN(uint32_t, wdata);
    IN(uint32_t, raddr);
    OUT(uint32_t, rdata);
    MEM(uint32_t, 1024, mem);    // 1024 行，每行 32 位

    DataMem() {
        // 写口：clk 上升沿且 wen 有效时，把 wdata 写入 waddr 指定的行
        mem.update().on(posedge(clk)).addr(waddr).en(wen).reads(wdata)
            = [](auto src) {
                auto [d] = src;
                return d;
            };

        // 读口：组合读，给出 raddr 行的 committed 内容
        rdata.assign().reads(mem, raddr) = [](auto src) {
            auto [m, a] = src;
            return m[a];
        };
    }
};
```

写口比 §3.2 的 Update 多了一段 `.addr(waddr)`：与 `.en(wen)` 同为声明式守卫读，决定写不写、写哪一行，不进 `.reads(...)`——读集只决定写入的内容。写口激活时只写一行，其余行保持，提交成本与存储器容量无关。这正是 Mem 独立于 Reg 存在的原因：若把 1024 行放在一条 `Reg<std::array<uint32_t, 1024>>` 里，每次写一行也要付出整阵列的次态替换。

读口是普通的 Assign：Mem 的读给出 committed 内容，读集中出现 `mem` 与出现 Reg 同理，同样适用 §3.1 的全部规则。

#### 一般形式

Mem 的 Update 注册链为：

```cpp
mem.update().on(e1, e2, ...).addr(a)[.en(g)].reads(s1, s2, ...) = [](auto src) { /* 返回写入值 */ };
```

各段按 `on → addr → en → reads → lambda` 的固定词序书写，`.addr(...)` 仅 Mem 的 Update 使用，其余各段语义与 §3.2 相同。

### 3.5 Elaboration 与外部驱动

前四节都在模块内部。本节给出根模块的用法：构造、elaborate、驱动。以 §3.2 的计数器为例：

```cpp
int main() {
    Counter top;            // 构造完成：全部结构与连接已注册（§4.1）
    top.elaborate();        // 展平、结构合法性检查、建图（§4.2–4.3）

    // 复位：rst_n 下降沿触发
    top.rst_n.set(1); top.eval();
    top.rst_n.set(0); top.eval();    // 复位生效，cnt ← 0
    top.rst_n.set(1); top.eval();

    // 时钟驱动：每次跳变对应一次 eval()（§5.1）
    top.en.set(1);
    for (int cycle = 0; cycle < 10; ++cycle) {
        top.clk.set(0); top.eval();
        top.clk.set(1); top.eval();  // 上升沿，cnt 自增
        printf("%d: %u\n", cycle, top.dout.get());
    }
}
```

逐点说明：

- 根模块的 `In` 由外部驱动（§4.2 悬空检查的唯一例外）：`set(v)` 写入输入端口，`get()` 读取输出端口的当前值；
- `elaborate()` 一次性完成展平、结构合法性检查与建图——多驱动、悬空、未命名实体等错误在此刻暴露（§6.2），不带病进入仿真；
- `set()` 只改输入，不推进仿真；先连续 `set()` 多个输入、再调一次 `eval()` 是合法用法；
- 输出序列：复位后 `dout` 为 0，此后每个 `clk` 上升沿自增，打印 1, 2, 3, …。

---

## 4. Elaboration：从模块树到仿真图

elaboration 在根模块构造完成后、首次仿真前执行一次：输入是 §3 注册的模块树，输出是仿真图。产出之后结构不再改变（不支持仿真期间增删模块、信号或动作）。

### 4.1 输入：构造期建模

Wolvicmod 的建模在构造期完成：模块构造函数中声明结构成员、注册 Assign 与 Update。注册只是收集声明，不涉及任何求值，因此建模过程可以由用户自由组织——辅助函数、循环、条件分支、参数化工厂方法都可以用来生成注册代码，以支撑更复杂的建模。子模块作为成员变量先于构造函数体完成构造，父模块在构造函数体内为其注册连线，这一次序由 C++ 语言保证。

唯一的要求是：根模块的构造函数返回时，全部结构与连接均已注册完毕——这就是 elaboration 的输入。框架因此不需要 build() / finalize() 之类的生命周期方法。

### 4.2 展平

展平把模块树消去，得到平坦的实体集与动作集，作为建图的直接输入。

- 实体集：全部模块的 Wire、Reg、Mem 与端口。端口语义上是 Wire（§2.2），展平后与其他 Wire 同等对待；
- 动作集：全部模块注册的 Assign 与 Update。动作内的引用原本是「模块路径 + 成员」的层次引用，展平时统一重定向到扁平实体——父模块驱动子模块的 In、读取子模块的 Out，展平后与模块内的普通读写再无区别，连线因此不需要专门表示。

层次信息不丢弃：每个扁平实体保留其层次路径名（§6.1），供诊断与波形输出定位。

结构合法性检查在展平后的扁平集合上进行：

- 每个信号恰有一条驱动 Assign——Wire SSA 在扁平命名空间的直接检查，端口按 §2.2 的规则同样适用（子模块的 In 由父模块驱动、Out 由子模块内部驱动）；
- 无悬空：除根模块的 In（由外部驱动）外，不存在没有驱动者的信号；
- Reg 与 Mem 不检查驱动数——它们由 Update 更新，天然允许多条（§2.4）。

展平是纯粹的收集与重定向，复杂度与实体、动作总数成线性。

### 4.3 仿真图：定义与分析

展平之后，形成两张仿真图，刻画信号（In、Out、Wire）、状态（Reg、Mem）、动作（Assign、Update）之间的依赖与执行顺序。

#### 组合求值图：组合逻辑的求值顺序

- 节点：信号（In、Out、Wire）、状态（Reg、Mem）、动作（Assign、Update）；
- 边：
  - Assign → 它驱动的信号；
  - 信号、状态 → 读它的动作；
- 图结构性质：
  - 源点：状态、根模块的 In（由外部驱动，模型内没有入边）；
  - 汇点：Update、根模块的 Out（被外部读取，模型内没有出边）；
  - 组合求值图允许有环：环按强连通分量（SCC）收缩为超节点后参与拓扑排序，环内的组合逻辑通过迭代求稳态；迭代设趟数上限，不收敛（振荡）则报错并附环路径。

#### 时序优先级图：状态更新的执行顺序

- 节点：Update 动作；
- 边：以同一状态为目标的多条 Update 按注册顺序**反向连边**，后注册者指向先注册者；
- 图结构性质：
  - 若干条互不相交的链，每个有多条 Update 的状态对应一条；
  - 同一状态的多条 Update 同时激活时按链执行：后注册者先写，先注册者后写并覆盖——注册顺序即优先级（§2.4）。

---

## 5. 仿真时间模型

### 5.1 用户视角

Wolvicmod 的用户接口与 Verilator 的 `eval()` 语义对齐：**设置输入、调 `eval()`、读取输出**。

时间由外部输入的时钟信号推进：时钟每跳变一次（上升沿或下降沿），对应一次 `eval()` 调用。

模型内部，任何 Bool 信号都可用作 Update 的边沿事件，其跳变会被识别并触发相应的 Update——时钟在模型内没有特殊地位。

`eval()` 返回时，模型已收敛到当前输入对应的稳态；不改变输入而重复调用 `eval()`，状态不再变化。


### 5.2 时间结构：eval 与 round

一次 `eval()` 由若干 **round** 构成，每个 round 固定为两个阶段：

1. **组合求值**：完整遍历组合求值图，所有动作执行其 lambda——包括 Update。Update 在此阶段只记录一条更新意图——lambda 的返回值与激活标记（边沿 ∧ 使能；边沿的检测见 §5.3）——不触碰任何状态。由此，无论是 lambda 的读集还是使能守卫，凡是读取状态的地方，读到的必是本轮写入前的值：所有读都发生在任何写生效之前；
2. **状态更新**：把激活的 Update 记录的返回值应用到目标状态：以同一状态为目标的多条 Update 按时序优先级图的链应用——后注册者先写、先注册者覆盖。全部写入在同一时刻生效（NBA）。此阶段不读取任何信号或状态，也不做任何判定。

阶段二若有任何状态被更新，写入的值自下一 round 起对读取者可见，可能构成新的边沿（级联时钟由此在同一 `eval()` 内逐级推进），于是进入下一 round；若没有任何状态更新，`eval()` 返回。写成伪代码：

```
def eval():
    while True:
        组合求值：按拓扑序执行所有动作的 lambda；
                Update 只记录意图（返回值 + 激活标记），不写状态
        状态更新：激活的 Update 按优先级链应用意图，写入统一生效
        if 本轮没有任何状态被更新:
            return
```

### 5.3 边沿检测

边沿即 Bool 信号值的跳变：0 变 1 为上升沿，反之为下降沿。

边沿检测发生在组合求值阶段，由 Update 对自己事件列表中的信号逐个判定。

Update 内部保存每个事件信号的旧值，执行时与当前值比较即知命中哪个边沿，判定后随即把旧值更新为当前值（旧值的初值取信号的初始值，不产生伪边沿）。

每条 Update 每个 round 恰好执行一次，因此每个边沿每 round 至多被检测一次，不漏检也不重复触发。且检测发生时 Update 的输入都已求值完毕（拓扑序保证）、状态写入尚未生效（§5.2），促成以下性质：

- 级联时钟：round $j$ 提交的 Reg 若被边沿事件引用，其跳变在 round $j+1$ 被检测为新边沿——分频器级联逐级翻转；
- 毛刺免疫：round 内组合求值的中间波动对检测不可见；
- 采样一致性：以同一边沿为事件的 Update 读到相同的值，同进同退。

两个边界情况的约定：

- 零延迟振荡：若某个时钟每个 round 都翻转，`eval()` 可能永不收敛——设置 round 上限，超限报错并附时钟环路径；
- 对输入时钟的采样约束：每个来自外部输入信号的时钟，在两次 `eval()` 之间至多跳变一次，否则边沿会被漏检。

---

## 6. 调试与观察

### 6.1 命名与层次路径

诊断与波形输出都依赖确定性的层次路径名（如 `top.sr.q`），这要求每个实体知道自己在上层模块中的成员名。C++26 的静态反射可以编译期枚举成员名，但编译器支持尚不稳定，不作为依赖。

Wolvicmod 的命名在创建点完成：结构成员不写作普通成员声明，而由 Module 的创建方法构造——`createIn`、`createOut`、`createWire`、`createReg`、`createMem`、`createChildModule`。创建方法完成三件事：构造对象（存储由框架持有，返回引用绑定为成员）、把名字挂载到当前模块的命名表、挂载时在模块内查重（重名直接报错）。层次路径沿模块树自动拼出。

```cpp
struct Counter : Module {
    In<Bool>&      clk  = createIn<Bool>("clk");
    Out<uint32_t>& dout = createOut<uint32_t>("dout");
    Reg<uint32_t>& cnt  = createReg<uint32_t>("cnt");

    Counter() { ... }
};
```

宏把名字收敛到一处，声明保持一行：

```cpp
#define IN(T, name)     In<T>&      name = createIn<T>(#name)
#define OUT(T, name)    Out<T>&     name = createOut<T>(#name)
#define WIRE(T, name)   Wire<T>&    name = createWire<T>(#name)
#define REG(T, name)    Reg<T>&     name = createReg<T>(#name)
#define MEM(T, R, name) Mem<T, R>&  name = createMem<T, R>(#name)
#define SUB(T, name)    T&          name = createChildModule<T>(#name)
```

宏参数按逗号切分，类型参数自身含逗号时（如 `std::array<T, N>`）先取别名再进宏。

机制要点：

- 引用成员的默认初始化器在基类之后、构造函数体之前按声明顺序执行，命名表在基类就绪后即可用，时序与 §4.1 的构造期模型一致；
- 模块禁止拷贝：拷贝构造与赋值在 Module 基类中删除——引用成员加框架持有存储，拷贝没有语义；
- 注册了动作但未经创建方法命名的实体，elaboration 报错；
- 将来 C++26 反射成熟后，可用编译期成员枚举替代显式名字，命名表接口不变。

### 6.2 诊断

任何建模错误都不被静默吸收，报错必附层次路径。

| 错误类别 | 检测点 | 机制 |
|---|---|---|
| 多驱动 / 悬空 | elaboration | 结构合法性检查（§4.2） |
| 未命名实体 | elaboration | 命名表对账（§6.1） |
| 组合环不收敛 | 运行时 | 迭代趟数上限，报错附环路径（§4.3） |
| 时钟零延迟振荡 | 运行时 | round 上限，报错附时钟环路径（§5.3） |
| 偷读（读未声明信号） | 运行时（调试模式） | 调试模式插桩对账（§6.4） |
| 多 Update 竞争 | 运行时（调试模式） | 可选互斥断言（§6.4） |
| 隐式状态 | 构造性消除 | Wire 无保持语义，条件保持必须显式 Reg |

### 6.3 波形与 trace

- 波形 dump：按层次路径输出信号与状态的值序列，格式为 FST；
- round 级事件记录：每个 round 的边沿命中、激活的 Update、提交的状态写，供回溯。

### 6.4 调试开关

- 读集对账插桩：调试模式下记录 lambda 实际访问的对象，与声明读集比对，偷读在首次仿真即报错；
- Update 互斥断言：可选开启，同一状态的多条 Update 同时激活时报告——默认行为是按注册顺序定优先级，不视为错误。

---


