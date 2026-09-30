# wolvicmod ZhuJiang L3 模型：测试方法与测试报告

本文是项目的验证总账：正确性判据、四级验证策略、全部对拍数据、性能结论与复现入口。模型层次与实现细节见 `wolvicmod-zhujiang-model.md`，DPI-C 集成与构建见 `xiangshan-wolvicmod-l3-integration.md`，ZhuJiang 概念背景见 `zhujiang-primer.md`。

**最终结论**：wolvicmod 周期精确建模的 ZhuJiang L3（WolvicZjTop）经 DPI-C 整体替换 XiangShan 昆明湖 V3 SoC 中的 ZhuJiang RTL，coremark 系统级仿真 `HIT GOOD TRAP`、difftest 663,692 指令零失配、**cycleCnt = 316,801 与 RTL 逐拍完全相等**；各级对拍累计逾 10 亿次信号比对零失配。

## 1. 验证方法论

### 1.1 正确性判据：周期精确的定义

**周期精确 = 在模型边界上，每一拍的信号取值与 RTL 完全一致。**模型边界三件套：

| 边界 | 接口 | 说明 |
|---|---|---|
| L2 侧 | `DecoupledPortIO`（xscache CHI flit，纯 valid/ready 六通道：tx req/rsp/dat + rx rsp/dat/snp） | 无 linkactive/sysco 链路状态机 |
| 内存侧 | memAXI（AXI4 master，id 6b / addr 48b / data 256b） | S 节点桥出口 |
| 外设侧 | cfgAXI（AXI4 master，id 3b / addr 48b / data 256b） | HI 节点桥出口，MMIO 必经 |

校验基线：coremark-2-iteration 的 cycleCnt = 316,801、instrCnt = 663,692（RTL ZhuJiang 配置实测，IPC 2.094981）。

"周期精确"只约束边界信号逐拍一致。一个结构要素必须镜像 RTL 建模，当且仅当它影响**延迟**（每级流水寄存器、SRAM 读延迟链）、**占用/反压**（每个队列深度、DBID/PoS/Commit/CM 池大小）或**竞争结果**（每个仲裁点的类型与优先级、FSM 转移次序）三者之一；其余纯行为（flit 字段变换、译码表、地址计算）用 C++ lambda/switch 黑盒表达。trace 均在边界抓取，中间层内部如何简化不影响对拍有效性——这是简化合法性的落地保证。

### 1.2 四级验证策略

| 级 | 方法 | 载体 |
|---|---|---|
| ① 原语级对拍 | 时序预制件 vs 真实参考 RTL（Chisel 源码 → firtool → Verilator 生成），同一激励逐拍比对；定向随机激励 + valid/ready 全组合扫掠 | `verify/cosim` + refgen + `run.sh`（已退役，见 §1.4） |
| ② 模块级合成流量对拍 | cosim harness 驱动单个模块/子系统 vs 对应 RTL，随机激励 + 反压扫掠；harness 模拟被测模块的整个运行环境（响应器、影子目录、PoS 管理器等） | 同上 |
| ③ golden trace 重放对拍 | 从 RTL emu 抓模型边界每拍 trace（FST → 列式文本），重放进 wolvicmod 模型逐拍驱动输入侧、比对输出侧；首个不一致拍即定位 | `make trace-front` / `make trace` + ctest 重放用例 |
| ④ 系统级 | 模型经 DPI-C 替换进 SoC，跑 coremark + difftest | `make emu WOLVIC=1` + coremark |

### 1.3 比对口径

- **bits 仅在 valid=1 时比对**：difftest 开 `RANDOMIZE_REG_INIT`，valid=0 时 RTL 的 bits 是随机初值垃圾（don't-care），不比。
- **每个对拍给出三要素**：seed 数 × 拍数 × 比对次数。比对计数 = 逐拍逐信号（含 valid/ready 与各 bits 字段）的比较次数。
- 对拍 harness 本身须遵守被测 RTL 的协议约定（如 CHI 保序），激励合法性是零失配结论成立的前提（§2.2 缺陷①即由此发现）。

