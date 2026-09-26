# DongJiang 语义提炼笔记（P3）

> 来源：XSCache/ZhuJiang `src/main/scala/dongjiang/**`，配置链取 kunminghu-v3 `DefaultConfig + LLC=ZhuJiang`（单核，即跑通 coremark 的配置）。
> 每个 §N 对应实施计划的一个子步骤，建模前填写，对拍后校正。

## 1. 全局配置推导（定死，所有子步骤共用）

### 1.1 配置链

- `DefaultConfig` = `ZhuJiangConfig("32MB", ways=16)` + L2 2MB + ...（Configs.scala:581）→ `cacheSizeInB=32MB, cacheWays=16`。
- `Top.scala:350` → `ZhuJiangNoCTopology(1, ZJParameters(), 256)`：`nodeNidBits=8, nodeAidBits=3`，单核 10 节点：HF(bank0,hfp0)、CC、HF(bank1,hfp0)、RI、HI(defaultHni)、HF(bank1,hfp1)、S、HF(bank0,hfp1)、M、P。**无 RH → hasHPR=false；无 BBN（r2rPos 空）**。
- `ZJParameters` 默认：`requestAddrBits=48, hnxBankOff=12, ciIdBits=4, dataBits=256, clusterCacheSizeInB=2MB, snoopFilterWays=16, hnxOutstanding=256, hnxDirSRAMBank=2`。

### 1.2 djParams 推导（ZJParameters.scala:246，djParamsOpt=None 分支）

bank = hfpId==0 的 HF 数 = **2**。一个 DongJiang 服务一个 bank（HomeWrapper 内，两 hfp 端口共用）：

```
DJParam{ addressBits=48, llcSizeInB=16MB(32M/2), sfSizeInB=2MB(2M*2*1/2),
  llcWays=16, sfWays=16, nrDirBank=2, nrDSBank=4, nrPoS=128(256/2),
  dataBufSizeInByte=4096(32*256/2), nrReqTaskBuf=32, nrHprTaskBuf=16, nrSnpTaskBuf=0,
  openDCT=true, dirRamSetup=1, dirRamLatency=2, dirRamExtraHold=false,
  dataRamSetup=2, dataRamLatency=2, dataRamExtraHold=false }
```

### 1.3 HasDJParam 派生（每 DongJiang 实例）

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

## 2. DongJiang 顶层结构（DongJiang.scala）

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

## 3. 5.1 Directory 语义（directory/Directory.scala + DirectoryBase.scala）

### 3.1 组装

- `Directory` = nrDirBank(2) × (DirectoryBase("llc") + DirectoryBase("sf"))，同 bank 的 llc/sf **读联动**：`llc.read.valid = readVec.valid & sf.read.ready`（反向对称），`readVec.ready = llc.ready & sf.ready` → 同一拍进两边，读响应天然同拍。
- 写按 `Addr.dirBank` 分发；`write.ready = (llcWReady | !llc.valid) & (sfWReady | !sf.valid)`；llc/sf 的 valid 独立（可只写一边）。
- `rRespVec(i)` = Valid(DirMsg{llc{wayOH,hit,metaVec}, sf{…}})，valid = `llcResp.valid & !toRepl`。**不带 addr/hnTxnID**（前端靠自身流水对号）。
- `wResp.llc/sf` = Valid(DirEntry{addr,wayOH,hit,metaVec}+hnTxnID)，valid = `resp.valid & toRepl`，按 bank 序 priority 取第一个（同一 write 通道 fire 间隔 ≥2 拍 ⇒ 4 拍后 resp 不可能同拍，PopCount≤1 恒成立）。
- unlock 广播到全部 4 个 DirectoryBase，各自按 hnIdx.dirBank==本 bank 过滤。

### 3.2 DirectoryBase 四拍流水

阶段记号 d0（fire 拍）→d4（第 4 拍）。`Shift{read,write,repl}` 各 4bit 右移寄存器：fire 从 bit3 进；d1=bit3 … d4=bit0。`reqSftReg`（addr+hnIdx+wriWayOH+metaVec）同步下移，仅在 `shiftReg.req.orR | 新fire` 时移位（**无请求时保持**）。

