# Wolvicmod ZhuJiang L3 模型：实现层次与建模约定

本文是 as-built 实现记录：用 wolvicmod 周期精确重实现 XiangShan 昆明湖 V3 的 ZhuJiang（L3 + 环形 NoC）的工作已完工，模型经 DPI-C 集成进 XiangShan emu 跑通 coremark 并通过系统级验证。文档关系：ZhuJiang 概念背景见 `zhujiang-primer.md`，与香山 RTL 的集成方案见 `xiangshan-wolvicmod-l3-integration.md`，验证方法与测试数据见 `verification-report.md`。

**目标**（已达成）：用 wolvicmod 周期精确（cycle-accurate）重实现 XiangShan 昆明湖 V3 的 ZhuJiang（L3 + 环形 NoC），替换 RTL ZhuJiang 跑通 coremark。

**周期精确的定义**：在模型边界上，每一拍的信号取值与 RTL 完全一致——
- L2 侧：`DecoupledPortIO`（xscache.chi flit，valid/ready）
- 内存侧：AXI4 master（id 6b / addr 48b / data 256b）
- 外设侧：AXI4 master（id 3b / addr 48b / data 256b，cfg 口）
- 校验基线：coremark-2-iteration 的 cycleCnt = 316,801、instrCnt = 663,692（见 README §2.3）

实现方法论：**层次镜像 RTL**。凡影响时序的结构要素（每级流水寄存器、每个队列深度、每个仲裁点、每个 FSM）一一对应建模；行为（flit 字段变换、译码表、地址计算）用黑盒 C++ lambda 直接表达。引用的 `ZJ` 路径 = `XiangShan/XSCache/ZhuJiang/src/main/scala/`。

---

## 1. 目标配置（单核 DefaultConfig，参数全部锁定）

拓扑：`XiangShan/src/main/scala/top/ZhuJiangNoCTopology.scala:25-39`，10 个环节点：

| idx | nodeId | 类型 | 设备 | coremark 是否必须 |
|---|---|---|---|---|
| 0 | 0x00 | HF bank0 hfp0 | HomeWrapper#0 lan0 | ✅ |
| 1 | 0x08 | CC | SocketIcnSide（接 L2） | ✅ |
| 2 | 0x10 | HF bank1 hfp0 | HomeWrapper#1 lan0 | ✅ |
| 3 | 0x18 | RI | Axi2Chi（DMA，XiangShan 已 tie-off） | ❌ 桩 |
| 4 | 0x20 | HI（defaultHni） | AxiLiteBridge → cfg AXI | ✅（MMIO 必经） |
| 5 | 0x28 | HF bank1 hfp1 | HomeWrapper#1 lan1 | ✅ |
| 6 | 0x30 | S mem_0 | AxiBridge → mem AXI | ✅ |
| 7 | 0x38 | HF bank0 hfp1 | HomeWrapper#0 lan1 | ✅ |
| 8 | 0x40 | M | ResetDevice | ⚠️ 简化为全局复位 |
| 9 | 0x48 | P | 无（纯打拍站） | ✅（并入环延迟） |

每 HNF（DongJiang，共 2 个，各 16MB）关键参数（推导见探查报告，`ZJ/zhujiang/ZJParameters.scala:243-269`、`dongjiang/DJParameters.scala`）：

- LLC：16MB = 2 dirBank × 8192 sets × 16 ways；SF：2MB = 2 × 1024 sets × 16 ways，meta 1bit（nrSfMetas=1）
- PoS 128（64/dirBank：4 set × 16 way，way14/15 保留给 ReplaceCM）；Commit 112
- TaskBuffer：req 16 + hpr 8（每 Frontend）；DataBuffer 128×32B；DBID 池 2×64
- CM：DataCM 64 / ReplaceCM 64 / ReadCM 64 / SnoopCM 32 / WriteCM 32（DatalessCM RTL 未例化，不建）
- 时序常量：目录读 4 拍 / muticycle 2；DS 读 5 拍 / muticycle 2
- 地址切分：offset[5:0]、dirBank=addr[6]、bankId=addr[12]、ci=addr[47:44]=0、hnTxnID={dirBank 1, posSet 2, posWay 4}

省略清单：HPR/DBG 环（hasHprRing=false、hwa 关）、BBN（`require(!hasBBN)`）、c2c、MBIST/DFT、QoS（环内仲裁不使用）、RI 数据通路、ZJPerf。时钟门控（DoubleCounterClockGate）**稳态等价于常开**（working 维持 + inbound 组合唤醒零延迟），不建睡/醒循环；但**上电横扫期冻结**必须建模——DongJiang 顶层 `woken` 锁存 + 横扫/预充计数功能使能（§3.4 as-built 与 §5.9）。

---

## 2. wolvicmod 建模约定

### 2.1 信号与类型

- **flit 用 C++ struct 位级定义**：`ReqFlit/RespFlit/DataFlit/SnoopFlit/HReqFlit`（zhujiang 格式，`ZJ/zhujiang/chi/Flit.scala:27-127`）与 `CHIREQ/CHIRSP/CHIDAT/CHISNP`（xscache 格式，`XSCache/src/main/scala/xscache/chi/Message.scala:428-557`）各一套，字段用 `uint64_t` + 位段辅助函数。注意 **DAT.DBID：zhujiang 16b vs xscache 12b**，适配层做零扩展/截断（对齐 `ZhuJiangBridge.scala:213-214,232`）——✅ 已实现于 `proj-xiangshan-l3/model/flit/`（`bit_pack.h` 位段助手 / `zj_flit.h` / `xs_flit.h`）与 `model/ring/`（`ring_slot.h`），单测 `tests/test_flit.cpp`）。**参数化方式 = 编译期 config traits**（模板参数 `Cfg`，与 RTL elaboration-time Parameters 同级；默认 = kunminghu-v3 锁定值）。实测锁定值：niw=**11**（`ZhuJiangNoCTopology.scala:17` 覆盖 nodeNidBits=8，非 ZJParameters 默认 5）、raw=48、dw=256、CHI Issue=E.b（Makefile 钉死，ZhuJiang 只支持 E.b）；总宽经生成 RTL 端口核实：环 REQ 105/RSP 66/DAT 375/HRQ 128（`build/rtl/Router*.sv`），xscache seam REQ 118/RSP 66/DAT 367/SNP 102（`CoupledL2.sv` `io_decoupledCHI_*`）
- **xscache seam 是 CHI Bundle 的裁剪子集**（firtool 裁掉桥不读写的字段，剩余字段即边界契约）：CHIREQ 裁 returnNID/returnTxnID/ns/likelyshared/allowRetry/pCrdType/lpIDWithPadding/tagOp/traceTag（mpam 仅 partID 9b；mpam/rsvdc 被 mapReq 读但落入 zhujiang 零宽字段，无语义）；**rx_rsp 无 tgtID**（mapRsp 不回填恒 0，`ZhuJiangBridge.scala:187-199`）；CHIDAT 裁 ccID/tagOp/tag/tu/traceTag/rsvdc（dataCheck/poison 配置 require 关闭）；CHISNP 裁 ns(恒 false)/traceTag/mpam
- **Decoupled 通道 = 两个端口**：`Out<FlitTx>`（`{valid, bits}` 合体 struct）+ `In<Bool>` ready；fire = valid && ready。反压路径保持组合
- **环链路（valid-only 无 ready）**：`RingSlot {valid, flit, rsvdValid, rsvdPayload}`，每站每通道每方向一个 `Reg<RingSlot>`
- **复位**：全局同步复位一根 `Bool`，Update 用 `.on(posedge(clk))` + 复位优先级（先注册）；不建模 M 节点的两相复位时序

### 2.2 时序原语库（先行于一切业务模块）

全部对齐参考 RTL 的拍级语义，各自带 doctest 单元测试（逐拍驱动 + 显式期望序列比对 + 随机反压保序对拍）。按"语义来源"严格分两侧收录——**wolvicmod 侧只收 chisel3 标准库语义的通用元件，XiangShan 生态（xs-utils / dongjiang）特有元件收项目侧**。两侧共用同一通道约定 `Valid<T>{valid,bits}` + 独立 `xxx_rdy` 端口（`wolvicmod/include/wolvicmod/prefab/valid.h`）：

**wolvicmod 预制菜（chisel3 通用）**——`wolvicmod/include/wolvicmod/prefab/`，命名空间 `wolvicmod::prefab`，伞头 `prefab.h`，测试 `wolvicmod/tests/test_prefab_*.cpp`：

| 原语（头文件） | 对齐对象 | 要点 |
|---|---|---|
| `Valid<T>`（`prefab/valid.h`） | —（通道约定） | valid+bits 合体载荷 + 独立 rdy 端口，fire=valid&&rdy |
| `Queue<T,N,Flow,Pipe>`（`prefab/queue.h`） | `chisel3.util.Queue` | Mem 存储 + 模 N 指针 + maybe_full；flow 空直通（被消费拍 do_enq/do_deq 均 false，**无幻影拷贝**）；pipe 满时 deq_rdy 放行 enq、同址读写见旧值；带 count |
| `FixedArb<T,N>`（`prefab/arb.h`） | `chisel3 Arbiter` | in0 最高优先级，全组合 |
| `RRArb<T,N>`（`prefab/arb.h`） | `chisel3 RRArbiter` | last_grant Reg（初值 0，RegEnable 无复位按两态取 0）；两遍优先级；fire 时推进 |
| `ValidPipe<T,N>`（`prefab/pipe.h`） | `chisel3.util.Pipe` | valid 每级 RegNext、bits 每级 RegEnable（上级 valid 门控）；N 拍精确延迟。dongjiang `Shift`（目录 4 拍/DS 5 拍的位移标记）= 本元件的 valid 链，不单设 |

**项目侧原语（XiangShan 特有）**——`proj-xiangshan-l3/prefab/`，命名空间 `zj::prefab`，伞头 `prefab.h`，测试 `proj-xiangshan-l3/tests/test_prefab_*.cpp`（构建 `proj-xiangshan-l3/build/`）；复用 wolvicmod 侧的 `Valid`/`FixedArb`/`ValidPipe`：