### 1.4 测试组织现状（2026-09-29 收敛后）

| 位置 | 内容 |
|---|---|
| `tests/` | doctest 快速回归（纯 C++，毫秒级；每测试文件独立 ctest 条目，`ctest -R <名字>` 单跑）；含组合穿透常驻审计与 golden trace 重放用例 |
| `verify/` | 两条端到端路径的基建：① **孤立 L3 共栖 A/B 回放**（`make replay`，wolvic vs verilated 独立 ZhuJiang RTL 对同一 coremark 全程 trace 逐拍比对三边界）；② **XiangShan 集成 emu**（`make emu [WOLVIC=1]` + `make coremark`）。外加 DPI 冒烟台（`verify/dpi/run.sh`）与 trace 提取器（`verify/trace/`） |

预制件元件级 RTL 对拍基建（`verify/cosim` + refgen + `run.sh`）已于 2026-09-29 删除——其对拍结论已固化（即 §2.1 表中数据），演进中的等价性保障由路径①承担；wolvicmod 框架仓的 `verify/`（chisel3 标准库元件对拍）保留不动。

## 2. 正确性测试报告

### 2.1 分层汇总表

全部对拍**零失配**。各层数据（三要素口径，从源文档逐项核对）：

| 层级 | 对象 | 规模 | 比对次数 | 结果 |
|---|---|---|---|---|
| ① 原语 | wolvicmod 预制件（chisel 通用 4 元件 10 配置 × 3 seed） | 330 万拍 | 1,241 万 | 零失配 |
| ① 原语 | 项目侧预制件（XiangShan 生态 6 元件 17 配置 × 3 seed） | 528 万拍 | 2,914 万 | 零失配 |
| ① 原语小计 | | **858 万拍** | **4,155 万** | 零失配 |
| ② 环 | Ring vs RTL ZRING 合成流量（3 seed × 50 万拍 × 1.62 亿/seed） | 150 万拍 | **4.86 亿** | 零失配 |
| ② socket | CcSocket（3 seed × 20 万拍） | 60 万拍 | **5,220 万** | 零失配 |
| ② 桥 | S 桥（outstanding=64）/ HI 桥（8），各 2 seed × 10 万拍 + 排空 | 40 万拍 | **941 万** | 零失配 |
| ② DongJiang | Directory | 合成流量 | 544 万 | 零失配 |
| ② DongJiang | DataBlock | 合成流量 | 213 万 | 零失配 |
| ② DongJiang | Backend（3 seed × 15 万拍） | 45 万拍 | 1,445 万 | 零失配 |
| ② DongJiang | Frontend（3 seed × 15 万拍） | 45 万拍 | 766 万 | 零失配 |
| ② DongJiang | ChiXbar（3 seed × 10 万拍） | 30 万拍 | 466 万 | 零失配 |
| ③ trace | CC 边界重放：coremark 前端 2 万拍（有效 19,965 拍，前 35 拍复位），L2 CHI 缝 + `ccn_0_0x8.io_dev_*` 双侧逐拍 | 2 万拍 | **265,459** | 零失配 |
| ③ trace | coremark 全程 golden trace 重放（`make replay-top`）：驱动 L2 tx 三通道 + rx ready + mem/cfg b/r 返回，比对 L2 ready/rx + mem/cfg aw/w/ar + cc_tx_req 断言 | **316,748 拍** | **5,423,490** | 零失配 |
| ③ trace | 孤立共栖 A/B 回放：verilated 独立 ZhuJiang RTL 对同一 trace 重放（RTL 侧直接证据） | 316,748 拍 | RTL 5,106,742 / wolvic 5,423,490 | 各自零失配 |
| ③ trace | **microbench 负载**共栖 A/B 回放（2026-09-30 补测，`make replay DUT=both TRACE_FILE=build/trace/microbench.txt`） | 252,597 拍 | RTL 4,104,436 / wolvic 4,357,033 | 各自零失配 |
| ④ 系统级 | `make emu WOLVIC=1` + coremark + difftest（EMU_THREADS=16，host time ≈ 876s） | 全程 | 663,692 指令 | 三判据全中（见下） |
| 回归 | proj ctest（含组合穿透审计与全程 trace 重放） | — | — | **17/17 全绿** |

