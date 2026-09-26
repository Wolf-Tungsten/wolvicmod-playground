#pragma once

// Backend：对齐 backend/Backend.scala（语义 §5.1）。Commit + ReplaceCM +
// SnoopCM + WriteCM + ReadCM 顶层组装（本配置无 BBN / DatalessCM / updPosNest）。

#include <array>
#include <cstdint>

#include "model/dj/backend_types.h"
#include "model/dj/cm.h"
#include "model/dj/commit.h"
#include "model/dj/replace.h"
#include "prefab/fastq.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/arb.h"
#include "wolvicmod/prefab/pipe.h"
#include "wolvicmod/prefab/queue.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::dj {

using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::FixedArb;
using wolvicmod::prefab::Valid;
using wolvicmod::prefab::ValidPipe;
using zj::prefab::FastQueue;

class Backend : public wolvicmod::Module {
public:
    using ReqPosArr = std::array<std::array<Valid<ReplReqPos>, 4>, 2>;
    using PosRespArr = std::array<std::array<Valid<uint8_t>, 4>, 2>;

    IN(bool, clk);
    IN(uint8_t, cfg_ci);
    IN(uint8_t, cfg_bank_id);

    OUT(Valid<HReqFlit>, tx_req);
    IN(bool, tx_req_rdy);
    OUT(Valid<SnoopFlit>, tx_snp);
    IN(bool, tx_snp_rdy);
    OUT(Valid<RespFlit>, tx_rsp);
    IN(bool, tx_rsp_rdy);
    IN(Valid<RespFlit>, rx_rsp);
    OUT(bool, rx_rsp_rdy);
    IN(Valid<DataFlit>, rx_dat);

    OUT(Valid<DirWrBoth>, write_dir);
    IN(bool, write_dir_rdy);
    IN(Valid<DirResp>, resp_dir_llc);
    IN(Valid<DirResp>, resp_dir_sf);

    OUT(ReqPosArr, req_pos_vec);
    IN(PosRespArr, pos_resp_vec);
    OUT(Valid<UpdPosTag>, upd_pos_tag);
    IN(Valid<CommitTask>, cmt_task_0);
    IN(Valid<CommitTask>, cmt_task_1);
    OUT(Valid<PosClean>, clean_pos);
    OUT(Valid<uint8_t>, unlock);
    IN(Valid<RespFlit>, fast_resp);
    OUT(bool, fast_resp_rdy);
    OUT(Valid<UpdHnTxnID>, upd_hn_txn_id);
    OUT(Valid<ReqDB>, req_db);
    IN(bool, req_db_rdy);
    OUT(Valid<DataTask>, data_task);
    IN(bool, data_task_rdy);
    IN(Valid<uint8_t>, data_resp);
    OUT(Valid<ReqDB>, clean_db);
    IN(bool, clean_db_rdy);
    OUT(uint8_t, get_addr_0_hnidx);
    IN(uint64_t, get_addr_0_result);
    OUT(uint8_t, get_addr_1_hnidx);
    IN(uint64_t, get_addr_1_result);
    OUT(uint8_t, get_addr_2_hnidx);
    IN(uint64_t, get_addr_2_result);

    MOD(Commit, commit);
    MOD(ReplaceCM, repl_cm);
    MOD(SnoopCM, snoop_cm);
    MOD(WriteCM, write_cm);
    MOD(ReadCM, read_cm);

    // cmtTaskVec 的 Pipe(1)
    using CmtPipeT = ValidPipe<CommitTask, 1>;
    MOD(CmtPipeT, cmt_pipe_0);
    MOD(CmtPipeT, cmt_pipe_1);
    // txReq 仲裁+队列；txRsp 仲裁（commit.txRsp vs FastQueue(fastResp)）
    using TxReqArbT = QosRRArb<HReqFlit, 2>;
    using TxReqQT = FastQueue<HReqFlit, 2, false>;
    using TxRspArbT = QosRRArb<RespFlit, 2>;
    using FastRespQT = FastQueue<RespFlit, 2, false>;
    MOD(TxReqArbT, tx_req_arb);
    MOD(TxReqQT, tx_req_q);
    MOD(TxRspArbT, tx_rsp_arb);
    MOD(FastRespQT, fast_resp_q);
    // dataTask 三路（各自先过 FastQueue 再 QosRR）
    using DataTaskQT = FastQueue<DataTask, 2, false>;
    using DataTaskArbT = QosRRArb<DataTask, 3>;
    MOD(DataTaskQT, cmt_dt_q);
    MOD(DataTaskQT, repl_dt_q);
    MOD(DataTaskQT, wri_dt_q);
    MOD(DataTaskArbT, data_task_arb);
    // reqDB 固定优先（repl 优先）
    using ReqDbArbT = FixedArb<ReqDB, 2>;
    MOD(ReqDbArbT, req_db_arb);
    // cmResp 合流 + Pipe
    using CmRespArbT = QosRRArb<CMResp, 3>;
    using CmRespPipeT = ValidPipe<CMResp, 1>;
    MOD(CmRespArbT, cm_resp_arb);
    MOD(CmRespPipeT, cm_resp_pipe);
    // alloc 三路（commit/repl 的 cmTask 先 FastQueue 再 QosRR 分发）
    using CmTaskQT = FastQueue<CMTask, 2, false>;
    using SnpAllocArbT = QosRRArb<CMTask, 2>;
    using WriAllocArbT = QosRRArb<CMTask, 2>;
    using ReadAllocArbT = QosRRArb<CMTask, 1>;
    MOD(CmTaskQT, cmt_snp_q);
    MOD(CmTaskQT, repl_snp_q);
    MOD(SnpAllocArbT, snp_alloc_arb);
    MOD(CmTaskQT, cmt_wri_q);
    MOD(CmTaskQT, repl_wri_q);
    MOD(WriAllocArbT, wri_alloc_arb);
    MOD(CmTaskQT, cmt_read_q);
    MOD(ReadAllocArbT, read_alloc_arb);
    // cleanPoS 合流队列（cleanPoS/unlock/cleanDB 三分叉）
    using CleanArbT = QosRRArb<PosClean, 2>;
    using CleanQT = FastQueue<PosClean, 2, false>;
    MOD(CleanArbT, clean_arb);
    MOD(CleanQT, clean_q);
    // writeDir Queue(1, pipe)
    using WDirQT = wolvicmod::prefab::Queue<DirWrBoth, 1, false, true>;
    MOD(WDirQT, wdir_q);
    // replCM.task 的 FastQueue
    using ReplTaskQT = FastQueue<ReplTask, 2, false>;
    MOD(ReplTaskQT, repl_task_q);

    Backend();
};

}  // namespace zj::dj