| 原语（头文件） | 对齐对象 | 要点 |
|---|---|---|
| `FastQueue<T,N,NoX>`（`prefab/fastq.h`） | xs-utils `FastQueue`（`queue/FastQueue.scala:6-58`） | 移位队列压实不变式；enq.ready **寄存**（初值 true）：满 → deq 后下一拍才恢复（一拍气泡，与 pipe Queue 的本质差别）；deq.valid/bits 组合；带 count/freeNum |
| `VipArb<T,N>`（`prefab/xsarb.h`） | xs-utils `VipArbiter`（`arb/VipArbiter.scala`） | vip 请求时 vip 获胜否则最低索引；指针让位到"之上最低 valid"（无则绕回之下）——连续 fire 时正向轮转跳过无效路、反压下粘性不动 |
| `QosRRArb` / `QosFixedArb<T,N>`（`prefab/xsarb.h`） | dongjiang `FastArb`（`utils/FastArb.scala:11-63`） | 按 qos==0xf 拆 high/low（low=全部输入）各过一个子仲裁器；hasHigh 抢占。**FastArb 的 rr 子仲裁器是 VipArbiter 而非 chisel RRArbiter**——QosRRArb=VipArb 子组、QosFixedArb=FixedArb 子组；T 须带 .qos 字段，子仲裁器类型经 `QosArb<T,N,Sub>` 第三参替换 |
| `Alloc<T,N>`（`prefab/xsarb.h`） | dongjiang `Alloc`（`utils/Alloc.scala`） | 首个空闲项优先编码（组合），free_id 全忙归末位 N-1（chisel PriorityEncoder 空输入语义，对拍实证） |
| `SpSram` / `DpSram`（`prefab/sram.h`） | xs-utils `Single/DualPortSramTemplate` | Mem 存储体 + ValidPipe 延迟链；读延迟 = (kIsc==1 ? Latency : kIsc+Latency) + (OutputReg?1:0)——目录配置 (1,2,outReg)=3 拍（d3 出 resp，连同 d4 输出寄存共 4 拍）、DS 配置 (2,2,outReg)=5 拍、双口 repl (1,1,outReg)=2 拍；intvCnt 回压间隔 max(Latency,kIsc)；way 掩码写；单口读写互斥、双口 bypassWrite 同址写优先/否则对齐 Verilator SyncReadMem 下件读新值（Undefined RDW 角落，ZhuJiang 不依赖）；ShouldReset 复现横扫回压（数据由 Mem 零初始化覆盖） |

这些原语是"周期精确"信用所在：业务模块只组装原语，不直接写散落的 `Reg<std::deque>`。收录边界与已知语义取舍（RegEnable 初值取 0、Queue flow 无幻影拷贝、FastQueue 的 valids 合并为 count、FastArb 的 RR=VipArbiter、ShouldReset 只建回压时序）详见 `wolvicmod/README.md` §4。

建模约定补充：**同名解包约定**——Assign/Update lambda 的 `src` 解包绑定名与 `.reads(...)` 信号名一致、顺序一致（Mem 进读集同样同名；子模块端口按层次路径展开为下划线绑定名；lambda 内局部临时变量不得与信号同名）。框架不编译期检查此约定，靠评审维持，详见 `wolvicmod/README.md` §3.9。

---

## 3. 模块层次

```
WolvicZjTop                                   ← 集成边界（DPI-C 落点）
│   端口：rn(DecoupledPortIO, xscache flit) / memAXI / cfgAXI / clk / reset
│
├── XscChiAdapter                    §3.1    xscache↔zhujiang flit 字段重映射（纯组合）
├── CcSocket (PDC dev+icn 两侧)      §3.2    L2↔环之间的信用弹性缓冲
├── Ring                             §3.3    10 站 × 4 通道 × 2 方向
│   └── RouterStop[10]
│       ├── 仅 CC/RI：RnRouter（REQ 地址译码选 TgtID）
│       ├── 每通道：InjQueue(2) + ChannelTap（注入仲裁/弹出匹配/SrcID 盖章）
│       └── 每通道每方向：EjectBuffer（REQ 5 / RSP·DAT 3）+ 2:1 RR 合流
├── HomeWrapper[2]                   §3.4    bank0 / bank1
│   ├── ChiBuffer × 2 lan（每通道深 2 队列，1 级）
│   └── DongJiang (HNF)              §3.5    ★ 工作量核心
│       ├── ChiXbar（组合，addr[6] 分 dirBank，QoS-RR）
│       ├── Frontend[2]（dirBank0/1）
│       │   ├── FastQueue(2) → ReqToChiTask（组合）
│       │   ├── TaskBuffer（req16+hpr8，FREE/SEND/WAIT/SLEEP，同址 sort）
│       │   ├── Block（s0→s1 打拍，PoS 查冲突，retry/sleep）
│       │   └── PoS（4set×16way，wakeup 广播）
│       ├── Directory
│       │   └── DirectoryBase[llc,sf]×2（SramTemplate 4拍、lockTable、reservation、PLRU）
│       ├── Backend
│       │   ├── Commit（112 项 FSM：FREE/FSTTASK/SECTASK/COMMIT/CLEAN）
│       │   ├── ReadCM(64) / WriteCM(32) / SnoopCM(32) / ReplaceCM(64)
│       │   └── 各级译码 Pipe + 仲裁网络
│       └── DataBlock
│           ├── BeatStorage[4 bank×2 beat]（SramTemplate 5拍）
│           ├── DataBuffer（128×32B，mask/repl 位图，两条 2 拍读流水）
│           ├── DBIDCtrl（2×64 FastQueue）
│           └── DataCM（64 项 8 态 FSM，REPL>critical>QoS-RR）
├── SNodeAxiBridge                   §3.6    CHI-SN→AXI4，64 CM，→ memAXI
├── HiNodeAxiLiteBridge              §3.7    CHI-HNI→AXI4，8 CM，→ cfgAXI
└── stubs：RiNode（全 tie-off）/ M→全局复位 / P→1 拍线延迟
```

### 3.1 XscChiAdapter

- 职责：`ZhuJiangBridge.scala:152-252`（实际路径 `XSCache/src/test/scala/ZhuJiangBridge.scala`，package zhujiang）的 `mapReq/mapRsp/mapDat/mapSnp` 字段级重映射（xscache Bundle ↔ zhujiang Flit），双向各四条通道，**全组合**（RTL 中就是纯连线）
- wolvicmod 形态：无状态，四对 Assign；同时承载 DBID 12↔16 的宽度适配断言
- 附带断言：路由到 CC 的 REQ 不允许出现（RTL 桥里 `ready:=false.B`，`ZhuJiangBridge.scala:147`）

**as-built 备注（P2 实证补充）**：✅ 已实现于 `model/cc/xsc_chi_adapter.h`（自由函数
`mapReq/mapRspZj/mapRspXs/mapDatZj/mapDatXs/mapSnp` + 纯组合模块，无 clk 端口）。
- firtool 把桥逻辑内联成 XSTop 的端口连线（`XSTop.sv` socket 实例 `io_icn_*` ↔ `_core_with_l2_io_decoupledCHI_*`）——"纯组合"假设经生成 RTL 证实
- 端口裁剪即契约：`io_decoupledCHI_rx_rsp` 无 `tgtID`（`mapRspXs` 亦不回填，恒 0）；`io_icn_tx_req` 整个不存在（eject REQ 死端，模型侧 `l2_tx_req_rdy` 恒 false）
- `io_decoupledCHI_tx_req` 保留 `mpam_partID(9b)/rsvdc(4b)`（被 mapReq 读但落 zhujiang 零宽字段，无语义）

### 3.2 CcSocket（PDC）

对齐 `ZJ/device/socket/PowerDomainCrossing.scala:16-62`，每通道每方向：

- Tx 侧：5 token 计数 Reg + 1 级寄存；`ready = tokens.orR`
- Rx 侧：1 级寄存 + 5 项 flow Queue
- 行为结果：L2↔CC 路由器之间每通道 ≈2 拍固定延迟 + 双向各 5 项在途

**as-built 备注（P2 实证补充）**：✅ 已实现于 `model/cc/`（`pdc.h` 的 `PdcTx/PdcRx`
+ `cc_socket.h` 的 `CcSocket`，inject REQ/RSP/DAT × eject REQ/RSP/DAT/SNP 七通道）。
- CC socket 在生成 RTL 中 = `SocketDevSide`（XSTop 内实例 `socket`，`io_icn_*`）+ `SocketIcnSide`（`zhujiang_opt.ccn_0_0x8`，`io_dev_*`）背对背，PDC 线（`ccn_0x8_sync_*`）在 Top 层直连；kunminghu-v3 单时钟域，两侧同 clk
- 合成流量对拍：`verify/cosim/cc_socket_ref.sv` + `harness_socket.cpp`（`run.sh socket`）：3 seed × 20 万拍 × 1740 万比对/seed = **5220 万比对零失配**
- **真实流量 trace 重放**（`tests/test_trace_replay.cpp`，`make replay`）：coremark 前端 2 万拍，驱动 L2 CHI 缝六通道 + 环侧 eject 四通道/inject ready，`io_decoupledCHI_*`（`core_with_l2` 口）与 `ccn_0_0x8.io_dev_*` 双侧逐拍比对，**265,459 次比对零失配**。注意：bits 仅在 valid=1 时比对——difftest 开 `RANDOMIZE_REG_INIT`，valid=0 时 RTL bits 是随机垃圾（don't-care）
- grant→token 回补有 2 拍可见延迟（激励驱动后需隔拍观察）

### 3.3 Ring / RouterStop

✅ P1 已实现于 `proj-xiangshan-l3/model/ring/`（`hrq_flit.h` HRQ 车道超集类型 /
`eject_buffer.h` VipTable+EjectBuffer / `channel_tap.h` SingleChannelTap+ChannelTap+RingPipe /
`ring.h` STOP_TABLE+Ring 组装），环级对拍零失配。

**as-built 备注（对拍实证补充）**：
- HRQ 车道用超集 struct（HReqFlit⊕SnoopFlit+is_snp），四种车道都有 tgt/src/txn/qos 字段直访
- ResetRRArbiter 语义 = chisel RRArbiter（仅复位风格差异），eject 合流/HF 的 HRQ 注入合并均用 `RRArb`
- **firtool 端口裁剪即边界契约**（对拍 harness 的来源）：ZRING 顶层 RI/HI 的 icn 端口是子集——
  `rni rx_req` 无 TgtID/Excl、`rni rx_resp` 仅 Opcode/SrcID/TgtID/TxnID、`hni rx_resp` 仅
  DBID/Opcode/QoS/TgtID/TxnID、`hni` 无 rx_req（ERQ 注入口不存在）；`rni tx_resp`、
  `hni tx_resp`/`tx_data` 无 ready 且 bits 大子集化。**P5 的 DPI 薄壳端口清单须按裁剪后的
  实际端口对齐**（届时按 Top 层重新生成的端口表核对）
- 复位：M 节点 resetInject 链沿环传播约 20+ 拍，对拍时空跑 32 拍待稳定

对齐 `ZJ/xijiang/router/base/`（`BaseRouter.scala`、`ChannelTap.scala`、`EjectBuffer.scala`）：

- **链路**：每站每通道 `Reg<RingSlot>` 打 1 拍 ⇒ 每跳 1 拍；无 tap 的通道纯 Pipe 直透
- **注入仲裁**（组合 Assign + 状态 Reg）：`inject.ready = emptySlot && availableSlot`；环上流量绝对优先；防饿死：阻塞 8 拍（10 节点环 timerBits=4）→ s_inject_reserved 盖 rsvd 令牌 → 令牌绕环回来必得槽
- **弹出**：`tgt.router==本节点` 匹配；EjectBuffer 满则 flit 续绕环；末槽 VIP 保留（tag=Cat(src,txn,tgtAid[,DataID])）
- **SrcID 盖章**：注入时 `nid` 覆写为本节点、`aid` 保留（`BaseRouter.scala:208-210`）
- **方向选择**：注入时静态最短路（rightNodes/leftNodes 折半表，配置期生成常量）
- **RnRouter**（仅 CC/RI）：REQ 按地址选 TgtID——`!device && bank(addr[12]) 命中 → HNF`；`device && HI addrSet 命中 → HI`；否则 → defaultHni 0x20
- 站参数表驱动结构：`STOP_TABLE[10] = {nodeId, type, taps...}`，RouterStop 按表参数化生成 tap/缓冲（对应 `xijiang/Node.scala:137-169` 的 injects/ejects）

