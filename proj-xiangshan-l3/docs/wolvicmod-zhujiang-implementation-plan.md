# Wolvicmod 建模 ZhuJiang：简化准则与实施计划

配套文档：`wolvicmod-zhujiang-hierarchy.md`（模块层次框架，下称"框架文档"）。本文回答两个问题：周期精确前提下哪些结构可以简化、按什么顺序实施。

## 1. 简化判定准则

"周期精确"只约束**边界信号逐拍一致**（框架文档开头定义）。一个结构要素必须镜像 RTL 建模，当且仅当它影响以下三者之一：

1. **延迟**——信号最早/最晚能在第几拍变化（每级流水寄存器、SRAM 读延迟链）；
2. **占用/反压**——同时能容纳多少事务在途（每个队列深度、DBID/PoS/Commit/CM 池大小、EjectBuffer 深度、socket token 数）；
3. **竞争结果**——多请求同拍争一个资源时谁赢（每个仲裁点的类型与优先级、FSM 状态转移的先后次序）。

凡不落入这三条的均为纯行为，用 C++ lambda/switch 黑盒表达：flit 字段变换、译码表、地址计算、数据搬运。改动它们不改变任何边界时序。

## 2. 可安全简化清单

| 简化项 | 理由 |
|---|---|
| 时钟门控（DoubleCounterClockGate） | 组合唤醒，时序等价于常开（框架文档 §1） |
| M 节点两相复位 → 全局同步复位 | 只影响复位过程，不影响运行期逐拍行为 |
| RI 节点全 tie-off 桩 | XiangShan 侧 DMA 已 tie-off，数据通路不激活 |
| QoS 分级仲裁 → 普通仲裁 | 环内仲裁不使用 QoS，QosRRArb 退化为单层 |
| DatalessCM | RTL 本就未例化 |
| HPR/DBG 环、BBN、c2c、MBIST/DFT、ZJPerf | 配置关闭或纯观测逻辑，不进数据通路 |
| 译码表（Read/Write/Dataless DecodeTable） | 纯查表，转写为 switch/lambda |
| ChiXbar、XscChiAdapter、字段重映射 | 全组合连线，无状态 |
| SRAM 宏 → Mem + ValidPipe 延迟链 | 时序等价，已封装为 SpSram/DpSram 原语 |
| FSM 内部状态编码 | 编码方式任意，只要转移拍数与占用期间外部可见行为一致 |

**条件化简化**（可以做，但必须带断言兜底，触发即报错）：

- 仲裁器优先级合并——仅当能证明该仲裁点在所跑负载下永不超过 1 个请求同拍竞争；保留断言；
- 串联队列合并——总深度不变且无中间抽头时可合为一个，但须核对 flow/pipe 语义差别（FastQueue 的"一拍气泡"即此类坑）。

**绝不能简化**：每级打拍位置、每个队列深度、每个仲裁点、TaskBuffer/PoS/Commit/DataCM 状态机与超时机制、防死锁三件套（环 rsvd 令牌、EjectBuffer VIP 末槽、目录 lockTable）。少一个就可能在某次反压下死锁（框架文档 §7）。

## 3. 实施计划（有序）

原则：自底向上；每层可独立对拍；先打通"能放 trace 的通路"，再啃工作量核心（DongJiang）。

| 步 | 内容 | 验收 |
|---|---|---|
| 0 | **P0 时序原语库** | ✅ 已完成（ctest 100 用例全绿） |
| 1 | **Flit 类型层**：zhujiang 五件套（ReqFlit/RespFlit/DataFlit/SnoopFlit/HReqFlit）+ xscache 四件套（CHIREQ/CHIRSP/CHIDAT/CHISNP）的 C++ struct 位级定义 + `RingSlot`；定死位宽（尤其 DBID 12↔16） | 位段辅助函数单测 |
| 2 | **P1 Ring**：RingSlot 链路寄存器 → RouterStop（InjQueue + ChannelTap 注入仲裁 + EjectBuffer + RR 合流）→ RnRouter 地址译码 → STOP_TABLE 表驱动组装 10 站 | 合成流量环上传输 trace 对拍 |
| 3 | **P2 边界通路**：XscChiAdapter（纯组合）→ CcSocket（PDC token 缓冲）→ HomeWrapper 外壳（ChiBuffer + friends 选 lan + ERQ 选址）；HNF 内部先用行为桩（固定延迟回响应） | L2↔环↔桩通路连通，coremark 前端 trace 可重放到 CC 边界 |
| 4 | **P4a 两个桥前移**（从框架文档 P4 拆出）：SNodeAxiBridge + HiNodeAxiLiteBridge。与 DongJiang 互不依赖；做好后"桩 HNF → 真桥 → AXI"即端到端通路，给 P3 提供可工作的内存后端 | 桥级 CHI→AXI 定向用例对拍 |
| 5 | **P3 DongJiang 全量**（工作量核心，内部分五步，每步独立对拍）：5.1 Directory（DirectoryBase llc/sf ×2，4 拍流水、lockTable、reservation、PLRU）→ 5.2 DataBlock（BeatStorage 5 拍、DataBuffer、DBIDCtrl、DataCM 八态 FSM）→ 5.3 Backend（Commit 112 + Read/Write/Snoop/Replace 四 CM 池 + 译码 Pipe + 仲裁网络）→ 5.4 Frontend（FastQueue→ToChiTask→TaskBuffer→Block s0/s1→PoS，三条阻塞源 + retry/sleep）→ 5.5 ChiXbar（组合分发，最后收口） | LLC hit/miss/snoop/同地址冲突定向用例对拍；替换第 3 步的行为桩 |
| 6 | **P4b 顶层组装 WolvicZjTop**：rn/memAXI/cfgAXI 三边界 + stubs（RI tie-off、M→全局复位、P→1 拍线延迟） | standalone coremark golden trace 全程重放逐拍比对 |
| 7 | **P5 DPI-C 集成**：静态库 + Verilog 薄壳（或直接链 difftest emu，二选一），跑 coremark 系统级验证 | difftest 过 + `HIT GOOD TRAP` + cycleCnt=316,801 与 RTL 完全相等 |

每步退出判据对应框架文档 §5 的四级验证策略（原语对拍 → 模块 trace 对拍 → 重放对拍 → 系统级）。trace 均在边界抓取，中间层内部如何简化不影响对拍有效性——这是简化合法性的落地保证。
