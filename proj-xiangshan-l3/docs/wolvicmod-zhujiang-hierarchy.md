# Wolvicmod 建模 ZhuJiang：模块层次框架规划

目标：用 wolvicmod 周期精确（cycle-accurate）重实现 XiangShan 昆明湖 V3 的 ZhuJiang（L3 + 环形 NoC），最终替换 RTL ZhuJiang 跑通 coremark。

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

省略清单：HPR/DBG 环（hasHprRing=false、hwa 关）、BBN（`require(!hasBBN)`）、c2c、MBIST/DFT、QoS（环内仲裁不使用）、RI 数据通路、ZJPerf。时钟门控（DoubleCounterClockGate）**稳态等价于常开**（working 维持 + inbound 组合唤醒零延迟），不建睡/醒循环；但**上电横扫期冻结**必须建模——DongJiang 顶层 `woken` 锁存 + 横扫/预充计数功能使能（§3.4 as-built 与 dongjiang-semantics §9）。

---

## 2. wolvicmod 建模约定

### 2.1 信号与类型

- **flit 用 C++ struct 位级定义**：`ReqFlit/RespFlit/DataFlit/SnoopFlit/HReqFlit`（zhujiang 格式，`ZJ/zhujiang/chi/Flit.scala:27-127`）与 `CHIREQ/CHIRSP/CHIDAT/CHISNP`（xscache 格式，`XSCache/src/main/scala/xscache/chi/Message.scala:428-557`）各一套，字段用 `uint64_t` + 位段辅助函数。注意 **DAT.DBID：zhujiang 16b vs xscache 12b**，适配层做零扩展/截断（对齐 `ZhuJiangBridge.scala:213-214,232`）——✅ 已实现于 `proj-xiangshan-l3/model/flit/`（`bit_pack.h` 位段助手 / `zj_flit.h` / `xs_flit.h`）与 `model/ring/`（`ring_slot.h`），单测 `tests/test_flit.cpp`）。**参数化方式 = 编译期 config traits**（模板参数 `Cfg`，与 RTL elaboration-time Parameters 同级；默认 = kunminghu-v3 锁定值）。实测锁定值：niw=**11**（`ZhuJiangNoCTopology.scala:17` 覆盖 nodeNidBits=8，非 ZJParameters 默认 5）、raw=48、dw=256、CHI Issue=E.b（Makefile 钉死，ZhuJiang 只支持 E.b）；总宽经生成 RTL 端口核实：环 REQ 105/RSP 66/DAT 375/HRQ 128（`build/rtl/Router*.sv`），xscache seam REQ 118/RSP 66/DAT 367/SNP 102（`CoupledL2.sv` `io_decoupledCHI_*`）
- **xscache seam 是 CHI Bundle 的裁剪子集**（firtool 裁掉桥不读写的字段，剩余字段即边界契约）：CHIREQ 裁 returnNID/returnTxnID/ns/likelyshared/allowRetry/pCrdType/lpIDWithPadding/tagOp/traceTag（mpam 仅 partID 9b；mpam/rsvdc 被 mapReq 读但落入 zhujiang 零宽字段，无语义）；**rx_rsp 无 tgtID**（mapRsp 不回填恒 0，`ZhuJiangBridge.scala:187-199`）；CHIDAT 裁 ccID/tagOp/tag/tu/traceTag/rsvdc（dataCheck/poison 配置 require 关闭）；CHISNP 裁 ns(恒 false)/traceTag/mpam
- **Decoupled 通道 = 两个端口**：`Out<FlitTx>`（`{valid, bits}` 合体 struct）+ `In<Bool>` ready；fire = valid && ready。反压路径保持组合
- **环链路（valid-only 无 ready）**：`RingSlot {valid, flit, rsvdValid, rsvdPayload}`，每站每通道每方向一个 `Reg<RingSlot>`
- **复位**：全局同步复位一根 `Bool`，Update 用 `.on(posedge(clk))` + 复位优先级（先注册）；不建模 M 节点的两相复位时序

### 2.2 时序原语库（P0 里程碑，先于一切业务模块） ✅ 已完成

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

## 3. 模块层次框架

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
`ring.h` STOP_TABLE+Ring 组装），环级对拍零失配（见 §5.1 与实施计划第 2 步）。

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

## 4. 集成边界与 DPI-C 方案（已落地 ✅，P5）

- wolvicmod 模型编译为静态库 `libzjmodel.a`，根模块 `WolvicZjTop` 暴露 `set/eval/get` 接口（对齐 Verilator eval 语义，§5.1 框架规划文档）
- XiangShan 侧替换点：chisel BlackBox `WolvicZjBB`（`--wolvic-zj` 开关，`src/main/scala/top/WolvicZjBB.scala`）整体替换 `Zhujiang` 实例（`Top.scala:378-382`）+ `connectCHIToZhuJiang` 的 SocketDevSide + flit remap——边界与 WolvicZjTop 完全一致（L2 CHI 六通道 + memAXI + cfgAXI）
- **单调用时序方案**：`tests/test_comb_audit.cpp` 常驻审计证明模型边界零组合穿透（根 In→根 Out 无纯组合路径），故 SV 薄壳 `dpi/sv/WolvicZjBB.sv` 每 `posedge` 调一次 DPI `wolvic_zj_step`：set 边沿前输入 → clk 0→1 提交 → 返回新输出（NBA 寄存）；复位期间不调用（模型构造态=复位完成态）
- 薄壳内做 struct↔位向量 的打包/解包：in_pack 1107 位 / out_pack 1443 位（布局 `dpi/csrc/wolvic_zj_pack.h`，SV `$fatal` 宽度断言 + C++ drift 检查兜底）；rn 侧 flit 位宽：REQ 118b / RSP 66b / DAT 367b / SNP 102b（xscache 扩展字段在壳内丢弃）
- 注入机制：SV 经 difftest `RTL_INCLUDE`（→ verilator `-y` libdir）；C++ 静态库 `libwolviczj_dpi.a` 经 difftest 新增 `USER_CXXFILES/USER_CXXFLAGS/USER_LDFLAGS` 钩子链接；emu.cpp 零改动（备选"直接链 difftest emu C++ 侧"未采用——DPI 方案不动 difftest 主体，更干净）
- 调试钩子：环境变量 `WOLVIC_ZJ_TRACE=<path>` 落每拍 in/out pack hex，与 golden trace 对拍定位分叉拍