### 3.4 HomeWrapper

对齐 `ZJ/device/home/HomeWrapper.scala`：

- 每 lan：ChiBuffer（每通道深 2 队列 ×1 级）+ friends 方向选择（tx flit 按目标 NID 属于哪个 lan 的 friends 决定从 hfp0/hfp1 发出，并改写 HomeNID/ReturnNID）
- ERQ 口：按 S 节点 addrSets（全匹配）选 TgtID=S
- 内部 1 个 DongJiang，2 lan 经 ResetRRArbiter 汇入

**as-built 备注（P2 实证补充）**：✅ 外壳已实现于 `model/home/home_shell.h`
（`HomeShell<Cfg>`，Cfg 为 NTTP——createChildModule 只支持默认构造；每 lan 7 个
`Queue<F,2>` ChiBuffer + 3 个 `RRArb<F,2>` eject 合流；inject friends 组合分发：
ERQ 选址 `addr.ci==ci?0x30:0`、ReturnNID noDmt(0x7FF) 改写 srcId、DAT.HomeNID 改写），
P3 起 HNF 为 DongJiang 全量模型（`model/home/dongjiang.{h,cpp}`，P2 的行为桩已删）。
实测锁定值：bank0 = nids{0x00,0x38} friends{{0x08,0x18},{0x40,0x30}}、
bank1 = nids{0x10,0x28} friends{{0x18,0x08},{0x30,0x40}}、mem_nid=0x30；
hnxPipelineDepth=0 → 每 lan 仅 1 级 ChiBuffer。
组装见 `model/wolvic_zj_top.h`（adapter→cc_socket→ring n1；n0/n7→shell0、
n2/n5→shell1；n4→HI 桥、n6→S 桥；RI n3 tie-off 桩内收于 WolvicZjTop）。

> ⚠️ **P4b 时钟门控修正**：HomeWrapper 的 DoubleCounterClockGate 门控整个
> DongJiang 时钟域（`hnx.clock := cg.io.ock`），上电横扫（目录 SRAM/DBID 预充）
> 冻结至首个 REQ/HPR flit 到达 ChiBuffer 出口（inbound 组合唤醒零延迟），两 hnf
> 独立唤醒——coremark 实测 hnf_0 cyc~1038 醒、hnf_1 cyc~9323 醒，首笔 mem.ar 时刻
> 直接由唤醒拍决定。模型在 DongJiang 顶层建 `woken` 单向锁存
> （`= woken | hnx_rx_req.valid`），横扫/预充计数（SpSram/DpSram/DBIDPool）挂
> `clk_en` 功能使能；ICG 冻结前 resetHold 已移一位，横扫窗口首拍无条件推进。
> 稳态流量下门控与常开等价（working 维持 + inbound 当拍唤醒），不建睡/醒循环。

### 3.5 DongJiang（HNF 本体）

对齐 `ZJ/dongjiang/`，连接拓扑照 `DongJiang.scala:83-87` 组装。各子模块的状态机、队列深度、仲裁优先级全部照 RTL（关键数据见 §1 参数表与探查报告的仲裁表）。

建模顺序建议（自底向上，每层可独立对拍）：

1. **Directory**：DirectoryBase(llc/sf) 双读口捆绑；4 拍流水 d0~d4；lockTable / SF reservationTable / PLRU；写优先单口
2. **DataBlock**：BeatStorage 5 拍、DataBuffer、DBIDCtrl、DataCM 八态 FSM
3. **Backend**：Commit 112 项 + 四个 CM 池 + 译码 Pipe + 仲裁网络（后端是"任务执行器"，前端是"任务分发器"）
4. **Frontend**：FastQueue→ToChiTask→TaskBuffer→Block(s0/s1)→PoS；三条阻塞源（pos/dir/resp）与 retry/sleep 机制
5. **ChiXbar**：组合分发 + QoS-RR

译码表（`ReadDecodeTable/WriteDecodeTable/DatalessDecodeTable.scala`）逐表转写为 C++ switch/查表 lambda——这是行为黑盒部分，不参与结构。

### 3.6 SNodeAxiBridge

对齐 `ZJ/device/bridge/axi/AxiBridge.scala`：64 个 CtrlMachine（PickOneLow 分配）、CHI ReadNoSnp/WriteNoSnp → AXI AW/AR、写数据 AxiDataBuffer(64)、awQueue 保序、同地址(32KB 粒度)写读排序 wakeup、R→CompData（DataID=addr(5) 拼接）、出口 ConditionVipArbiter ×3。

> ✅ **P4a as-built**（`model/bridge/`）：共享 CM 骨架原为 `BridgeCm<Tr>` 子模块（对齐 `BaseCtrlMachine.scala`，两桥差异经 traits 转写 opvec/info/entry 类型参数），2026-09-29 拍平后降级为普通 C++ 载体 `CmSt<Tr>`（状态 POD）+ `CmLogic<Tr>`（纯静态函数组），两桥各持 `REG(std::array<CmSt, kOutst>)` 一条 update 循环（`bridge_cm.h`）+ `CmST` traits + `AxiDataBuffer`（`axi_data_buffer.h`）+ `SNodeAxiBridge`（`snode_axi_bridge.h`）。实测参数：outstanding=64（`ZhuJiangNoCTopology.scala` MemoryOutstanding）、AXI id 6b/data 256b/addr 48b、**compareTag=addr[37:6]**（64B 粒度、32b 字段——注意并非 32KB）。`AxiBufferChain`：S/HI 节点 `buffers=0`（AxiDeviceParams 默认）→ RTL 直通，边界即桥自身 axi 端口，不建模。`ConditionVipArbiter` 实现为项目 prefab `CondVipArb`（`prefab/xsarb.h`，SelNto1+selReg+VipArb，**出口仲裁有 1 拍注册延迟**）；freelist 的恒 ready MimoQueue 行为等价为 1 拍延迟寄存；64 CM 的 wakeup/info/alloc/W 广播经数组 wire 汇聚（拍平前为 `wolvicmod::combine`）。`working`/ZJPerf/MbistPipeline 不建模（时钟门控/性能/DFT 约定）；RTL 断言转注释。

### 3.7 HiNodeAxiLiteBridge

对齐 `ZJ/device/bridge/axilite/AxiLiteBridge.scala`：结构同 §3.6 简化版，8 CM。**不可省略**：coremark 的 UART 输出等全部 MMIO 都经 CC→defaultHni→cfg AXI 出仿真外设。

> ✅ **P4a as-built**：`CmHiT` traits + `HiNodeAxiLiteBridge`（`hinode_axilite_bridge.h`）。实测参数：outstanding=8（AxiDeviceParams 默认）、`busDataBits=cfgAxiDataBits=L3OuterBusWidth=256`（SoC.scala:148）、**tagOffset=3 → compareTag=addr[18:3]**、`nodeId=0x20` 常量化（生成 SV 中 nodeId 端口已被 firtool 常量折叠裁除）。W 直出（无 dataBuffer，CM 内 64b 数据/8b 掩码，`slvMask=MaskGen(addr,size,32)`——`info.mask` 在 RTL 中抽取但不被消费，模型同留作保真）；`icn.tx.req`（ERQ）恒 invalid。生成 SV 端口裁剪注记：HI 侧 `axi_b_ready`/`icn_rx_resp_ready`/`icn_rx_data_ready`/`nodeId` 等常量端口被 firtool 裁除，模型保留这些出口（对拍时不比）。
>
> ⚠️ **compAck 同拍覆盖陷阱**（桥级对拍实证）：`BaseCtrlMachine.scala:100-117` 中 `rx.resp`（CompAck）与 `rx.data` 两个 `when` 块都写 `compAck`，同拍同到时按 Chisel 后连接优先 = **rx.data 块覆盖 rx.resp 块**——ECA 写若 CompAck 与 NonCopyBackWriteData 同拍到达同一 CM，CompAck 被丢弃、CM 永久占住（5 万拍后触发 "bridge CM time out" 调试断言）。香山依赖 CHI 保序（CompAck 后于写数据）；模型按 RTL 原样复现该覆盖语义，激励侧须遵守保序。

---

## 4. 建模准则（什么必须镜像、什么可以简化）

### 4.1 简化判定准则

"周期精确"只约束**边界信号逐拍一致**（见本文开头"周期精确的定义"）。一个结构要素必须镜像 RTL 建模，当且仅当它影响以下三者之一：

1. **延迟**——信号最早/最晚能在第几拍变化（每级流水寄存器、SRAM 读延迟链）；
2. **占用/反压**——同时能容纳多少事务在途（每个队列深度、DBID/PoS/Commit/CM 池大小、EjectBuffer 深度、socket token 数）；
3. **竞争结果**——多请求同拍争一个资源时谁赢（每个仲裁点的类型与优先级、FSM 状态转移的先后次序）。

凡不落入这三条的均为纯行为，用 C++ lambda/switch 黑盒表达：flit 字段变换、译码表、地址计算、数据搬运。改动它们不改变任何边界时序。

### 4.2 可安全简化清单

| 简化项 | 理由 |
|---|---|
| 时钟门控（DoubleCounterClockGate） | 组合唤醒，时序等价于常开（§1 省略清单）。**订正**："时序等价于常开"仅稳态成立，**上电横扫期冻结必须建模**（P4b 实证，见 §3.4 与 §5.9） |
| M 节点两相复位 → 全局同步复位 | 只影响复位过程，不影响运行期逐拍行为 |
| RI 节点全 tie-off 桩 | XiangShan 侧 DMA 已 tie-off，数据通路不激活 |
| QoS 分级仲裁 → 普通仲裁 | 环内仲裁不使用 QoS，QosRRArb 退化为单层。**订正**：环内仲裁确实不使用 QoS，但 **QoS==0xf 请求经 ChiXbar 改道 HPR 优先通道不可省**——HprTaskBuffer 全通路已建模（见 §5.8/§5.9） |
| DatalessCM | RTL 本就未例化 |
| HPR/DBG 环、BBN、c2c、MBIST/DFT、ZJPerf | 配置关闭或纯观测逻辑，不进数据通路 |
| 译码表（Read/Write/Dataless DecodeTable） | 纯查表，转写为 switch/lambda |
| ChiXbar、XscChiAdapter、字段重映射 | 全组合连线，无状态 |
| SRAM 宏 → Mem + ValidPipe 延迟链 | 时序等价，已封装为 SpSram/DpSram 原语 |
| FSM 内部状态编码 | 编码方式任意，只要转移拍数与占用期间外部可见行为一致 |

**条件化简化**（可以做，但必须带断言兜底，触发即报错）：

- 仲裁器优先级合并——仅当能证明该仲裁点在所跑负载下永不超过 1 个请求同拍竞争；保留断言；
- 串联队列合并——总深度不变且无中间抽头时可合为一个，但须核对 flow/pipe 语义差别（FastQueue 的"一拍气泡"即此类坑）。

**绝不能简化**：每级打拍位置、每个队列深度、每个仲裁点、TaskBuffer/PoS/Commit/DataCM 状态机与超时机制、防死锁机制（环 rsvd 令牌、EjectBuffer VIP 末槽、TaskBuffer 超时锁定、目录 lockTable）——它们在 coremark 中未必触发，但少一个就可能在某次反压下死锁。