| 阶段 | 逻辑 |
|---|---|
| d0 | 端口仲裁 **repl_d0(d4 分配写回) > write > read**。meta/tag 单口 SRAM 发 req（setup=1+lat=2+outputReg ⇒ d3 出数）；repl 双口 SRAM 发 rreq（d2 出数）。`tagMetaReady = !(req 在 d1)` ⇒ **新请求至少隔 2 拍**；`replWillWrite = (repl&read).orR` 期间禁止任何新读写。`io.read.ready = resetDone & tagMetaReady & !replWillWrite & !io.write.valid`；`io.write.ready` 无末项。 |
| d1 | 无逻辑（等 SRAM）。 |
| d2 | `replMes`：d4 写回同 set 两级前递（d2 组合看 d4、d1 寄存在 d2 看 d4），否则取 repl SRAM rresp。`useWayVec = lockTable ∪ reservationTable(仅sf) ∪ pendingAlloc_d3` 中 set 匹配的 way 的 OH 并集。`replWay = PLRU.get_replace_way(replMes)`；`unuseWay = PriorityEncoder(~useWayVec)`。 |
| d3 | tag/meta 出数。`hitVec = tagHit & metaVal`；`invalidVec = !metaVal & !useWay(d2寄存)`；**selWay 优先级：hit > 有 invalid（取首个 invalid）> replWay 被占（取 unuseWay）> replWay**。`newReplMes = PLRU.get_next_state(replMesReg, wriUpdRepl_d3 ? wriWayOH : selWay)`。仅 sf：`pendingAlloc = read & !hit & hasInvalid`。组 resp：addr 由 {config.bankId, tag(selWay), reqSet, dirBank} 重组（victim 地址）、wayOH、hit、metaVec(selWay)、hnTxnID、toRepl=repl(d3)。lockTable/reservation 更新（§3.4）。 |
| d4 | `resp.valid = read(d4)`。repl 数组写回三选一：`wriUpdRepl_d4`（写类，触 PLRU，way=wriWayOH）/ `updTagMeta_d4`（分配写回：tag+meta SRAM 写选中 way + PLRU 更新，数据来自 reqSftReg 携带的写请求字段）/ `outDirResp & readHit`（读命中触 PLRU）。 |

### 3.3 请求类型 → shift(read,write,repl) 与行为

| 请求 | d0 SRAM 行为 | (r,w,repl) | 后续 |
|---|---|---|---|
| 前端读 `read.fire` | meta/tag/repl 全读 | (1,0,0) | d4 resp 给前端，toRepl=0；命中则 d4 触 PLRU |
| 后端写命中（hit=1, directAlloc=0） | meta 写（wayOH 掩码，数据广播） | (0,1,0) | 无 resp；d4 wriUpdRepl 触 PLRU |
| 后端 directAlloc 写（仅 sf） | tag+meta 写 | (0,1,0) | 无 resp；清同 hnIdx reservation；d4 触 PLRU |
| 后端 wriNoHit（hit=0, directAlloc=0，替换申请） | meta/tag/repl 全读 | (1,0,1) | d3 选牺牲路；d4 resp(toRepl=1)→wResp（victim 地址/way/meta/hnTxnID）；**同拍 d4 updTagMeta 把 reqSftReg 携带的新 tag/meta 写入选中 way + PLRU 更新**（即 repl_d0，占端口 2 拍） |
| repl_d0 幻影项 | meta/tag 写（d4 写回的 SRAM 请求） | (0,1,1) | 仅占位阻塞，d3/d4 无效果、无 resp |

（recRead = req.fire & !req.write；wriNoHit 的 meta req.write=0 故记为 read；readRepl_d3 = read&!write&repl，wriRepl = !read&write&repl。）

### 3.4 lockTable / reservationTable

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

### 3.5 数据类型