系统级验收判据（三条全中才算过，2026-09-27 实测全中）：

1. `HIT GOOD TRAP`——coremark 跑完且软件结果正确（CRC 全对）；
2. difftest 全程无失配（663,692 条指令 vs NEMU）；
3. `cycleCnt = 316,801`——与 RTL ZhuJiang golden **逐拍完全相等**。

### 2.2 对拍发现的真实缺陷（测试有效性的证据）

零失配不等于测试无效——对拍过程揪出了多个真实缺陷与语义陷阱，均已修复或忠实复现：

| # | 缺陷 | 发现环节 | 内容与处置 |
|---|---|---|---|
| ① | ECA 写 CompAck 与写数据同拍到达时被覆盖 | 桥级对拍（P4a） | RTL `BaseCtrlMachine` 中 `rx.resp`（CompAck）与 `rx.data` 两个 when 块都写 `compAck`，同拍同到时按 Chisel 后连接优先，**rx.data 分支覆盖 rx.resp 分支**——CompAck 被丢弃、CM 永久占住（5 万拍后触发调试断言）。香山依赖 CHI 保序（CompAck 后于写数据）；**模型按 RTL 原样复现该覆盖语义**（故零失配），harness 改为写数据发完再回 CompAck |
| ② | BackendDecode `hnTxnIdOut` 时序 bug | DongJiang 顶层集成 | RTL 中 `hnTxnIdOut.valid = RegNext(decValReg)`（第二级流水）；模型曾用首级 `dec_val_reg` 组合输出，导致 commit **早一拍消费陈旧 cmtCode**（有效 task 的 commit code 依赖后端重译码，滞后一拍即丢 OpSend/WriLLC，commit 永久卡在 kCommit）。修正为 RegNext 第二级后回归零失配 |
| ③ | Frontend 四处建模陷阱 | frontend 对拍 | RegNext 与 comb 须分离建模；PriorityMux 无匹配时取**末值**；arbiter `out.ready` 与 lock 无关；lockIdx 默认 **N-1**（空输入语义） |
| ④ | 时钟门控上电横扫冻结 | P4b 全程 trace 重放 | HomeWrapper 时钟门控将上电横扫（目录 SRAM/DBID 预充）冻结至首请求唤醒，两 HNF 独立（coremark 实测 hnf_0 cyc~1038 醒、hnf_1 cyc~9323 醒）。**省略则首笔内存访问早 ~1000 拍，边界立即失配**；已建模为 DongJiang 顶层 `woken` 锁存 + 横扫/预充 `clk_en` 功能使能 |
| ⑤ | getDBID 多匹配取首个 dcid | DataBlock 对拍 | 多匹配时 RTL 按 PriorityEncoder 取**首个** dcid；模型初版取末位，在违例域才暴露，修为忠实 |
| ⑥ | DPI 打包三 bug | DPI 冒烟（`verify/dpi/run.sh`） | `bit_pack.h::setWide` 末字越界；`fromSv`/`toSv` 须按 nbits 截断（Verilator `svBitVecVal` 是 uint32，缓冲 `ceil(nbits/32)`）；`elaborate()` 之后才允许 set 端口。冒烟台不经 emu、几十秒定位 |

## 3. 性能报告