### 4.3 正确性关键路径

- **同地址三级串行**（TaskBuffer sort → PoS sleep/wakeup → 目录 lockTable）是正确性关键路径，对拍用例必须覆盖
- **DBID 位宽坑**（zhujiang 16b vs xscache 12b）在适配层显式断言，值域不超 12 位语义
- 时钟门控：稳态睡/醒省略（时序等价，§3.4 已论证）；**上电横扫冻结不可省**（P4b 实证——省略则首笔内存访问早 ~1000 拍，边界立即失配），已建模为 DongJiang `woken` + 横扫/预充 `clk_en` 功能使能；若未来对功耗建模再补睡/醒细节

---

## 5. DongJiang 语义参考

> 来源：XSCache/ZhuJiang `src/main/scala/dongjiang/**`，配置链取 kunminghu-v3 `DefaultConfig + LLC=ZhuJiang`（单核，即跑通 coremark 的配置）。
> 以下各节是对拍校正后的最终语义。

### 5.1 全局配置推导（定死，所有子模块共用）

#### 5.1.1 配置链

- `DefaultConfig` = `ZhuJiangConfig("32MB", ways=16)` + L2 2MB + ...（Configs.scala:581）→ `cacheSizeInB=32MB, cacheWays=16`。
- `Top.scala:350` → `ZhuJiangNoCTopology(1, ZJParameters(), 256)`：`nodeNidBits=8, nodeAidBits=3`，单核 10 节点：HF(bank0,hfp0)、CC、HF(bank1,hfp0)、RI、HI(defaultHni)、HF(bank1,hfp1)、S、HF(bank0,hfp1)、M、P。**无 RH → hasHPR=false；无 BBN（r2rPos 空）**。
- `ZJParameters` 默认：`requestAddrBits=48, hnxBankOff=12, ciIdBits=4, dataBits=256, clusterCacheSizeInB=2MB, snoopFilterWays=16, hnxOutstanding=256, hnxDirSRAMBank=2`。

#### 5.1.2 djParams 推导（ZJParameters.scala:246，djParamsOpt=None 分支）

bank = hfpId==0 的 HF 数 = **2**。一个 DongJiang 服务一个 bank（HomeWrapper 内，两 hfp 端口共用）：

```
DJParam{ addressBits=48, llcSizeInB=16MB(32M/2), sfSizeInB=2MB(2M*2*1/2),
  llcWays=16, sfWays=16, nrDirBank=2, nrDSBank=4, nrPoS=128(256/2),
  dataBufSizeInByte=4096(32*256/2), nrReqTaskBuf=32, nrHprTaskBuf=16, nrSnpTaskBuf=0,
  openDCT=true, dirRamSetup=1, dirRamLatency=2, dirRamExtraHold=false,
  dataRamSetup=2, dataRamLatency=2, dataRamExtraHold=false }
```

#### 5.1.3 HasDJParam 派生（每 DongJiang 实例）

| 量 | 值 | 备注 |
|---|---|---|
| bankBits / offsetBits / ciBits | 1 / 6 / 4 | bankId=addr[12]，ci=addr[47:44] |
| useAddrBits | 41 | useAddr = addr[47:13] ++ addr[11:6]（剔除 bank 位） |
| llc sets / setBits / tagBits | 8192 / 13 / 27 | 全局 16384；llcSet=useAddr[13:1] |
| sf sets / setBits / tagBits | 1024 / 10 / 30 | 全局 2048；sfSet=useAddr[10:1] |
| posWays / posSets / posSetBits | 16 / 4 / 2 | 全局 posSets=8；posSet=useAddr[2:1]；posTagBits=38 |
| nrPoS / nrCommit（每 bank） | 64 / 56 | 全局 128 / 112 |
| hnTxnID | 7 bit = {dirBank[6], posSet[5:4], posWay[3:0]} | dirBank=useAddr[0] |
| nrSfMetas / metaIdBits | 1 / 0 | 无 BBN，单 CC → sf metaVec 仅 1 项 |
| readDirLatency / dirMuticycle | 4 / 2 | dirRamSetup=1+lat=2+outputReg |
| readDsLatency | 5 | (setup2→3)+lat2 |
| nrDataBuf / dbIdBits | 128 / 7 | 4096B/32B；nrDataCM=64，dcIdBits=6 |
| CM 数（每 DongJiang） | Replace 64, Snoop 32, Read 64, Dataless 32, Write 32, Receive 32, Task 3 | |
| TaskBuf（每 dirBank） | req=16, hpr=8, snp=0 | hpr 因 hasHPR=false 闲置 |
| 超时（周期） | TASKBUF 120000, POS/LOCK/DATACM 80000, COMMIT 72000, REPLACE 60000, SNP/READ/WRITE 40000 | |

### 5.2 DongJiang 顶层结构（DongJiang.scala）

- 每 bank 一个 DongJiang：**2×Frontend**（per dirBank）+ 共享 **Backend / Directory / DataBlock / ChiXbar**。
- lan 口 rx.req/resp/data 经 `setRx` 打 `tgt=LAN`；nrIcn=1 → rxRsp/rxDat 的 Arbiter 退化为直通。
- 互连（方向：生产者→消费者）：
  - chiXbar.rxReq/Hpr/Snp → 各 frontend（rxSnp 仅 BBN，本配置无）
  - frontend.readDir → directory.readVec[i]；directory.rRespVec[i] → frontend.respDir
  - backend.writeDir/unlock → directory；directory.wResp → backend.respDir
  - frontend↔backend：fastResp（fastRRArb）、reqPosVec2、updPosTag、cleanPoS、getAddrVec（hnIdx 双向）、cmtTaskVec、posRespVec2、updPosNest(仅 BBN)
  - →dataBlock 三路仲裁：reqDB = fastArb(backend.reqDB, RR(frontend.reqDB_s3), RR(frontend.reqDB_s1))（**backend 优先级最高**）；cleanDB = validOnly(fastArb(RR(frontend.cleanDB), backend.cleanDB))；task = validOnly(fastArb(backend.dataTask, RR(frontend.fastData)))
  - dataBlock.resp → backend.dataResp；backend.updHnTxnID → dataBlock
  - backend.txReq/txSnp/txRsp、dataBlock.txDat → chiXbar → lan tx
  - 各 frontend.alrUsePoS 求和 → posBusy 分档（0.5/0.75/0.9）→ RegNext → chiXbar.cBusy
- flushCache.ack = DontCare（本配置不建模 flush）；working = frontend.working 的移位或。

### 5.3 Directory 语义（directory/Directory.scala + DirectoryBase.scala）

#### 5.3.1 组装

- `Directory` = nrDirBank(2) × (DirectoryBase("llc") + DirectoryBase("sf"))，同 bank 的 llc/sf **读联动**：`llc.read.valid = readVec.valid & sf.read.ready`（反向对称），`readVec.ready = llc.ready & sf.ready` → 同一拍进两边，读响应天然同拍。
- 写按 `Addr.dirBank` 分发；`write.ready = (llcWReady | !llc.valid) & (sfWReady | !sf.valid)`；llc/sf 的 valid 独立（可只写一边）。
- `rRespVec(i)` = Valid(DirMsg{llc{wayOH,hit,metaVec}, sf{…}})，valid = `llcResp.valid & !toRepl`。**不带 addr/hnTxnID**（前端靠自身流水对号）。
- `wResp.llc/sf` = Valid(DirEntry{addr,wayOH,hit,metaVec}+hnTxnID)，valid = `resp.valid & toRepl`，按 bank 序 priority 取第一个（同一 write 通道 fire 间隔 ≥2 拍 ⇒ 4 拍后 resp 不可能同拍，PopCount≤1 恒成立）。
- unlock 广播到全部 4 个 DirectoryBase，各自按 hnIdx.dirBank==本 bank 过滤。

#### 5.3.2 DirectoryBase 四拍流水

阶段记号 d0（fire 拍）→d4（第 4 拍）。`Shift{read,write,repl}` 各 4bit 右移寄存器：fire 从 bit3 进；d1=bit3 … d4=bit0。`reqSftReg`（addr+hnIdx+wriWayOH+metaVec）同步下移，仅在 `shiftReg.req.orR | 新fire` 时移位（**无请求时保持**）。

| 阶段 | 逻辑 |
|---|---|
| d0 | 端口仲裁 **repl_d0(d4 分配写回) > write > read**。meta/tag 单口 SRAM 发 req（setup=1+lat=2+outputReg ⇒ d3 出数）；repl 双口 SRAM 发 rreq（d2 出数）。`tagMetaReady = !(req 在 d1)` ⇒ **新请求至少隔 2 拍**；`replWillWrite = (repl&read).orR` 期间禁止任何新读写。`io.read.ready = resetDone & tagMetaReady & !replWillWrite & !io.write.valid`；`io.write.ready` 无末项。 |
| d1 | 无逻辑（等 SRAM）。 |
| d2 | `replMes`：d4 写回同 set 两级前递（d2 组合看 d4、d1 寄存在 d2 看 d4），否则取 repl SRAM rresp。`useWayVec = lockTable ∪ reservationTable(仅sf) ∪ pendingAlloc_d3` 中 set 匹配的 way 的 OH 并集。`replWay = PLRU.get_replace_way(replMes)`；`unuseWay = PriorityEncoder(~useWayVec)`。 |
| d3 | tag/meta 出数。`hitVec = tagHit & metaVal`；`invalidVec = !metaVal & !useWay(d2寄存)`；**selWay 优先级：hit > 有 invalid（取首个 invalid）> replWay 被占（取 unuseWay）> replWay**。`newReplMes = PLRU.get_next_state(replMesReg, wriUpdRepl_d3 ? wriWayOH : selWay)`。仅 sf：`pendingAlloc = read & !hit & hasInvalid`。组 resp：addr 由 {config.bankId, tag(selWay), reqSet, dirBank} 重组（victim 地址）、wayOH、hit、metaVec(selWay)、hnTxnID、toRepl=repl(d3)。lockTable/reservation 更新（§5.3.4）。 |
| d4 | `resp.valid = read(d4)`。repl 数组写回三选一：`wriUpdRepl_d4`（写类，触 PLRU，way=wriWayOH）/ `updTagMeta_d4`（分配写回：tag+meta SRAM 写选中 way + PLRU 更新，数据来自 reqSftReg 携带的写请求字段）/ `outDirResp & readHit`（读命中触 PLRU）。 |

#### 5.3.3 请求类型 → shift(read,write,repl) 与行为

| 请求 | d0 SRAM 行为 | (r,w,repl) | 后续 |
|---|---|---|---|
| 前端读 `read.fire` | meta/tag/repl 全读 | (1,0,0) | d4 resp 给前端，toRepl=0；命中则 d4 触 PLRU |
| 后端写命中（hit=1, directAlloc=0） | meta 写（wayOH 掩码，数据广播） | (0,1,0) | 无 resp；d4 wriUpdRepl 触 PLRU |
| 后端 directAlloc 写（仅 sf） | tag+meta 写 | (0,1,0) | 无 resp；清同 hnIdx reservation；d4 触 PLRU |
| 后端 wriNoHit（hit=0, directAlloc=0，替换申请） | meta/tag/repl 全读 | (1,0,1) | d3 选牺牲路；d4 resp(toRepl=1)→wResp（victim 地址/way/meta/hnTxnID）；**同拍 d4 updTagMeta 把 reqSftReg 携带的新 tag/meta 写入选中 way + PLRU 更新**（即 repl_d0，占端口 2 拍） |
| repl_d0 幻影项 | meta/tag 写（d4 写回的 SRAM 请求） | (0,1,1) | 仅占位阻塞，d3/d4 无效果、无 resp |

