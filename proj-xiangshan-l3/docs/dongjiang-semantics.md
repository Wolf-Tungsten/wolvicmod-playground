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

## 4. 待办提炼（后续子步骤开工前补）

- 5.2 DataBlock：BeatStorage/DataBuffer/DBIDCtrl/DataCM（八态 FSM）
- 5.3 Backend：Commit、Read/Write/Snoop/Replace/Dataless CM、Decode Pipe、仲裁网络
- 5.4 Frontend：FastQueue→ToChiTask→TaskBuffer→Block s0/s1→PoS
- 5.5 ChiXbar：组合分发 + cBusy