本节是对外留存的性能结论（原始逐次实验记录 `perf-breakdown.md` 已于文档整理时删除，git 历史可溯）。所有数据同机 fresh 测量（32 核 / 186GB，Verilator 5.047，coremark-2-iteration + difftest）；所有运行 cycleCnt 均为 316,801、指令流一致——差异纯粹是仿真宿主开销。

### 3.1 基线与现状对照

| 配置 | 线程 | 每拍 wall | 全程 host time | 备注 |
|---|---|---|---|---|
| RTL ZhuJiang（基线） | 8 | 1.00 ms | **316s** | 两次独立复现 313s/316s |
| wolvic（优化前） | 16 | 2.67 ms | 876s | 优化前最优点 |
| wolvic（优化前） | 8 | 3.83 ms | 1212s | |
| wolvic（优化前） | 1 | 11.65 ms | ~3690s（推算） | 50k 实测 582.5s 推算 |
| RTL ZhuJiang（2026-09-29 复测） | 8 | 0.940 ms | **297.6s** | cycleCnt/IPC 与 wolvic 逐值相同 |
| **wolvic（最终，2026-09-29 复测）** | 8 | **0.862 ms** | **272.9s（-8.3%）** | **反超 RTL**；difftest 全程无失配 |

最终复测两侧 `cycleCnt = 316,801 / IPC = 2.094981` 逐值相同——行为等价性在 emu 路径闭环。

### 3.2 优化历程摘要

累计 **emu host time -77.5%**（8T 1212s → 272.9s，从慢 3.83× 到快 1.09×）；**孤立回放全程 eval -92%**（15.39s → 1.26s，316,748 拍，taskset 钉核）。关键步骤：

| 步骤 | 内容 | 效果 |
|---|---|---|
| 脏驱动求值 + 去间接调用 | 值变化代际戳驱动，静止逻辑锥整片跳过；lambda 去 `std::function` 双层间接 | emu 1T 11.65 → **9.13 ms/拍**；模型侧 A/B 2.87×（491s → 171s） |
| 推送派发 | 轮询扫描反转为反向依赖表推送 + 层级桶就绪队列 | 模型侧再 1.99×；emu 1T → **8.93 ms/拍** |
| Flat 引擎 / 别名化 / 快速提交 / 成员位图 / 类型聚类 | 静态序 + 活性位图；恒等连接 elaboration 期消除；相位二去虚调用 | 回放 eval 持续压缩 |
| 建模层整项化 | PosTable / TaskBuffer / Commit 条目阵列拍平为整项 Reg + 单条 update | 全程 eval 降至 ~20.5s |
| 静止门控全覆盖 | 预制件拍平 + TaskBuffer/PosTable/DirectoryBase/Ring/CcSocket/AXI 桥静止门 | 全程 eval **5.00s** |
| 守卫预过滤 | 每拍 update 激活 1087 条中 82.8% 是空跑；bool 写入点匹配方向跳变时先读守卫，假则不派发 | eval 5.00 → **1.97s**——**首次快过 verilated RTL**（0.57×） |
| 守卫直读 + edgeBit | 守卫改直指针单次加载；per-execPos 边沿事实位图取代 prev 机制 | eval 1.97 → **1.26s**（对 RTL eval 0.42×） |

另有一条证伪记录：边界输出 o_out 按通道拆寄存器（收益≈0）——生成代码实证边界下游组合锥被 Verilator 调度进 NBA 区、挂时钟位每拍无条件运行，与触发粒度无关。

**emu 翻转幅度小于孤立回放的原因**：emu 每拍成本由 SoC 基线 + difftest 主导（两侧共有基线 ~6.44 ms/拍 @1T），L3 只占小头（模型侧占比 ~9.7%），且 wolvic 侧 L3 走单调用点串行 DPI——L3 求值的大幅提速摊到整拍只剩 ~8%。