（recRead = req.fire & !req.write；wriNoHit 的 meta req.write=0 故记为 read；readRepl_d3 = read&!write&repl，wriRepl = !read&write&repl。）

#### 5.3.4 lockTable / reservationTable

- lockTable：`[posSets=4][lockWays]`，llc lockWays=15、sf=14。项 = {valid, set, way}。索引 = hnIdx.pos（hnIdx.dirBank 必须==本 bank）。
- d3 逐项目求 `reqHit = shiftReg.req(d3) & req.hnIdx==该项`：
  - readHit / readRepl（命中或替换选中）→ **无条件置锁 {reqSet, selWay}**（RTL 的
    `!oldLock & newLock` 来自事件类型编码 b01，与实际锁态无关；另有 HAssert 假定
    不重复置锁——对拍激励随机复用 hnIdx 时该路径必须按"覆盖"建模，已实证）；
  - readMiss → 断言未锁，不变；
  - write/wriRepl → 不变。
- `unlock.valid & unlock.hnIdx==该项` → 清锁（优先级高于置锁，elsewhen 结构实为 unLockHit 先行判断）。
- 作用：d2 `useWayVec` 把**所有 slot** 同 set 的锁定 way 都视为占用 → 他请求替换选路避让。
- reservationTable（仅 sf，同构 [4][14]）：d3 `pendingAlloc`（读 miss 且有 invalid way）且 hnIdx 匹配 → 置 {reqSet, selWay}；`directAlloc 写（同 hnIdx）或 unlock → 清`。llc 侧恒为 0（WireInit）。
- 更新条件：lockTable 在 `shiftReg.req(d3) | unlock.valid` 拍整体更新；reservationTable 在 `pendingAlloc | directAlloc | unlock` 拍更新。

#### 5.3.5 数据类型

- ChiState：llc 2bit {I=0, SC=1, UD=2, UC=3}（isValid = !=I）；sf 1bit（isValid=state[0]）。
- 读口：`Addr{addr48}` + hnIdx。写口：`DirEntry{addr48, wayOH16, hit, metaVec[1×ChiState]}` + hnIdx + directAlloc。
- resp：`DirEntry + hnTxnID7 + toRepl`；rRespVec 只保留 DirMsg{wayOH,hit,metaVec}×{llc,sf}。

#### 5.3.6 PLRU（rocket-chip PseudoLRU，16 路 15 bit）

- 树形位布局（16 路）：state[14]=root（1 ⇒ 左子树 way7-0 更老），左子树状态 = state[13:7]，右子树 = state[6:0]，递归；叶节点 1bit = 右孩子(奇数 way)更老。
- `get_replace_way`：从根向叶，每级取"更老"方向（bit=1 走左），路径即 way 编码（MSB 先行）。
- `get_next_state(state, touchWay)`：沿 touch 路径把各级 bit 指向"另一子树更老"，未触子树递归不变。
- 模型用同构 C++ 递归（对 16 路完全展开），与 PLRUTest 的 2/3/4/5/6 路断言可互验。

#### 5.3.7 SRAM 与复位

- meta/tag：单口、way=16、waymask=wayOH、写数据全 way 广播。repl：双口、way=1、bypassWrite。
- `shouldReset`（meta、repl；tag 不复位，靠 meta valid=0 屏蔽）：SramResetGen 上电扫描清零，resetDelay=4 拍 + set×interval(=setup=1) 拍 ⇒ llc meta ≈ 8196 拍、repl 相同（并行）。期间 req.ready=0。**横扫写与正常写共享 intvCnt 重装路径**（`when(ramRen||ramWen) intvCnt := interval-1`）：末笔横扫写后 ready 还要再延迟 interval-1 拍（llc meta=8197 拍才 ready；dir 对拍实证，prefab SpSram/DpSram 已修）。
- `resetDoneReg = RegEnable(true, metaReq.ready & replR.ready & replW.ready)` 门控 io.read/write.ready（llc DirectoryBase read_rdy 首真于 8198 拍）。

#### 5.3.8 对拍要点（harness 激励合法性）

- 写 directAlloc 须 hit=0 且 metaVec 有效（RTL 断言）；sf directAlloc 须命中 reservation（owner 匹配）。
- read 与 write 同拍只能活一个（read.ready 含 !write.valid；harness 侧 write 优先）。
- 响应间隔 ≥2 拍；wResp 每拍 ≤1。
- unlock 的 hnIdx 必须指向已锁项（PopCount==1 断言），且 way < lockWays。

### 5.4 DataBlock 语义（data/{DataBlock,BeatStorage,DataBuffer,DBIDCtrl,DataCM}.scala）

#### 5.4.1 组装（DataBlock.scala）

- `beatStorage` = nrDSBank(4) × nrBeat(2) 个 BeatStorage（每实例一条 HomeDatRam：
  SpSram 256bit × 65536 组，setup=2+lat=2+outputReg ⇒ **5 拍出数**）。
- `DataCM`（64 entry 控制 FSM）+ `DBIDCtrl`（dbid 分配池）+ `DataBuffer`（写数据缓冲）。
- 端口：txDat(Decoupled DataFlit 出)、rxDat(入)、updHnTxnID、reqDB(HnTxnID+dataVec 入)、
  task(Valid DataTask 入)、resp(Valid HnTxnID 出)、cleanDB(HnTxnID+dataVec 入)。
- txDat 二选一：`dbToCHI`(buf.toCHI.valid) 优先，`dsToCHI`(dsResp.valid & toCHI) 次之；
  bits 取 `dataCM.getChiDat.bits`（entry 寄存的 DataFlit），DataID/Data/BE 按源替换
  （dsResp 侧 BE=全 1）。`rxDat.ready = buf.fromCHI.ready = !dsResp.valid`。
- DS 读写口按 (bank, beatNum) 交叉分发；dsResp 经两级 fastArb（先 8 合 1 后 Pipe）入 buf。

#### 5.4.2 BeatStorage（5 拍流水）

- shift{read,write} 5bit；`reqReady = !req(4)`（请求隔 2 拍，对齐 SRAM interval=2）；
  `write.ready = rstDone & reqReady`，`read.ready = … & !write.valid`（写优先）。
- 无 shouldReset（数据阵列不复位）→ rstDoneReg 第 1 拍即锁存。
- resp：shift.outResp（d0+5）+ respPipe(5) 携带 {dcid, dbid, beatNum, toCHI}。

#### 5.4.3 DBIDPool / DBIDCtrl

- Pool = 2 × FastQueue(64)（dbid 7bit，偶/奇分queue）；上电 64 拍逐拍预充
  （q0←Cat(0,i)、q1←Cat(1,i)），rstDone 后 deq.valid 放行。
- enq（release 回池）：1 个 → 短queue（count<=）；2 个 → 一边一个。
- deq（alloc）：hasTwo = 两 queue 均非空 ⇒ `req.ready`；req.bits(i)=1 才弹对应输出；
  只弹 1 个时从**长**queue取（count>=），2 个时一边一个。resp(i) 组合直连。

#### 5.4.4 DataBuffer（datBuf：DpSram 8bit × 128 组(dbid) × 32 字节lane，1 拍读）

- `maskRegVec[dbid]` = 已收字节掩码：clean→0；dsResp.fire→全 1；
  fromCHI.fire→（CompData/SnpRespData(/Fwded)→全 1，其余(NCBWr 系)→ m|BE）。
- `replRegVec[dbid]`：readToDS.fire & repl 置位，clean 清除。
- 写口（valid+1 拍提交）：dsResp 优先（fromCHI.ready=!dsResp.valid）；
  mask = dsWriReg ? (replReg ? 全1 : ~mask) : (CompData/SnpResp 系 ? ~mask : BE)。
- 读口：rreq = RegNext(readToCHI.fire | readToDS.fire)（DS 优先选 dbid），
  rresp 再 +1 拍 ⇒ 读数据 fire+2 拍与 toCHIQ/toDSQ enq 对齐（2bit 移位 rToXSftReg(0)）。
- readToCHI.ready = hasFreetoCHI & !readToDS.valid（DS 优先）；hasFree =
  对应 Q(depth2).freeNum > 在途读数。
- toCHIQ enq：Data=rresp、BE=maskRegVec(dbid)、DataID=Cat(beatNum,0)；
  toDSQ enq：beat=rresp + {dcid, ds, beatNum} 寄存两拍链。

#### 5.4.5 DataCtrlEntry FSM（8 态，entry 数 = 64 = nrDataCM）

状态：FREE→(alloc.fire)→ALLOC→(taskHit→REPL/READ/SEND/SAVE 按 dataOp 优先级 repl>read>send>save；
cleanHit→CLEAN)；REPL→(readAll&saveAll)→sendAll?RESP:SEND；READ→readAll→(send→SEND / save→SAVE / RESP)；
SEND→sendAll→(save→SAVE / RESP)；SAVE→saveAll→RESP；RESP→(resp.fire)→ALLOC；
CLEAN→(release.fire)→isZero?FREE:ALLOC。
- **entry 生命周期**：一次 alloc 可服务同 hnTxnID 的多个 task（RESP→ALLOC 循环），
  dbid 由 clean 分段释放（release.dataVec = reg.dataVec & task.dataVec）。
- 三通道计数（以 read 为例，send/save 同构）：sReadVec=待发 beat 位图、wReadVec=待回写完成位图；
  taskHit 且 dataOp 对应位置位 → 两图同装 task.dataVec；发射(sFire)清 s 位、完成(wFire:
  dsWriDB/txDatFire/dbWriDS 按 dcid+beatNum 匹配)清 w 位；isXAll = w 图全空。
- readToDB.valid = isRepl|isRead（REPL 须 sReadVec 与 sSaveVec 非空且同步）；
  readToDS.valid = isRepl|isSave；readToCHI.valid = isSend。
- critical：alloc/taskHit 清；发射时 PopCount(剩) > 1 保持。
- updHnTxnID：匹配即改 task.hnTxnID；task/clean/upd 均按 hnTxnID 全等匹配。

#### 5.4.6 DataCM 仲裁

- reqDBIn.ready = reqDBOut.ready & hasFreeDC；alloc 到 freeDCID（首个 FREE entry），
  dbidVec 同拍取自 DBIDCtrl.resp。**task 延迟 1 拍**（taskReg/taskFireReg）广播全 entry。
- 读仲裁：repl 优先——某 entry isRepl 时其 readToDB/readToDS **捆绑同发**
  （out.valid 互接对方 ready，两边同 ready 才 fire）；多个 repl 取 critical 优先再按 dcid 序。
  非 repl 走 connectReadToX：critical（valid&critical）唯一者优先，否则 fastQosRRArb
  （QoS==0xf 高优先层 + VipArbiter RR；resp/release 用 fastRRArb.validOut（RR，valid-only））。
