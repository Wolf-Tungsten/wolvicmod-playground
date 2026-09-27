#pragma once

// DongJiang 全量组装（P3 收口）：对齐 dongjiang/DongJiang.scala（kunminghu-v3
// 单核配置：nrIcn=1、nrDirBank=2、hasHPR=false、hasBBN=false）：
//   lan.rx.req → ChiXbar 重定向（addr[6] 双 bank，QoS==0xf 改道 HPR）→ 2×Frontend
//   lan.rx.resp → Backend；lan.rx.data → DataBlock（fire 抄送 Backend.rxDat）
//   Frontend↔Directory（read/rresp）、Frontend↔Backend（cmtTask/reqPos/posResp/
//   updPosTag/cleanPoS/getAddrVec）、Backend↔Directory（write/wresp/unlock）、
//   Backend↔DataBlock（updHnTxnID/dataResp）、合流仲裁（fastRRArb=VipArb、
//   fastArb=FixedArb，validOut 形态 out.ready 恒 true）
//   cBusy = RegNext({0, posBusy(alrUsePoS 合计分档)})；working = 10 级移位或
// hnx 端口与 HnfStub 同形（P2 行为桩原位替换），另加 ci/bank_id 配置。

#include <array>
#include <cstdint>

#include "model/dj/backend.h"
#include "model/dj/chixbar.h"
#include "model/dj/data.h"
#include "model/dj/directory.h"
#include "model/dj/dj_types.h"
#include "model/dj/frontend.h"
#include "model/flit/zj_flit.h"
#include "prefab/xsarb.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/expr.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/arb.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::home {

using namespace zj::chi;
using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Valid;

class DongJiang : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(uint8_t, ci);       // config.ci（4b，单核恒 0）
    IN(uint8_t, bank_id);  // config.bankId（1b；closeLLC 未建模，恒 0）

    // hnx 侧（接 HomeShell hnx_*，与 HnfStub 同形）
    IN(Valid<RReqFlit>, hnx_rx_req);
    OUT(bool, hnx_rx_req_rdy);
    IN(Valid<RespFlit>, hnx_rx_resp);
    OUT(bool, hnx_rx_resp_rdy);
    IN(Valid<DataFlit>, hnx_rx_data);
    OUT(bool, hnx_rx_data_rdy);
    OUT(Valid<RespFlit>, hnx_tx_resp);
    IN(bool, hnx_tx_resp_rdy);
    OUT(Valid<DataFlit>, hnx_tx_data);
    IN(bool, hnx_tx_data_rdy);
    OUT(Valid<SnoopFlit>, hnx_tx_snoop);
    IN(bool, hnx_tx_snoop_rdy);
    OUT(Valid<HReqFlit>, hnx_tx_erq);
    IN(bool, hnx_tx_erq_rdy);

    OUT(bool, working);

    DongJiang();

    // 子模块公开（白盒对拍/调试惯例，与 dj 各模块一致）
    MOD(dj::Frontend, fe0);
    MOD(dj::Frontend, fe1);
    MOD(dj::Backend, backend);
    MOD(dj::Directory, directory);
    MOD(dj::DataBlock, datablock);
    MOD(dj::ChiXbar, chixbar);

    // 合流仲裁：fastRRArb（VipArbiter）/ fastArb（chisel Arbiter，index0 优先）
    using FastRespArbT = zj::prefab::VipArb<RespFlit, 2>;
    MOD(FastRespArbT, fast_resp_arb);
    using Merge2ReqDB = zj::prefab::VipArb<dj::ReqDB, 2>;
    MOD(Merge2ReqDB, reqdb_s3_arb);
    MOD(Merge2ReqDB, reqdb_s1_arb);
    MOD(Merge2ReqDB, cleandb_in_arb);
    using ReqDbArbT = wolvicmod::prefab::FixedArb<dj::ReqDB, 3>;
    MOD(ReqDbArbT, reqdb_arb);
    using CleanDbArbT = wolvicmod::prefab::FixedArb<dj::ReqDB, 2>;
    MOD(CleanDbArbT, cleandb_arb);
    using Merge2Task = zj::prefab::VipArb<dj::DataTask, 2>;
    MOD(Merge2Task, task_in_arb);
    using TaskArbT = wolvicmod::prefab::FixedArb<dj::DataTask, 2>;
    MOD(TaskArbT, task_arb);

    REG(uint8_t, cbusy_reg);  // RegNext({0, posBusy})，3bit
    REG(uint16_t, work_sft);  // max(readDirLatency,readDsLatency)*2 = 10 级移位
    // HomeWrapper DoubleCounterClockGate 功能等效：上电后 DongJiang 时钟域冻结，
    // 首个 REQ/HPR flit 到达（inbound 组合唤醒，零延迟）后 woken 单向锁存；
    // 此后稳态流量下门控与常开等价（working 维持 + inbound 即醒）。仅"上电横扫/
    // 预充计数"（目录 SRAM / DBIDPool）挂 w_clk_en——冻结期其余逻辑本无活动。
    REG(bool, woken);
    WIRE(bool, w_clk_en);

    using A2Bool = std::array<bool, 2>;
    using A2VReq = std::array<Valid<dj::ReqDB>, 2>;
    using A2VTask = std::array<Valid<dj::DataTask>, 2>;
    using A2VRsp = std::array<Valid<RespFlit>, 2>;
    using A3VReq = std::array<Valid<dj::ReqDB>, 3>;
    using U8x3 = std::array<uint8_t, 3>;
    using A3Bool = std::array<bool, 3>;
};

}  // namespace zj::home