**负载扩展补测：microbench（2026-09-30）**。L3 压力型微基准负载（`make trace IMG=microbench.bin TNAME=microbench FULLN=3000000` 生成，252,597 拍；L3 流量密度明显高于含长启动期的 coremark，两侧每拍耗时均同比例上升），孤立回放钉核（`taskset -c 2`）单侧计时、两次取最优：

| 侧 | eval 最优 | 每拍 | 对 RTL 倍数 |
|---|---|---|---|
| wolvic | **1.375s** | 5,442 ns | — |
| verilated RTL | 3.819s | 15,118 ns | wolvic 快 **2.78×** |

对照 coremark 全程（316,748 拍）：wolvic eval 1.26s（3,978 ns/拍）、RTL ≈ 3.0s（≈9,470 ns/拍）、快 ≈2.4×——microbench 的高密度流量下模型优势进一步扩大。等价性对拍数据见 §2.1。

### 3.3 差距拆解结论（早期，优化前 1T 对 1T）

wolvic 版比纯 RTL 多出 11.65 − 7.23 = **4.42 ms/拍**（1.61×），归组拆解：

| 构成 | ms/拍 | 归属 |
|---|---|---|
| wolvicmod 模型框架调度 + DPI glue | ~2.5 | 框架可优化（§3.2 已绝大部分消除） |
| BlackBox 边界结构成本 | ~2.4 | Verilator 侧：BlackBox 端口使 L2 侧供给逻辑无法死码消除；o_out NBA 更新后下游组合逻辑每拍整体重估 |
| （减去）省掉的 L3 RTL 工作 | −0.9 | RTL 版需付、wolvic 版省掉 |

优化后剩余差距落在**边界结构桶，且成为唯一大头**；证伪实验表明这是 BlackBox 边界的固有结构成本，**"不改 Verilator"约束下无可行大收益路径**。

**多线程同步损耗机制**：DPI 调用点是每拍的全局收敛点——in_pack（1107b）依赖几乎所有生产者 MTask，o_out（1443b）被几乎所有消费者 MTask 依赖，每拍 MTask 图坍缩成 `[全部生产者] → [DPI] → [全部消费者]` 串行波次，同步延迟主导墙钟（wolvic 8T 自旋占 ~62%、16T ~73%；RTL 依赖图宽而浅、无全局汇聚点，仅 ~10%）。故 wolvic 版 MT 扩展性差（优化前 8T→16T 仅 1.43×，RTL 8T 近线性）。

### 3.4 复现方法与构建教训

**emu 变体构建 / 测速 / 采样 / 留存**（以 wolvic 1T 为例；集成构建细节见 `xiangshan-wolvicmod-l3-integration.md`）：

```bash
make -C proj-xiangshan-l3 emu WOLVIC=1 ET=<线程数>     # 构建（印记不匹配自动 clean）
cd proj-xiangshan-l3/XiangShan
./build/emu -b 0 -e 0 -C 50000 -i ./ready-to-run/coremark-2-iteration.bin \
  --diff ./ready-to-run/riscv64-nemu-interpreter-so                  # 测速（5 万拍窗口）
perf record -F 999 -o build/perf-<变体>.data ./build/emu ...          # 采样
make -C proj-xiangshan-l3 stash-emu NAME=<变体名>                     # 留存到 build/emu-variants/
```

**变体血统校验三件套**（采数前必做——`build/rtl` 不随 `WOLVIC_ZJ` 自动再生，绕过 proj Makefile 可能拿到 stale RTL 变体；优化期间曾因此误记成绩，后由指纹检查识破）：

```bash
strings -a <emu> | grep -c DataCM.scala   # wolvic 版 = 0；RTL 版 > 100
nm <emu> | grep -c wolvic_zj_step         # wolvic 版 = 3（含调用点）；RTL 版 = 0
cat XiangShan/build/.llc-config           # wolvic 版 = "ZhuJiang-wolvic trace="
```