- getDBID：TxnID 匹配唯一 entry → dbidVec(entry)(DataID==2?1:0)。
- 工具件语义（FastArb.scala）：fastArb=chisel 固定优先 Arbiter；fastRRArb=VipArbiter；
  fastQosRRArb=qos 0xf 高优层套 VipArbiter。均有 prefab（wolvicmod VipArb / proj FastQueue 已验证）。

#### 5.4.7 对拍要点（harness 激励合法性）

- task 须在 alloc 之后（≥1 拍，entry 仍 ALLOC）且 hnTxnID 匹配唯一 entry；
  resp(Valid) 无反压；clean 仅在 ALLOC 态（resp 后/未 task 前）；updHnTxnID 同。
- rxDat 的 TxnID+DataID 必须命中在途 entry 且对应 beat 未收（getDBID PopCount==1）；
  无协议联锁保证 readToDS 晚于数据到达——激励应尽快送 rxDat 以覆盖真实 merge。
- 释放闭环：resp 后须在有限拍内 clean（否则 dbid/entry 泄漏，超时断言 80000 拍）。
- 预充窗口：DBIDPool 64 拍内 reqDB.rdy=0（对齐比对）；BeatStorage 无横扫。
- dataVec 非零；repl 通道 readToDB/readToDS 捆绑同拍 fire。
- **同拍同类动作只发一路**（task/upd/clean 都是单拍 Valid 脉冲）：harness 曾因同拍
  两个 clean 互相覆盖导致 entry 永远等不到 clean → 僵尸 entry → 同 hnTxnID 双 entry
  （RTL dbgVec 断言域），排障路径：entry dump 实证双 entry 后再回溯调度器。
  getDBID 在多匹配时按 PriorityEncoder 取**首个** dcid（模型初版取末位，违例域
  才暴露，顺手修为忠实）。

### 5.5 Backend 语义（backend/{Backend,Commit,ReplaceCM,SnoopCM,ReadCM,WriteCM,Decode,Bundle}.scala）

#### 5.5.1 组装（Backend.scala）

- 五个部件：`Commit`（112 entry）+ `ReplaceCM`(64) + `SnoopCM`(32) + `WriteCM`(32) + `ReadCM`(64)。
  **DatalessCM 本配置不例化**（nrDatalessCM 未用）。
- txReq = FastQueue(fastQosRRArb(readCM.txReq, writeCM.txReq))；txSnp = snoopCM 直连；
  txRsp = fastQosRRArb(commit.txRsp, FastQueue(io.fastResp))（无 BBN 两路）。rxRsp.ready 恒 1。
- cleanPoS 合流（commit/repl fastQosRRArb → FastQueue）**同时三分叉**：io.cleanPoS（出）、
  io.unlock（hnIdx 原样）、io.cleanDB（hnIdx→hnTxnID，dataVec=Full）——cleanPoS.ready 由
  io.cleanDB.ready 反压。
- writeDir：replCM.writeDir → Queue(1, pipe) → io.writeDir；writeDirDone = io.writeDir.fire
  且 sf.valid & directAlloc（回 replCM）。wDirQ.enq 的 llc/sf addr 被 getAddrVec(2).result 改写。
- reqDB = fastArb(replCM, commit)（**repl 固定优先**）；dataTask = fastQosRRArb(
  FastQueue(commit), FastQueue(repl), FastQueue(writeCM))。
- cmResp 二路 Pipe(fastQosRRArb.validOut(snoop, read, write))：toRepl=0→commit、=1→repl。
- alloc 三路：snoopCM ← fastQosRRArb(FQ(commit.cmTaskVec(SNP)), FQ(repl.cmTaskVec(SNP)))；
  writeCM ← 同构(WRI)；readCM ← fastQosRRArb(FQ(commit.cmTaskVec(READ)))。
- getAddrVec(0) ← txReq.TxnID 的 hnIdx；getAddrVec(1) ← txSnp.TxnID；getAddrVec(2) ←
  replCM.writeDir.llc.addr。txReq.bits.Addr 按 MemAttr/Size 对齐改写（!cacheable→原址；
  size=6→64B 对齐；否则→32B 对齐）；txSnp.bits.Addr = (addr>>6)<<3。
- commit.cmtTaskVec 每路先 Pipe(1)。

#### 5.5.2 CommitEntry（五态 FSM，entry=112=2 银行 ×(posWays-2=14)×posSets 4）

状态：FREE →(alloc 且 task.isValid)→ FSTTASK /（否则）→ COMMIT；
FSTTASK →(decListIn，taskCode.isValid & cmt.waitSecDone)→ SECTASK /（否则）→ COMMIT；
SECTASK →(decListIn)→ COMMIT；COMMIT →(allFlagDone)→ CLEAN →(cleanPoS.fire)→ FREE。

- **flag 双层**：intl.s（待发 decode/reqDB/cmTask/dataTask/wriDir）、intl.w（待回
  cmResp/replResp/dataResp）、chi.s（待发 dbid/resp 两路 txRsp）、
  chi.w（待收 xCBWrData0/1、compAck）。allocHit 或 decListIn.valid 时整体重算；
  否则各 fire/hit 逐位清零。allFlagDone = 两层全 0。
- rxRsp/rxDat 监听（按 TxnID 匹配）：compAck（Rsp.CompAck|Dat.NCBWrDataCompAck）、
  XCBWrData0/1（NCBWr/CBWr 按 DataID 00/10）；alrGetReg 累积；respErrReg 取首个错误
  （cmResp.isERR 优先于 rxDat.RespErr）。
- 输出：reqDB（dataVec：snoop 任务→Full 否则 chi.dataVec）、dataTask（仅 alr.reqDB 后置），
  replTask（flag wriDir 且 !w.dataResp），cmTaskVec（SNP/READ/WRI 三选一 valid，
  bits 由 taskReg/taskInst 重组），txRsp（dbid 或 resp：opcode = resp?cmt.opcode:
  (isCopyBackWrite?CompDBIDResp:DBIDResp)），trd/fthDecOut（decValid = s.decode &
  !(w.cmResp|w.xCBWrData0|w.xCBWrData1)，按 FST/SEC 分流），cleanPoS（CLEAN 态）。
- instReg（TaskInst）：FST 段 OR 累积 cmResp.taskInst，SEC 段覆盖；xCBResp 在
  FST/FREE 段锁存 rxDat.Resp。decListIn 到达时 taskNext.{decList,task,cmt} 整体换入
  （FST 且 waitSecDone → cmt 清零等 SEC 结果）。

#### 5.5.3 译码 Pipe（backend/Decode.scala + frontend/decode/Bundle.scala）

- `Commit` 内 `trdDec`(Third)、`fthDec`(Fourth) 各一：entries 的 trd/fthDecOut 经
  fastRRArb.validOut 各合一路 → Decode 模块；2 拍延迟（RegNext+RegEnable 两级）后
  hnTxnIdOut/decListOut/taskCodeOut/cmtCodeOut 按 hnTxnID 匹配回灌各 entry 的 decListIn。
- Decode 内部：`thirdDec` 用当前 instReg（TaskInst）在四级表里查 decList(2)（
  `fourthDec` 查 (3)）；`GetDecRes` 按 decList 索引查 taskCode/secTaskCode/commitCode。
- **四级内容寻址表**（frontend/decode/Bundle.scala Decode 对象）：
  `table = Read_LAN_DCT_DMT.table ++ Dataless_LAN.table ++ Write_LAN.table`
  （34 chi 项 × ≤7 state 项 × ≤8 task 项 × ≤1 sec 项，1904 表项）。
  decode = 对表项按 UInt 全等 PriorityEncoder（断言唯一）；查码 = 四重 Vec 索引。
- 指令/码位宽（Chisel Bundle 字段 MSB 先排）：
  - ChiInst(18b)：valid, channel(2), fromLAN, toLAN, opcode(7), expCompAck, allocate, ewa, order(2), fullSize
  - StateInst(5b)：valid, srcHit, othHit, llcState(2)
  - TaskInst(19b)：valid, fwdValid, channel(2), opcode(5=max(5,4)), resp(3), fwdResp(3), getXCBResp, xCBResp(3)
  - TaskCode(24b)：ops(4: snoop/read/dataless/write), dataOp(5), opcode(7), needDB, returnDBID, expCompAck, doDMT, retToSrc, snpTgt(2), fullSize
  - CommitCode(28b)：wri*(3+2+2=7 含 srcValid/snpValid/llcState), dataOp(5), waitSecDone, sendResp, sendfwdResp, channel(2), opcode(5), resp(3), fwdResp(3), fullSize
  - 注：字段序 = trait 后挂先生效（末位 trait 字段在 MSB）——模型按生成 RTL 端口/常量实证后定稿。
- DecodeCHI 编码：I=0,SC=1,UC=2,UD=3,I_PD=4,SC_PD=5,UC_PD=6(=UD_PD),SD_PD=7；
  toResp(UD→SD 其余直通)；toState(低 2 位 I/SC/UC/UD)。
- 表内容（三张，逐字翻译见模型 dj_decode_table）：
  - Read_LAN_DCT_DMT（15 chi 项）：readNoSnp×3、readOnce×8、readNotSharedDirty、readUnique、stashOnceShared。
  - Dataless_LAN（5）：makeUnique、evict、cleanShared、cleanInvalid、makeInvalid。
  - Write_LAN（14）：writeNoSnpPtl×3、writeUniquePtl×8、writeEvictOrEvict、writeBackFull×2、writeCleanFull。

#### 5.5.4 ReplaceEntry（十八态 FSM，entry=64）

FREE →(alloc)→ REQPOS（isReplDIR）/ WRIDIR（否则）；
REQPOS →(reqPoS.fire)→ WAITPOS →(posRespHit→WRIDIR，否则回 REQPOS 重试)；
WRIDIR →(writeDir.fire)→ WAITDIR(isReplDIR) / WAITWRIDIR(isDirectAllocSF) / RESPCMT；
WAITWRIDIR →(writeDirDoneHit)→ RESPCMT；
WAITDIR →(dirRespHit)→ sfRespHit:RESPCMT / needReplLLC:(localClean?SAVEDATA:UPDATEID) / !need:SAVEDATA；
UPDATEID →(updHnTxnID.fire)→ WRITE →(cmTask WRI.fire)→ WAITRWRI →(cmRespHit)→ alrReplSF?CLEANPOST:RESPCMT；
RESPCMT →(resp.fire)→ isReplSF:(needSnp?REQDB:CLEANPOSR) / isReplLLC:CLEANPOSR / 否则 FREE；
REQDB →(reqDB.fire)→ SNOOP →(cmTask SNP.fire)→ WAITRSNP →(cmRespHit)→ cmRespData?COPYID:CLEANPOSR；
COPYID →(1 拍)→ REQPOS（hnTxnID←repl.hnTxnID，换槽重迭代）；
SAVEDATA →(dataTask.fire)→ WAITRESP →(dataRespHit)→ alrReplSF?CLEANPOST:RESPCMT；
CLEANPOST/CLEANPOSR →(cleanPoS.fire)→ CLEANPOSR/FREE。
- 关键副作用：llcRespHit 记 repl.toLan/ds（ds.set(addr, way)）；sfRespHit 记
  needSnp/alrReplSF 并刷新 dir.sf；cmRespData 到达时把任务改写为 wriLLC（hit=0，
  meta=UD/SC 按 passDirty）——snooze 数据回流更新 LLC。
