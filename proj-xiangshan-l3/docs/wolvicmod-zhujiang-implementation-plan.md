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
| 1 | **Flit 类型层**：zhujiang 五件套（ReqFlit/RespFlit/DataFlit/SnoopFlit/HReqFlit）+ xscache 四件套（CHIREQ/CHIRSP/CHIDAT/CHISNP）的 C++ struct 位级定义 + `RingSlot`；定死位宽（尤其 DBID 12↔16） | ✅ 已完成（`model/`，`tests/test_flit.cpp` 11 用例；位宽经生成 RTL 端口实证） |
| 2 | **P1 Ring**：RingSlot 链路寄存器 → RouterStop（InjQueue + ChannelTap 注入仲裁 + EjectBuffer + RR 合流）→ RnRouter 地址译码 → STOP_TABLE 表驱动组装 10 站 | ✅ 已完成（`model/ring/`：eject_buffer.h / channel_tap.h / ring.h；单测 `tests/test_ring.cpp` 9 用例；环级对拍 `verify/cosim/harness_ring.cpp` + `run.sh ring`：wolvicmod Ring vs RTL ZRING 合成流量，3 seed × 50 万拍 × 1.62 亿次比对/seed = **4.86 亿次比对零失配**） |
| 3 | **P2 边界通路**：XscChiAdapter（纯组合）→ CcSocket（PDC token 缓冲）→ HomeWrapper 外壳（ChiBuffer + friends 选 lan + ERQ 选址）；HNF 内部先用行为桩（固定延迟回响应） | ✅ 已完成（`model/cc/` pdc.h / cc_socket.h / xsc_chi_adapter.h、`model/home/` home_shell.h / hnf_stub.h、`model/zj_l3.h` 组装；单测 test_pdc / test_adapter / test_home_shell / test_zj_l3 全绿；socket 对拍 `run.sh socket`：3 seed × 20 万拍 = **5220 万比对零失配**；**coremark 前端 2 万拍 trace 重放到 CC 边界**：`make replay` → `tests/test_trace_replay.cpp`，L2 CHI 缝 + `ccn_0_0x8.io_dev_*` 双侧逐拍比对 = **265,459 比对零失配**；bits 按 valid 门控比对——difftest `RANDOMIZE_REG_INIT` 下 valid=0 的 bits 是随机垃圾） |
| 4 | **P4a 两个桥前移**（从框架文档 P4 拆出）：SNodeAxiBridge + HiNodeAxiLiteBridge。与 DongJiang 互不依赖；做好后"桩 HNF → 真桥 → AXI"即端到端通路，给 P3 提供可工作的内存后端 | ✅ 已完成（`model/bridge/` axi_flit.h / bridge_cm.h / axi_data_buffer.h / snode_axi_bridge.h / hinode_axilite_bridge.h + `prefab/xsarb.h` 增 CondVipArb；单测 `tests/test_bridge.cpp` 5 用例、test_zj_l3 全通路；桥级对拍 `run.sh bridge`：S(outstanding=64)/HI(8) 各 2 seed × 10 万拍 + 排空 = **941 万比对零失配**。**对拍揪出激励侧协议违例**：ECA 写的 CompAck 与写数据同拍到达同一 CM 时，RTL BaseCtrlMachine 的 rx.data 分支会覆盖 rx.resp 分支写入的 compAck（香山依赖 CHI 保序：CompAck 须后于写数据）→ CM 永久泄漏（模型忠实复现同行为故零失配）；harness 改为写数据发完再回 CompAck 后，5 万拍 timer 断言全部消失） |
| 5 | **P3 DongJiang 全量**（工作量核心，内部分五步，每步独立对拍）：5.1 Directory（DirectoryBase llc/sf ×2，4 拍流水、lockTable、reservation、PLRU）→ 5.2 DataBlock（BeatStorage 5 拍、DataBuffer、DBIDCtrl、DataCM 八态 FSM）→ 5.3 Backend（Commit 112 + Read/Write/Snoop/Replace 四 CM 池 + 译码 Pipe + 仲裁网络）→ 5.4 Frontend（FastQueue→ToChiTask→TaskBuffer→Block s0/s1→PoS，三条阻塞源 + retry/sleep）→ 5.5 ChiXbar（组合分发，最后收口） | 5.1 ✅（`run.sh dir` 544 万比对零失配）。**5.2 ✅**（`run.sh db` 213 万比对零失配）。**5.3 ✅ 已完成**（`model/dj/`：dj_decode.h + dj_decode_table.inc（DecodeDump 机械转储 1904 表项）+ backend_types.h + commit.{h,cpp}（BackendDecode 2 拍 Pipe + CommitEntry 五态×112 + Commit）+ replace.{h,cpp}（ReplaceEntry 十八态×64 + ReplaceCM Alloc/reqPoS 矩阵）+ cm.{h,cpp}（Snoop/Read/Write 三 CM 池）+ qosrr.h + backend.{h,cpp} 顶层；`tests/test_decode.cpp` 锚点；对拍 `run.sh backend`（harness = HN 环境全模拟：decode 表合法 CommitTask + CHI/目录/dataResp/posResp 响应器）3 seed × 15 万拍 = **1445 万比对零失配**。语义提炼 `docs/dongjiang-semantics.md` §5-6）。**5.4 ✅ 已完成**（`model/dj/`：frontend_types.h + frontend.{h,cpp}（ReqToChiTask + TaskEntry×16/TaskBuffer（sort/lock 语义）+ Block 三阻塞源 + PosEntry×16/PosSet×4/PosTable + FrontendDecode（fstDec/secDec/GetDecRes + respCompData 快路 + cleanUnuseDB）+ 顶层）；对拍 `run.sh frontend`（harness = 影子目录 4 拍 respDir + PoS 管理器全生命周期 + getAddr 地址表，FE_INTCMP 白盒内部寄存器对拍定位）3 seed × 15 万拍 = **766 万比对零失配**。四处建模陷阱（RegNext|comb 分离、PriorityMux 无匹配取末值、arb out.ready 与 lock 无关、lockIdx 默认 N-1）记录于 §7.8）。**5.5 ✅ 已完成**（`model/dj/chixbar.{h,cpp}`：rxReq 按 addr[6] 双 bank 重定向 + QoS==0xf 改道 HPR + tx 直通 SrcID:=0/CBusy；对拍 `run.sh chixbar` 3 seed × 10 万拍 = **466 万比对零失配**，语义 §8）。**P3 收口 ✅**（`model/home/dongjiang.{h,cpp}` 顶层组装替换 HnfStub：2×FE + Backend + Directory + DataBlock + ChiXbar + 合流仲裁 + cBusy；Frontend 补全 HPR 通路（8 项 HprTaskBuffer + selectReq）；修正 BackendDecode hnTxnIdOut 时序（RegNext 第二级，集成发现的模型 bug）；test_zj_l3 以合法激励（order=3/eca=1/CompAck）+ 内存模型端到端打通（ReadReceipt + CompData×2 + writeDir + DS save），proj ctest 15/15，五个子模块对拍全套回归零失配；语义 §9） |
| 6 | **P4b 顶层组装 WolvicZjTop**：rn/memAXI/cfgAXI 三边界 + stubs（RI tie-off、M→全局复位、P→1 拍线延迟） | ✅ 已完成（`model/wolvic_zj_top.{h,cpp}` 由 zj_l3 演进重命名；RI tie-off 桩内收——inject 常 0、eject rdy 常 1，对齐 ZCI 生成 RTL s_axi_main 常 0 与 ZRING 裁剪语义。C++ FST 直读提取器 `verify/trace/extract_top_trace.cpp`：libfst FacProcessMask 只读三边界信号，处理 Verilator 64b 拆片 `name [msb:lsb]_i` 与恒等 alias 共享 handle；与 extract_cc_trace.py 交叉验证 181 万比对零失配；3.6GB 全程 FST 提取仅 1.8s。**时钟门控 woken 建模**：HomeWrapper DoubleCounterClockGate 冻结上电横扫至首请求唤醒，两 hnf 独立；DongJiang 顶层 `woken` 锁存 + SpSram/DpSram/DBIDPool `clk_en` 功能使能 + 横扫首拍无条件推进——框架文档"门控等价常开"仅稳态成立，语义见 dongjiang-semantics §9 修正版。**coremark 全程 golden trace 重放**（`tests/test_wolvic_top_replay.cpp`，`make replay-top`）：316,748 拍 × **5,423,490 次比对零失配**——驱动 L2 tx 三通道+rx ready+mem/cfg b/r 返回（RTL 仿真内存/外设真实应答），比对 L2 ready/rx+mem/cfg aw/w/ar+cc_tx_req 断言） |
| 7 | **P5 DPI-C 集成**：静态库 + Verilog 薄壳（或直接链 difftest emu，二选一），跑 coremark 系统级验证 | difftest 过 + `HIT GOOD TRAP` + cycleCnt=316,801 与 RTL 完全相等 |

每步退出判据对应框架文档 §5 的四级验证策略（原语对拍 → 模块 trace 对拍 → 重放对拍 → 系统级）。trace 均在边界抓取，中间层内部如何简化不影响对拍有效性——这是简化合法性的落地保证。