**构建类型教训（已发生一次）**：正确性验证对构建类型不敏感，性能数字才敏感。从零配置得到空 `CMAKE_BUILD_TYPE`（-O0）时模型库**慢 ~9 倍而功能全绿**——回放的 0 失配完全掩盖了它（已修复：CMakeLists 未显式指定时强制 Release）。凡从零/换机复测性能，计时前做两步光线检查：① `grep CMAKE_BUILD_TYPE build/CMakeCache.txt` 是 Release；② 钉核（`taskset -c 2`）跑 30k 拍，wolvic eval 应在 0.2s 量级而非 2s+。

## 4. 覆盖边界与已知限制

| 项 | 现状 |
|---|---|
| 负载覆盖 | coremark-2-iteration（全程，正确性 + 性能基线）+ microbench（252,597 拍，L3 压力型负载，2026-09-30 补测）；Linux boot trace 抓取进行中，尚未纳入对拍 |
| 配置覆盖 | 单 tile DefaultConfig：ZhuJiang 32MB / 2 HNF（各 16MB），CHI Issue E.b，参数全部锁定。多核/多 tile、其他 LLC 容量未验证 |
| CHI 特性子集 | DataCheck/Poison 关闭（配置强制）；无 linkactive/sysco 链路管理（模型按常通链路处理）；无 DVM/MPAM 语义（mpam 字段落 zhujiang 零宽字段，无语义）；c2c、BBN、HPR/DBG 环未建模（配置关闭或不进数据通路） |
| 防死锁机制 | 环 rsvd 令牌、EjectBuffer VIP 末槽、目录 lockTable 在 coremark 中未必触发——靠合成流量对拍覆盖（环级 4.86 亿、socket 5220 万、Directory 544 万比对），少一个就可能在某次反压下死锁，故全部建模 |
| 观测信号 | `l3Miss` / `l3MissMatch` tie-off 为 `false.B`（模型不提供）；ZJPerf 性能计数器未建模 |
| 其他简化 | M 节点两相复位 → 全局同步复位；RI 节点全 tie-off 桩（XiangShan 侧 DMA 已 tie-off）；时钟门控稳态睡/醒不建（时序等价常开，上电横扫冻结已建模） |

## 5. 复现入口速查

| 命令 | 内容 |
|---|---|
| `make -C proj-xiangshan-l3 test` | ctest 回归（17/17，含组合穿透审计与全程 trace 重放） |
| `make -C proj-xiangshan-l3 replay-rtl && make -C proj-xiangshan-l3 trace && make -C proj-xiangshan-l3 replay DUT=both` | 孤立 L3 共栖回放：造 RTL → 造 coremark 全程 trace → wolvic vs verilated RTL 逐拍对拍（`DUT=rtl\|wolvic` 为孤立性能剖析） |
| `make -C proj-xiangshan-l3 trace IMG=<bin> TNAME=<名> FULLN=<拍数>` + `make replay DUT=both TRACE_FILE=build/trace/<名>.txt` | 换负载（microbench 等任意镜像）生成 trace 并回放；非 cm_full 不跑自动校验 |
| `make -C proj-xiangshan-l3 trace` | coremark 全程 golden trace 生成 + 自动重放校验（`ctest -R test_wolvic_top_replay`，即源文档所称 `replay-top`） |
| `make -C proj-xiangshan-l3 emu WOLVIC=1 && make -C proj-xiangshan-l3 coremark` | 系统级：wolvic 版 emu 构建 + coremark + difftest（三判据见 §2.1） |
| `bash proj-xiangshan-l3/verify/dpi/run.sh` | DPI 冒烟（不经 emu 的快速自检，预期 `[smoke-dpi] PASS`） |

注：早期文档中的 `make zjrtl-replay` / `make replay-top` 目标名已演进为上表中的 `replay` / `trace`（重放校验挂在 ctest 用例上）。详细集成构建流程、生命周期与排障见 `xiangshan-wolvicmod-l3-integration.md`。