- ReplacementWritePolicy：issueWrite = !toLan || dirty；saveLocalCleanVictim = toLan && !dirty。
- reqPoS 矩阵：每 (dirBank × posSet) 一个 VipArbiter(nrReplaceCM)，entry 的
  reqPoS.ready = 各矩阵 ready 按自身 (bank,set) 命中 OR。

#### 5.5.5 SnoopEntry（五态，entry=32）

FREE →(alloc)→ PRESNP（SnpUniqueFwd 且 snpVec>1）/ SENDSNP；
PRESNP →(倒数第 2 个 txSnp.fire)→ SENDSNP（PRESNP 段发 SnpMakeInvalid）；
SENDSNP →(alrSnpAll)→ WAITRESP →(alrGetAll=响应齐&数据齐)→ RESPCMT →(resp.fire)→ FREE。
- txSnp：PRESNP 段 opcode=SnpMakeInvalid，否则 task.chi.opcode；
  RetToSrc = 最后一个 & task.retToSrc；TgtID=各 meta 对应节点（本配置恒 0x09）。
- 响应合并：rspHit/datHit 时 taskInst 按优先级合并（fwdValid OR、channel DAT>RSP、
  opcode/resp 依 fwd/channel 保持或覆盖、resp 不劣化）；respErr 取首个非 OK。
- nrSfMetas=1：snpVec 恒单位，PRESNP 路径实际只用于 SnpUniqueFwd 单节点（PopCount=1
  不满足 >1，故直接 SENDSNP）。

#### 5.5.6 ReadEntry（八态，entry=64）

FREE →(alloc)→ SENDREQ（本配置无 BBN，CANNEST/CANTNEST/SENDACK 不到达）；
SENDREQ →(txReq.fire)→ doDMT?RESPCMT:WAITDATA0；
WAITDATA0 →(recDataHit)→ isHalfSize?RESPCMT:WAITDATA1 →(recDataHit)→ RESPCMT →(resp.fire)→ FREE。
- txReq：ExpCompAck/MemAttr/Size/Opcode 透传，Order=None，ReturnTxnID/NID 按 doDMT
  （本配置 openDCT 的 DCT 回传字段，LAN 侧 doDMT 仍可出现）。
- recDataHit：opcode=CompData 且 TxnID 匹配；记 nodeId←HomeNID、txnID←DBID、
  taskInst.resp←Resp、respErr。
- resp：!doDMT → taskInst{valid, channel=DAT, opcode=CompData, resp=reg.resp, fwdResp=I}；
  doDMT → 仅 valid。

#### 5.5.7 WriteEntry（八态，entry=32）

FREE →(alloc)→ SENDREQ →(txReq.fire)→ WAITDBID →(dbidHit)→ DATATASK →(dataTask.fire)→
WAITDATA →(dataRespHit)→ RESPCMT →(resp.fire)→ FREE（isRespCmt = state==RESPCMT & alrGetComp）。
- dbidHit = Rsp.CompDBIDResp|DBIDResp：txnID←DBID、nodeId←SrcID、alrGetComp |= (op==CompDBIDResp)、
  respErr；compHit = CompDBIDResp|Comp：alrGetComp=1、respErr。
- dataTask：dataOp 透传、txDat.Resp=cbResp、Opcode=isImmediateWrite?NonCopyBackWriteData:CopyBackWriteData。

#### 5.5.8 共享件

- `Alloc`（dongjiang/utils/Alloc.scala）：CM 池分配（Snoop/Read/Write 用；prefab 已对拍）。
- fastQosRRArb/fastRRArb/fastArb/VipArbiter/FastQueue：同 §5.4.6，prefab 齐备。
- Alloc 语义已由 P0 `alloc` 对拍覆盖（AllocRef_n4/n16）。

### 5.6 Backend 对拍补充（harness 环境模型与框架纪律）

- harness（verify/cosim/harness_backend.cpp）= HN 环境全模拟：请求生成器按
  frontend/decode 表构造 CommitTask（ci/state 随机，task=getTaskCode，task 无效时
  cmt=getCommitCode(0,0)）；PoS way 池（commit 0-13/repl 14-15）按 cleanPoS 释放；
  CHI 响应器（read→CompData、write→DBIDResp(/CompDBIDResp)+Comp+RN 写数据、
  snoop 按 opcode 取表内合法响应变体、CompAck 计划）；目录 wResp（替换写随机
  victim/meta 分布）；dataResp/posResp；getAddrVec 地址表（updPosTag 跟写）。
- 框架纪律两处新踩点：① 子模块 Out 端口不能由父模块驱动（Alloc 池的
  entry alloc_rdy 须 combine 成数组后驱动 Alloc.out_rdy）；② 父模块读集不含
  子模块内部 Reg——ReplaceCM 的 reqPoS 矩阵命中测试改用新增的
  `hn_txn_id_out` 端口投影（combine 后使用）。
- 验收：`run.sh backend` 3 seed × 15 万拍 = **1445 万比对零失配**。Backend 不设
  独立单测，行为验证全部走该对拍。

### 5.7 Frontend 语义（frontend/{Frontend,ToChiTask,TaskBuffer,Block,PoS,Decode}.scala）

#### 5.7.1 组装（Frontend.scala，每 dirBank 一份）

- 主链路：rxReq → FastQueue(2) → ReqToChiTask → reqTaskBuf（16 项，sort）→
  （与 hprTaskBuf(8) 仲裁：selectReq = !hpr.chiTask_s0.valid & !hpr.lockTask；
  **本配置 hasHPR=false，HPR 恒空转，selectReq 恒真**）→ Block s0/s1 →
  Pipe(readDirLatency-1=3) → Decode s2/s3 → cmtTask_s3。
- Block 同步拍发 posAlloc_s0（addr+channel）→ PosTable（返回 block_s1/hnIdx_s1/
  sleep_s1/wakeup）；wakeup 广播回两个 TaskBuffer。
- 出口：readDir(→Directory)、respDir(←Directory rRespVec)、reqDB_s1(Block 快回)、
  reqDB_s3(Decode 快路)、fastData_s3、cleanDB_s3、fastResp_s1(FastQueue 出)、
  cmtTask(→Backend，Decode 出口不再有 Pipe)、getAddrVec(←PosTable 槽地址)、
  reqPosVec/posRespVec/updPosTag/cleanPoS(↔ReplaceCM/Backend)、alrUsePoS、working。
- io.cleanDB.ready 有 HAssert 必须恒 1（DongJiang 顶层 fastArb 保证）。

#### 5.7.2 ReqToChiTask（纯组合）

- 字段直搬：addr/qos/nodeId=SrcID/channel=REQ/opcode/txnID/order/snpAttr/snoopMe/
  memAttr/expCompAck/size；toLAN = addr.ci==config.ci；fromLAN = flit.tgt==LAN（恒 1）。
- dataVec(1) = size==6 | addr(5)；dataVec(0) = size==6 | !addr(5)。

#### 5.7.3 TaskEntry / TaskBuffer（TaskState one-hot：FREE/SEND/WAIT/SLEEP）

- FSM：FREE→(in.fire)→SEND→(s0.fire)→WAIT→(wakeup 命中→SEND / sleep_s1→SLEEP /
  retry_s1→SEND / 否则→FREE)；SLEEP→(wakeup)→SEND。
- wakeup 按 useAddr 全等匹配（每个 PosEntry 的 wakeup 经 PosTable Mux1H 广播）。
- sort（req/hpr buf 均开）：nidReg 记录"同 useAddr 在途任务数"（入队时 init），
  同址任务 release 时全员 -1（othRel）；s0.valid = isSend & nid==0（保序：同址
  按入队序出站）。release = RegNext(isValid) & isFree。
- 超时：isWait & retry_s1 每拍计数，到 8 拍 lock=1；TaskBuffer 有 lock 项时
  **锁定该项优先出站**（lockVec PriorityEncoder + hasLockReg 锁住仲裁），否则
  fastRRArb 轮转。io.lockTask = hasLockReg。
- 池化：Alloc 分配入队（首个 FREE 项）。

#### 5.7.4 Block（s0 寄存 1 拍为 s1；三条阻塞源）

- validReg_s1/taskReg_s1 ← chiTask_s0（valid 每拍采样；bits en=valid）。
- 阻塞：pos = posBlock_s1；dir = cacheable & !readDir.ready；
  resp = blockByDB | (shouldResp & !fastResp.ready)，其中
  sReceipt = isRead & (isEO|isRO)（拍 s0 的字段）、sDBID = isWrite & !isCopyBackWrite、
  shouldResp = sReceipt | (sDBID & reqDB.ready)、blockByDB = sDBID & !reqDB.ready。
- retry_s1 = validReg_s1 & any（回 TaskBuffer 的 WAIT→SEND 与 PosSet 的 s1 保持）。
- task_s1 = s0 + hnIdx_s1 + alr{reqDB: reqDB_s1.fire, sData: false,
  sDBID: fastResp_s1.fire & op==DBIDResp}（valid = validReg & !any）。
- readDir_s1：valid = validReg & cacheable & !(pos|resp)；addr/hnIdx 同 task。
- reqDB_s1：valid = validReg & sDBID & fastResp.ready & !(pos|dir)，dataVec=Full。
- fastResp_s1：valid = validReg & shouldResp & !(pos|dir)；Opcode =
  sReceipt?ReadReceipt:DBIDResp；DBID=hnTxnID；TgtID=nodeId；SrcID=getNoC；TxnID/QoS 透传。
- 时序意义：s1 有效后若阻塞则**整个 s1 内容保持**（taskReg 只在 chiTask_s0.valid
  时更新——上游 arbitration 每拍都会给 valid，所以等价于每拍重写；阻塞期间上游
  selectReq/TaskBuffer 保持同一任务）。

#### 5.7.5 PosEntry / PosSet / PosTable

- PosEntry 状态 {req, snp, tagVal, tag(posTagBits=38), offset(6)}：
  alloc(addrVal/tag/offset 写入)、updTag 同字段刷新（仅允许 offset=0）、
  clean(req/snp 各自清)、wakeup = RegNext(cleanHit & one(req^snp) & tagVal)。
- PosSet（16 way）：alloc 两拍流水——
  s0：matTagVec = 同 tag 且 tagVal 的 way（重入检查）；freeVec（!valid 且未被
  s1 在途占用）；blockReq = matTag | !hasFree；matchReqS1 = s1 在途同 tag → 也阻；
  block = (isSnp ? blockSnp : blockReq) | matchReqS1 | lockReg | reqPoS.valid。
  allocWay = freeWay（无 BBN canNest 恒 false 路径）。
  s1：allocReg_s1（en = !block_s0 的 valid）+ allocWayReg；**entry alloc.fire 于
  s1 拍**（allocHit = allocReg_s1.valid & !retry_s1 & way 匹配）。
  sleep_s1 = RegNext(alloc.valid & matTag)；block_s1 = RegNext(alloc.valid & block_s0)
  | reqPoS.valid；hnIdx_s1.valid = RegNext(alloc.valid) & !reqPoS.valid。
- reqPoS（ReplaceCM 要槽）：replSelWay = req→way15 / snp→way14 / 否则首个 free
  （dropRight 2）；reqPosFire = reqPoS.valid & freeVec(selWay) & !lockReg；
  posResp = RegNext(fire)+RegEnable(selWay)；**lockReg**：fire 置位、本 set 的
  updTag 清除（repl 槽地址就绪前阻塞新 alloc）。