## 5. 验证策略（周期对齐怎么保证）

1. **原语级**：P0 每个原语对拍 Chisel 语义（定向随机激励，valid/ready 全组合扫掠）——✅ 已达成：预制菜 vs 真实 Chisel 源码（chisel 7.13.0 → firtool → Verilator 5.047）逐拍对拍，两侧分治——wolvicmod 侧 `wolvicmod/verify/`（chisel 通用 4 元件 10 配置 × 3 seed，330 万拍 / 1241 万次比对）、项目侧 `proj-xiangshan-l3/verify/`（XiangShan 生态 6 元件 17 配置 × 3 seed，528 万拍 / 2914 万次比对），合计 858 万拍 / 4155 万次比对，零失配
2. **模块级 trace 对拍**：从 RTL emu 抓取 golden trace——在 `ZhuJiangBridge`（或加 CHILogger 等价物）记录 CC socket 边界每拍六通道的 valid/ready/bits，以及 mem/cfg AXI 边界每拍五通道；coremark 全程
3. **重放对拍**：wolvicmod 模型载入 golden trace 的输入侧（rx 方向），逐拍驱动，输出侧（tx 方向）逐拍与 trace 比对；首个不一致拍即定位（wolvicmod 的 round 级事件记录，框架文档 §6.3）
4. **系统级**：集成进 emu 跑 coremark，判据 = difftest 通过 + `HIT GOOD TRAP` + cycleCnt 与 RTL 基线 316,801 完全相等

## 6. 里程碑

| 阶段 | 内容 | 验收 |
|---|---|---|
| P0 | 时序原语库（§2.2）+ 单测 | ✅ 已达成：两侧 ctest 全绿（逐文件独立条目）+ RTL 对拍零失配（两侧 `verify/run.sh`：27 配置 × 3 seed × 10 万拍，累计 858 万拍 / 4155 万次比对） |
| P1 | Ring + RouterStop + ChannelTap/EjectBuffer | 环上传输 trace 对拍 |
| P2 | XscChiAdapter + CcSocket + HomeWrapper 外壳 | ✅ 已达成：单测全绿 + socket 对拍 5220 万比对零失配 + **coremark 前端 2 万拍 trace 重放到 CC 边界（26.5 万比对零失配，`make replay`）**（HNF 用行为桩） |
| P3 | DongJiang 全量（Directory→DataBlock→Backend→Frontend→ChiXbar） | ✅ 已达成：五子模块独立对拍全零失配（dir 544万/db 213万/backend 1445万/frontend 766万/chixbar 466万比对）+ 顶层组装替换行为桩，test_wolvic_zj_top 端到端全绿 |
| P4 | S/HI 桥 + 顶层组装 | ✅ 已达成：P4a 两桥对拍 941 万比对零失配；P4b WolvicZjTop 三边界顶层 + RI tie-off 内收 + 时钟门控 woken 建模 + **coremark 全程 316,748 拍重放 542 万比对零失配**（`make replay-top`） |
| P5 | DPI-C 集成 + coremark 系统级验证 | ✅ 已达成：BlackBox `WolvicZjBB` 整体替换 + SV 薄壳单调用方案（零组合穿透审计背书）+ difftest `USER_*`/`RTL_INCLUDE` 注入；**coremark `HIT GOOD TRAP`，difftest 663,692 指令零失配，cycleCnt = 316,801 与 RTL 逐拍完全相等**（`make emu WOLVIC=1`，876s） |

## 7. 风险与注意点

- **工作量集中于 DongJiang**：五个状态机族（TaskBuffer/PoS/Commit/四 CM/DataCM）+ 两张目录 + 约 20 个仲裁点，建议 P3 内部再按 §3.5 的 5 步细分
- **防死锁机制不能省**：环的 rsvd 令牌、EjectBuffer VIP 末槽、TaskBuffer 超时锁定、目录 lockTable——它们在 coremark 中未必触发，但少一个就可能在某次反压下死锁
- **同地址三级串行**（TaskBuffer sort → PoS sleep/wakeup → 目录 lockTable）是正确性关键路径，对拍用例必须覆盖
- **DBID 位宽坑**（zhujiang 16b vs xscache 12b）在适配层显式断言，值域不超 12 位语义
- 时钟门控：稳态睡/醒省略（时序等价，§3.4 已论证）；**上电横扫冻结不可省**（P4b 实证——省略则首笔内存访问早 ~1000 拍，边界立即失配），已建模为 DongJiang `woken` + 横扫/预充 `clk_en` 功能使能；若未来对功耗建模再补睡/醒细节