- ChiState：llc 2bit {I=0, SC=1, UD=2, UC=3}（isValid = !=I）；sf 1bit（isValid=state[0]）。
- 读口：`Addr{addr48}` + hnIdx。写口：`DirEntry{addr48, wayOH16, hit, metaVec[1×ChiState]}` + hnIdx + directAlloc。
- resp：`DirEntry + hnTxnID7 + toRepl`；rRespVec 只保留 DirMsg{wayOH,hit,metaVec}×{llc,sf}。

### 3.6 PLRU（rocket-chip PseudoLRU，16 路 15 bit）

- 树形位布局（16 路）：state[14]=root（1 ⇒ 左子树 way7-0 更老），左子树状态 = state[13:7]，右子树 = state[6:0]，递归；叶节点 1bit = 右孩子(奇数 way)更老。
- `get_replace_way`：从根向叶，每级取"更老"方向（bit=1 走左），路径即 way 编码（MSB 先行）。
- `get_next_state(state, touchWay)`：沿 touch 路径把各级 bit 指向"另一子树更老"，未触子树递归不变。
- 模型用同构 C++ 递归（对 16 路完全展开），与 PLRUTest 的 2/3/4/5/6 路断言可互验。

### 3.7 SRAM 与复位

- meta/tag：单口、way=16、waymask=wayOH、写数据全 way 广播。repl：双口、way=1、bypassWrite。
- `shouldReset`（meta、repl；tag 不复位，靠 meta valid=0 屏蔽）：SramResetGen 上电扫描清零，resetDelay=4 拍 + set×interval(=setup=1) 拍 ⇒ llc meta ≈ 8196 拍、repl 相同（并行）。期间 req.ready=0。**横扫写与正常写共享 intvCnt 重装路径**（`when(ramRen||ramWen) intvCnt := interval-1`）：末笔横扫写后 ready 还要再延迟 interval-1 拍（llc meta=8197 拍才 ready；dir 对拍实证，prefab SpSram/DpSram 已修）。
- `resetDoneReg = RegEnable(true, metaReq.ready & replR.ready & replW.ready)` 门控 io.read/write.ready（llc DirectoryBase read_rdy 首真于 8198 拍）。

### 3.8 对拍要点（harness 激励合法性）

- 写 directAlloc 须 hit=0 且 metaVec 有效（RTL 断言）；sf directAlloc 须命中 reservation（owner 匹配）。
- read 与 write 同拍只能活一个（read.ready 含 !write.valid；harness 侧 write 优先）。
- 响应间隔 ≥2 拍；wResp 每拍 ≤1。
- unlock 的 hnIdx 必须指向已锁项（PopCount==1 断言），且 way < lockWays。

## 4. 5.2 DataBlock 语义（data/{DataBlock,BeatStorage,DataBuffer,DBIDCtrl,DataCM}.scala）

### 4.1 组装（DataBlock.scala）

- `beatStorage` = nrDSBank(4) × nrBeat(2) 个 BeatStorage（每实例一条 HomeDatRam：
  SpSram 256bit × 65536 组，setup=2+lat=2+outputReg ⇒ **5 拍出数**）。
- `DataCM`（64 entry 控制 FSM）+ `DBIDCtrl`（dbid 分配池）+ `DataBuffer`（写数据缓冲）。
- 端口：txDat(Decoupled DataFlit 出)、rxDat(入)、updHnTxnID、reqDB(HnTxnID+dataVec 入)、
  task(Valid DataTask 入)、resp(Valid HnTxnID 出)、cleanDB(HnTxnID+dataVec 入)。
- txDat 二选一：`dbToCHI`(buf.toCHI.valid) 优先，`dsToCHI`(dsResp.valid & toCHI) 次之；
  bits 取 `dataCM.getChiDat.bits`（entry 寄存的 DataFlit），DataID/Data/BE 按源替换
  （dsResp 侧 BE=全 1）。`rxDat.ready = buf.fromCHI.ready = !dsResp.valid`。