- entry alloc.bits：reqPoS.valid 时 addrVal=0/addr=0/channel=reqPoS.channel；
  否则 addrVal=1/addr=allocReg_s1/channel=allocReg_s1.channel。
- PosTable：4 set 按 addr.posSet 分发；alrUsePoS = 全 valid 计数；working=任意 valid；
  getAddrVec.result = stateVec(set)(way).addr（catPoS(bankId, tag, set, dirBank) 重组）。
- wakeup：PosTable Mux1H（各 set 内再 Mux1H）。

#### 5.7.6 Decode（s2 = fstDec；s3 = SecDec+GetDecRes+组装）

- chiInst_s2 = task_s2.chi.getChiInst（valid 由 task_s2.valid 门控）→ fstDec →
  decList_s2（RegEnable 到 s3）。
- stateInst_s3 = respDir.valid ? respDir.getStateInst(chi.metaIdOH) : Lit(valid=1)；
  其 valid 字段 := validReg_s3 → secDec → decList_s3 → GetDecRes 查 taskCode/cmtCode。
- 一致性断言（Block+4 拍目录延迟保证）：cacheable 任务 s3 时 respDir 必有效；
  非 cacheable 必无效。
- cmtTask_s3：dir=respDir（无效时清零）、alr{reqDB|=reqDB_s3.fire、sData=fastData_s3.fire、
  sDBID 透传}、decList、task=taskCode、cmt=task.isValid?0:cmtCode、
  ds.set(addr, respDir.llc.way)。
- **快路径** respCompData = validReg & !task.isValid & cmt.sendResp & channel==DAT &
  opcode==CompData：reqDB_s3.valid（dataVec=chi.dataVec）+ fastData_s3.valid（=
  respCompData & reqDB.ready；txDat.Resp=cmt.resp、Opcode=CompData、dataOp.read+send）。
- cleanUnuseDB = validReg & alr.reqDB & !isFullSize & !(sf.hit|llc.hit)：
  cleanDB_s3 释放未用 beat（dataVec = ~chi.dataVec）。

#### 5.7.7 对拍要点（harness 激励合法性）

- rxReq 只发 decode 表内 14 个合法 opcode（reqIsLegal 子集：readNoSnp/readOnce/
  readNSD/readUnique/makeUnique/evict/cleanShared/cleanInvalid/makeInvalid/
  writeNoSnpPtl/writeUniquePtl/writeUniqueFull/writeBackFull/writeCleanFull/
  writeEvictOrEvict），addr 须带 cacheable=1（device=0）。
- memAttr.device 必须 0；非全尺寸任务必须 isAllocatingRead|isDataless|isWriteFull
  （ReqToChiTask 的 HAssert）——非 cacheable 请求只发全尺寸 WriteNoSnp 系。
- respDir 由 Directory 侧（对拍环境）按 4 拍延迟回送（rRespVec 语义：llc+sf 两
  half，命中时 wayOH/hit/meta 一致；非 cacheable 任务不回）。
- posRespVec 与 updPosTag 由 ReplaceCM 侧环境按 PoS 规则产生；cleanPoS 来自
  Backend 环境（本步对拍时由 harness 扮演 Backend+Directory 两侧环境）。

#### 5.7.8 对拍发现的四处建模陷阱（全部经生成 SV / 探针实证）

1. **RegNext(x) | y ≠ RegNext(x | y)**：PoS.scala 的
   `block_s1 = RegNext(alloc_s0.valid & block_s0) | reqPoS.valid` 与
   `hnIdx_s1.valid = RegNext(alloc_s0.valid) & !reqPoS.valid`——reqPoS.valid 的
   组合项在输出拍生效，不能一并寄存（模型曾把 req_pos_valid 写进寄存器读集，
   多阻塞一拍）。
2. **chisel PriorityMux 无匹配取末值**：Decode 的四级内容寻址译码
   `PriorityEncoder(vec.map(_ === inst))` 由 PriorityMux 实现，无匹配时返回末位
   索引（l-1）而非 0。SecDecProbe 实证：行 [0x10,0×6] 下 si=0x13（llc 命中 UC 的
   ReadNoSnp-expCompAck，表外状态）→ 6。dj_decode.h 的 dec* 函数据此返回 kL-1。
3. **TaskBuffer 仲裁器 out.ready 与 hasLockReg 无关**：生成 SV 中
   `.io_out_ready(io_chiTask_s0_ready)` 直连——锁定期间仲裁器照常每拍 fire 并
   推进 vip 指针，只是输出被锁定项覆盖。模型曾用 `!has_lock_reg` 门控 out_rdy，
   导致 vip 指针漂移（白盒对拍在 cyc38 即发现，端口失配在百拍后才浮现）。
4. **lockIdx 无 lock 时默认 N-1**：锁定窗口内若 lockVec 已空（锁定项刚发射），
   选中项 = PriorityMux 默认的末项（tb[15]）而非回退到 RR 仲裁输出。

调试基建：FE_INTCMP=1 白盒对拍（ref 经 Verilator CELL 直读内部寄存器 vs dut
.get()，覆盖 16 任务项/4×16 PoS 项/vip/hasLockReg/Block/Decode 控制寄存器，报首
个发散拍）、FE_INTWATCH=lo:hi 窗口监视、FE_TRACE=N 端口轨迹；refgen 侧
GetDecResProbe/SecDecProbe 探针裁决译码语义争议。

对拍结果：`run.sh frontend` 3 seed × 15 万拍 = **766 万比对零失配**；
dj_decode.h 无匹配语义修正后 `run.sh backend` 回归仍零失配。

### 5.8 ChiXbar 语义（dongjiang/ChiXbar.scala，纯组合分发）

本配置（nrIcn=1、nrDirBank=2、hasHPR=false、hasBBN=false）下形态：

- **rxReq 重定向**：唯一一路输入按 getDirBank = useAddr[0] = addr[6] 分发到两个
  dirBank。每 bank 一个 fastQosRRArb（N=1 退化为直通）。**QoS==0xf 的请求不进
  rxReq.out，而是改道 rxHpr.out**——Frontend 的 HPR 优先通道由此合成；
  其余进 rxReq.out。非选中分支 bits 为 DontCare（对拍仅 valid 时比对 bits）。
- **in.ready 与 valid 相关**（生成 SV 实证）：`in.ready = in.valid & 选中 bank 的
  (qosF ? hpr_rdy : req_rdy)`——VipArbiter(1) 的 `in(i).ready = selPtrOH(i) &
  out.ready`，selPtrOH 仅在本路 valid 时置位。
- **tx 四通道**（txReq/txSnp/txRsp/txDat）：nrIcn==1 直通，统一 `SrcID:=0`，
  txRsp/txDat 另 `CBusy:=io.cBusy`（DongJiang 顶层以 RegNext(posBusy) 驱动）。
- 对拍 `run.sh chixbar`：随机 5 路 flit + 随机 ready + cBusy，
  3 seed × 10 万拍 = **466 万比对零失配**。

### 5.9 P3 收口：DongJiang 顶层组装与集成（dongjiang/DongJiang.scala）

`model/home/dongjiang.{h,cpp}`：2×Frontend + Backend + Directory + DataBlock +
ChiXbar，hnx 端口与原 HnfStub 行为桩同形（原位替换，桩源码已删）。要点：

- **rx 输入 setRx**：tgt:=LAN(0)；backend.rxDat.valid := rxDat fire（抄送）。
- **合流仲裁**（DongJiang.scala:160-171）：backend.fastResp ← fastRRArb
  (fe.fastResp)（VipArb）；dataBlock.reqDB ← fastArb(backend.reqDB,
  fastRRArb(fe.reqDB_s3), fastRRArb(fe.reqDB_s1))（FixedArb index0 优先）；
  cleanDB/task 为 validOut 形态（out.ready 恒 true）；rxRsp/rxDat 单路直通。
- **cBusy** = RegNext({0, posBusy})：两 FE alrUsePoS 合计分档
  （<64/<96/<115/else → 0/1/2/3）。
- **getAddrVec**：hnIdx 广播两 FE，result 按 hnIdx.dirBank 选择回填。
- **Frontend 的 rxHpr 通道必须建模**：ChiXbar 把 QoS==0xf 请求改道 HPR，
  HprTaskBuffer 独立存在（本配置 8 项；req 侧 16 项），selectReq =
  !hprBuf.s0.valid & !hprBuf.lockTask。Frontend 模型为此补全 HPR 全通路
  （FastQueue→ReqToChiTask→HprTaskBuffer），对拍同步加 HPR 激励仍零失配。
- **集成发现的模型 bug（已修）**：BackendDecode（backend/Decode.scala）的
  `io.hnTxnIdOut.valid := RegNext(decValReg)`——与 code 输出同属第二级流水；
  模型曾用首级 dec_val_reg 组合输出，导致 commit 早一拍消费陈旧 cmtCode
  （有效 task 的 commit code 依赖后端重译码，滞后一拍即丢 OpSend/WriLLC，
  commit 永久卡在 kCommit）。backend 对拍回归在修正后仍零失配。
- **集成激励合法性**：非法 ChiInst 经 PriorityMux 落第 33 行产生垃圾行为
  （如 ReadOnce 必须 order=3 且 eca=1——译码表 8 个 ReadOnce 行全部如此）。
  test_zj_l3 据此修正激励（eca=1、order=3、单次 CompAck）。
- **上电横扫（P4b 修正版）**：目录/数据 SRAM 横扫**冻结至 DongJiang 首个请求
  唤醒**——HomeWrapper 的 DoubleCounterClockGate 门控整个 DongJiang 时钟域
  （`hnx.clock := cg.io.ock`），上电后 cken 一拍即落，横扫计数（SramResetGen）
  随之冻结；首个 REQ/HPR flit 到达 ChiBuffer 出口（inbound 组合唤醒，零延迟）
  后横扫才连续跑完（llc 8196 拍 ready、sf 2064 拍、DBIDPool 64 拍预充同理）。
  实测：hnf_0 首请求 cyc~1038 唤醒 → llc ready 9234 → 首笔 mem.ar 9251；
  hnf_1 首请求 cyc~9323 唤醒 → ready 19977。两 hnf 独立唤醒。**"时钟门控时序
  等价于常开"只在稳态成立**（working 维持 + inbound 当拍唤醒）；上电横扫期
  必须建模。模型：DongJiang 顶层 `woken` 单向锁存（首 hnx_rx_req.valid 置位），
  横扫/预充计数挂 `clk_en = woken | hnx_rx_req.valid`（SpSram/DpSram/DBIDPool
  功能使能）；另 ICG 冻结前 resetHold 已移位一次，横扫窗口首拍无条件推进
  （sram.h rst 更新 `cnt==kRstCycles || clk_en`），否则 ready 晚 1 拍（重放实测）。
  旧记录"约 8.2k 拍才 ready（从复位起算）"仅对模块级对拍成立（harness 激励
  即唤醒）。
- 端到端时序实测（单 ReadOnce miss+allocate，clk_en 恒 1 场景）：~8200 拍横扫完成后 commit →
  ReadReceipt → txReq → 内存 → CompData×2 → CompAck → writeDir → DS save，
  commit/PoS/DataCM 全部回收。