- DS 读写口按 (bank, beatNum) 交叉分发；dsResp 经两级 fastArb（先 8 合 1 后 Pipe）入 buf。

### 4.2 BeatStorage（5 拍流水）

- shift{read,write} 5bit；`reqReady = !req(4)`（请求隔 2 拍，对齐 SRAM interval=2）；
  `write.ready = rstDone & reqReady`，`read.ready = … & !write.valid`（写优先）。
- 无 shouldReset（数据阵列不复位）→ rstDoneReg 第 1 拍即锁存。
- resp：shift.outResp（d0+5）+ respPipe(5) 携带 {dcid, dbid, beatNum, toCHI}。

### 4.3 DBIDPool / DBIDCtrl

- Pool = 2 × FastQueue(64)（dbid 7bit，偶/奇分queue）；上电 64 拍逐拍预充
  （q0←Cat(0,i)、q1←Cat(1,i)），rstDone 后 deq.valid 放行。
- enq（release 回池）：1 个 → 短queue（count<=）；2 个 → 一边一个。
- deq（alloc）：hasTwo = 两 queue 均非空 ⇒ `req.ready`；req.bits(i)=1 才弹对应输出；
  只弹 1 个时从**长**queue取（count>=），2 个时一边一个。resp(i) 组合直连。

### 4.4 DataBuffer（datBuf：DpSram 8bit × 128 组(dbid) × 32 字节lane，1 拍读）

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

### 4.5 DataCtrlEntry FSM（8 态，entry 数 = 64 = nrDataCM）

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

### 4.6 DataCM 仲裁

- reqDBIn.ready = reqDBOut.ready & hasFreeDC；alloc 到 freeDCID（首个 FREE entry），
  dbidVec 同拍取自 DBIDCtrl.resp。**task 延迟 1 拍**（taskReg/taskFireReg）广播全 entry。
- 读仲裁：repl 优先——某 entry isRepl 时其 readToDB/readToDS **捆绑同发**
  （out.valid 互接对方 ready，两边同 ready 才 fire）；多个 repl 取 critical 优先再按 dcid 序。
  非 repl 走 connectReadToX：critical（valid&critical）唯一者优先，否则 fastQosRRArb
  （QoS==0xf 高优先层 + VipArbiter RR；resp/release 用 fastRRArb.validOut（RR，valid-only））。
- getDBID：TxnID 匹配唯一 entry → dbidVec(entry)(DataID==2?1:0)。
- 工具件语义（FastArb.scala）：fastArb=chisel 固定优先 Arbiter；fastRRArb=VipArbiter；
  fastQosRRArb=qos 0xf 高优层套 VipArbiter。均有 prefab（wolvicmod VipArb / proj FastQueue 已验证）。

### 4.7 对拍要点（harness 激励合法性）

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

## 5. 5.3 Backend 语义（backend/{Backend,Commit,ReplaceCM,SnoopCM,ReadCM,WriteCM,Decode,Bundle}.scala）

### 5.1 组装（Backend.scala）

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
  FastQueue(commit), FastQueue(repl), FastQueue(writeCM)）。
- cmResp 二路 Pipe(fastQosRRArb.validOut(snoop, read, write))：toRepl=0→commit、=1→repl。
- alloc 三路：snoopCM ← fastQosRRArb(FQ(commit.cmTaskVec(SNP)), FQ(repl.cmTaskVec(SNP)))；
  writeCM ← 同构(WRI)；readCM ← fastQosRRArb(FQ(commit.cmTaskVec(READ)))。
- getAddrVec(0) ← txReq.TxnID 的 hnIdx；getAddrVec(1) ← txSnp.TxnID；getAddrVec(2) ←
  replCM.writeDir.llc.addr。txReq.bits.Addr 按 MemAttr/Size 对齐改写（!cacheable→原址；
  size=6→64B 对齐；否则→32B 对齐）；txSnp.bits.Addr = (addr>>6)<<3。
- commit.cmtTaskVec 每路先 Pipe(1)。

### 5.2 CommitEntry（五态 FSM，entry=112=2 银行 ×(posWays-2=14)×posSets 4）

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

### 5.3 译码 Pipe（backend/Decode.scala + frontend/decode/Bundle.scala）

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

### 5.4 ReplaceEntry（十八态 FSM，entry=64）

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

### 5.5 SnoopEntry（五态，entry=32）

FREE →(alloc)→ PRESNP（SnpUniqueFwd 且 snpVec>1）/ SENDSNP；
PRESNP →(倒数第 2 个 txSnp.fire)→ SENDSNP（PRESNP 段发 SnpMakeInvalid）；
SENDSNP →(alrSnpAll)→ WAITRESP →(alrGetAll=响应齐&数据齐)→ RESPCMT →(resp.fire)→ FREE。
- txSnp：PRESNP 段 opcode=SnpMakeInvalid，否则 task.chi.opcode；
  RetToSrc = 最后一个 & task.retToSrc；TgtID=各 meta 对应节点（本配置恒 0x09）。
- 响应合并：rspHit/datHit 时 taskInst 按优先级合并（fwdValid OR、channel DAT>RSP、
  opcode/resp 依 fwd/channel 保持或覆盖、resp 不劣化）；respErr 取首个非 OK。
- nrSfMetas=1：snpVec 恒单位，PRESNP 路径实际只用于 SnpUniqueFwd 单节点（PopCount=1
  不满足 >1，故直接 SENDSNP）。

### 5.6 ReadEntry（八态，entry=64）

FREE →(alloc)→ SENDREQ（本配置无 BBN，CANNEST/CANTNEST/SENDACK 不到达）；
SENDREQ →(txReq.fire)→ doDMT?RESPCMT:WAITDATA0；
WAITDATA0 →(recDataHit)→ isHalfSize?RESPCMT:WAITDATA1 →(recDataHit)→ RESPCMT →(resp.fire)→ FREE。
- txReq：ExpCompAck/MemAttr/Size/Opcode 透传，Order=None，ReturnTxnID/NID 按 doDMT
  （本配置 openDCT 的 DCT 回传字段，LAN 侧 doDMT 仍可出现）。
- recDataHit：opcode=CompData 且 TxnID 匹配；记 nodeId←HomeNID、txnID←DBID、
  taskInst.resp←Resp、respErr。
- resp：!doDMT → taskInst{valid, channel=DAT, opcode=CompData, resp=reg.resp, fwdResp=I}；
  doDMT → 仅 valid。

### 5.7 WriteEntry（八态，entry=32）

FREE →(alloc)→ SENDREQ →(txReq.fire)→ WAITDBID →(dbidHit)→ DATATASK →(dataTask.fire)→
WAITDATA →(dataRespHit)→ RESPCMT →(resp.fire)→ FREE（isRespCmt = state==RESPCMT & alrGetComp）。
- dbidHit = Rsp.CompDBIDResp|DBIDResp：txnID←DBID、nodeId←SrcID、alrGetComp |= (op==CompDBIDResp)、
  respErr；compHit = CompDBIDResp|Comp：alrGetComp=1、respErr。
- dataTask：dataOp 透传、txDat.Resp=cbResp、Opcode=isImmediateWrite?NonCopyBackWriteData:CopyBackWriteData。

### 5.8 共享件

- `Alloc`（dongjiang/utils/Alloc.scala）：CM 池分配（Snoop/Read/Write 用；prefab 已对拍）。
- fastQosRRArb/fastRRArb/fastArb/VipArbiter/FastQueue：同 §4.6，prefab 齐备。
- Alloc 语义已由 P0 `alloc` 对拍覆盖（AllocRef_n4/n16）。

## 6. 5.3 Backend 对拍补充（harness 环境模型与框架纪律）

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

## 7. 待办提炼（后续子步骤开工前补）

- 5.4 Frontend：FastQueue→ToChiTask→TaskBuffer→Block s0/s1→PoS
- 5.5 ChiXbar：组合分发 + cBusy
