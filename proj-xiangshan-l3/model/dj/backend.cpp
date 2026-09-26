#include <wolvicmod/wolvicmod.h>

#include "model/dj/backend.h"

namespace zj::dj {

Backend::Backend() {
    commit.clk = clk;
    repl_cm.clk = clk;
    snoop_cm.clk = clk;
    write_cm.clk = clk;
    read_cm.clk = clk;
    cmt_pipe_0.clk = clk;
    cmt_pipe_1.clk = clk;
    tx_req_arb.clk = clk;
    tx_req_q.clk = clk;
    tx_rsp_arb.clk = clk;
    fast_resp_q.clk = clk;
    cmt_dt_q.clk = clk;
    repl_dt_q.clk = clk;
    wri_dt_q.clk = clk;
    data_task_arb.clk = clk;
    req_db_arb.clk = clk;
    cm_resp_arb.clk = clk;
    cm_resp_pipe.clk = clk;
    cmt_snp_q.clk = clk;
    repl_snp_q.clk = clk;
    snp_alloc_arb.clk = clk;
    cmt_wri_q.clk = clk;
    repl_wri_q.clk = clk;
    wri_alloc_arb.clk = clk;
    cmt_read_q.clk = clk;
    read_alloc_arb.clk = clk;
    clean_arb.clk = clk;
    clean_q.clk = clk;
    wdir_q.clk = clk;
    repl_task_q.clk = clk;

    commit.cfg_ci = cfg_ci;
    commit.cfg_bank_id = cfg_bank_id;
    repl_cm.cfg_ci = cfg_ci;
    snoop_cm.cfg_ci = cfg_ci;
    write_cm.cfg_ci = cfg_ci;
    read_cm.cfg_ci = cfg_ci;

    rx_rsp_rdy = true;

    // ---- cmtTaskVec Pipe(1) ----
    cmt_pipe_0.enq = cmt_task_0;
    cmt_pipe_1.enq = cmt_task_1;
    commit.cmt_task_0 = cmt_pipe_0.deq;
    commit.cmt_task_1 = cmt_pipe_1.deq;

    // ---- txReq：read/write QosRR → FastQueue → Addr 重写 ----
    tx_req_arb.in.assign().reads(read_cm.tx_req, write_cm.tx_req) = [](auto src) {
        auto [r, w] = src;
        std::array<Valid<HReqFlit>, 2> a;
        a[0] = r;
        a[1] = w;
        return a;
    };
    tx_req_q.enq = tx_req_arb.out;
    tx_req_arb.out_rdy = tx_req_q.enq_rdy;
    read_cm.tx_req_rdy.assign().reads(tx_req_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[0];
    };
    write_cm.tx_req_rdy.assign().reads(tx_req_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[1];
    };
    get_addr_0_hnidx.assign().reads(tx_req_q.deq) = [](auto src) {
        auto [deq] = src;
        return static_cast<uint8_t>(deq.bits.txn_id & 0x7F);
    };
    tx_req.assign().reads(tx_req_q.deq, get_addr_0_result) = [](auto src) {
        auto [deq, result] = src;
        HReqFlit f = deq.bits;
        const bool cacheable = (deq.bits.mem_attr >> 2) & 1;
        if (!cacheable) {
            f.addr = result;
        } else if (deq.bits.size == 6) {
            f.addr = result & ~0x3Full;
        } else {
            f.addr = result & ~0x1Full;
        }
        return Valid<HReqFlit>{deq.valid, f};
    };
    tx_req_q.deq_rdy = tx_req_rdy;

    // ---- txSnp：直连 + Addr 重写 ----
    get_addr_1_hnidx.assign().reads(snoop_cm.tx_snp) = [](auto src) {
        auto [tx_snp] = src;
        return static_cast<uint8_t>(tx_snp.bits.txn_id & 0x7F);
    };
    tx_snp.assign().reads(snoop_cm.tx_snp, get_addr_1_result) = [](auto src) {
        auto [tx_snp, result] = src;
        SnoopFlit f = tx_snp.bits;
        f.addr = (result >> 6) << 3;
        return Valid<SnoopFlit>{tx_snp.valid, f};
    };
    snoop_cm.tx_snp_rdy = tx_snp_rdy;

    // ---- txRsp：commit.txRsp vs FastQueue(fastResp) ----
    fast_resp_q.enq = fast_resp;
    fast_resp_rdy = fast_resp_q.enq_rdy;
    tx_rsp_arb.in.assign().reads(commit.tx_rsp, fast_resp_q.deq) = [](auto src) {
        auto [c, f] = src;
        std::array<Valid<RespFlit>, 2> a;
        a[0] = c;
        a[1] = f;
        return a;
    };
    tx_rsp_arb.out_rdy = tx_rsp_rdy;
    tx_rsp = tx_rsp_arb.out;
    commit.tx_rsp_rdy.assign().reads(tx_rsp_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[0];
    };
    fast_resp_q.deq_rdy.assign().reads(tx_rsp_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[1];
    };

    // ---- commit ↔ 广播/回响应 ----
    commit.rx_rsp = rx_rsp;
    commit.rx_dat = rx_dat;
    commit.repl_resp = repl_cm.resp;
    commit.data_resp = data_resp;
    // CM 池的 rx 广播
    snoop_cm.rx_rsp = rx_rsp;
    snoop_cm.rx_dat = rx_dat;
    read_cm.rx_dat = rx_dat;
    write_cm.rx_rsp = rx_rsp;
    write_cm.data_resp = data_resp;

    // ---- reqPosVec2 / updPosTag / updHnTxnID 直连 ----
    req_pos_vec = repl_cm.req_pos_vec;
    repl_cm.pos_resp_vec = pos_resp_vec;
    upd_pos_tag = repl_cm.upd_pos_tag;
    upd_hn_txn_id = repl_cm.upd_hn_txn_id;

    // ---- replTask → FastQueue → replCM.task ----
    repl_task_q.enq = commit.repl_task;
    commit.repl_task_rdy = repl_task_q.enq_rdy;
    repl_cm.task = repl_task_q.deq;
    repl_task_q.deq_rdy = repl_cm.task_rdy;

    // ---- replCM 边界 ----
    repl_cm.resp_dir_llc = resp_dir_llc;
    repl_cm.resp_dir_sf = resp_dir_sf;
    repl_cm.data_resp = data_resp;

    // ---- cmResp 合流 + Pipe：toRepl=0→commit、=1→repl ----
    cm_resp_arb.in.assign().reads(snoop_cm.resp, read_cm.resp, write_cm.resp) = [](auto src) {
        auto [s, r, w] = src;
        std::array<Valid<CMResp>, 3> a;
        a[0] = s;
        a[1] = r;
        a[2] = w;
        return a;
    };
    cm_resp_arb.out_rdy = true;
    cm_resp_pipe.enq = cm_resp_arb.out;
    snoop_cm.resp_rdy.assign().reads(cm_resp_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[0];
    };
    read_cm.resp_rdy.assign().reads(cm_resp_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[1];
    };
    write_cm.resp_rdy.assign().reads(cm_resp_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[2];
    };
    commit.cm_resp.assign().reads(cm_resp_pipe.deq) = [](auto src) {
        auto [deq] = src;
        return Valid<CMResp>{deq.valid && !deq.bits.toRepl, deq.bits};
    };
    repl_cm.cm_resp.assign().reads(cm_resp_pipe.deq) = [](auto src) {
        auto [deq] = src;
        return Valid<CMResp>{deq.valid && deq.bits.toRepl, deq.bits};
    };

    // ---- writeDir：Queue(1, pipe) + addr 重写 + writeDirDone ----
    get_addr_2_hnidx.assign().reads(repl_cm.write_dir) = [](auto src) {
        auto [write_dir] = src;
        return static_cast<uint8_t>(write_dir.bits.llc.addr & 0x7F);
    };
    wdir_q.enq.assign().reads(repl_cm.write_dir, get_addr_2_result) = [](auto src) {
        auto [write_dir, result] = src;
        DirWrBoth w = write_dir.bits;
        w.llc.addr = result;
        w.sf.addr = result;
        return Valid<DirWrBoth>{write_dir.valid, w};
    };
    repl_cm.write_dir_rdy = wdir_q.enq_rdy;
    write_dir = wdir_q.deq;
    wdir_q.deq_rdy = write_dir_rdy;
    repl_cm.write_dir_done.assign().reads(write_dir, write_dir_rdy) = [](auto src) {
        auto [write_dir, write_dir_rdy] = src;
        const bool fire = write_dir.valid && write_dir_rdy;
        const bool da = write_dir.bits.sfValid && write_dir.bits.sf.directAlloc;
        return Valid<uint8_t>{fire && da, write_dir.bits.sf.hnIdx};
    };

    // ---- reqDB：repl 固定优先 ----
    req_db_arb.in.assign().reads(repl_cm.req_db, commit.req_db) = [](auto src) {
        auto [r, c] = src;
        std::array<Valid<ReqDB>, 2> a;
        a[0] = r;
        a[1] = c;
        return a;
    };
    req_db_arb.out_rdy = req_db_rdy;
    req_db = req_db_arb.out;
    repl_cm.req_db_rdy.assign().reads(req_db_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[0];
    };
    commit.req_db_rdy.assign().reads(req_db_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[1];
    };

    // ---- dataTask 三路 ----
    cmt_dt_q.enq = commit.data_task;
    commit.data_task_rdy = cmt_dt_q.enq_rdy;
    repl_dt_q.enq = repl_cm.data_task;
    repl_cm.data_task_rdy = repl_dt_q.enq_rdy;
    wri_dt_q.enq = write_cm.data_task;
    write_cm.data_task_rdy = wri_dt_q.enq_rdy;
    data_task_arb.in.assign().reads(cmt_dt_q.deq, repl_dt_q.deq, wri_dt_q.deq) = [](auto src) {
        auto [c, r, w] = src;
        std::array<Valid<DataTask>, 3> a;
        a[0] = c;
        a[1] = r;
        a[2] = w;
        return a;
    };
    data_task_arb.out_rdy = data_task_rdy;
    data_task = data_task_arb.out;
    cmt_dt_q.deq_rdy.assign().reads(data_task_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[0];
    };
    repl_dt_q.deq_rdy.assign().reads(data_task_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[1];
    };
    wri_dt_q.deq_rdy.assign().reads(data_task_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[2];
    };

    // ---- cmTask alloc 三路 ----
    cmt_snp_q.enq = commit.cm_task_snp;
    commit.cm_task_snp_rdy = cmt_snp_q.enq_rdy;
    repl_snp_q.enq = repl_cm.cm_task_snp;
    repl_cm.cm_task_snp_rdy = repl_snp_q.enq_rdy;
    snp_alloc_arb.in.assign().reads(cmt_snp_q.deq, repl_snp_q.deq) = [](auto src) {
        auto [c, r] = src;
        std::array<Valid<CMTask>, 2> a;
        a[0] = c;
        a[1] = r;
        return a;
    };
    snoop_cm.alloc = snp_alloc_arb.out;
    snp_alloc_arb.out_rdy = snoop_cm.alloc_rdy;
    cmt_snp_q.deq_rdy.assign().reads(snp_alloc_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[0];
    };
    repl_snp_q.deq_rdy.assign().reads(snp_alloc_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[1];
    };

    cmt_wri_q.enq = commit.cm_task_wri;
    commit.cm_task_wri_rdy = cmt_wri_q.enq_rdy;
    repl_wri_q.enq = repl_cm.cm_task_wri;
    repl_cm.cm_task_wri_rdy = repl_wri_q.enq_rdy;
    wri_alloc_arb.in.assign().reads(cmt_wri_q.deq, repl_wri_q.deq) = [](auto src) {
        auto [c, r] = src;
        std::array<Valid<CMTask>, 2> a;
        a[0] = c;
        a[1] = r;
        return a;
    };
    write_cm.alloc = wri_alloc_arb.out;
    wri_alloc_arb.out_rdy = write_cm.alloc_rdy;
    cmt_wri_q.deq_rdy.assign().reads(wri_alloc_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[0];
    };
    repl_wri_q.deq_rdy.assign().reads(wri_alloc_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[1];
    };

    cmt_read_q.enq = commit.cm_task_read;
    commit.cm_task_read_rdy = cmt_read_q.enq_rdy;
    read_alloc_arb.in.assign().reads(cmt_read_q.deq) = [](auto src) {
        auto [c] = src;
        std::array<Valid<CMTask>, 1> a;
        a[0] = c;
        return a;
    };
    read_cm.alloc = read_alloc_arb.out;
    read_alloc_arb.out_rdy = read_cm.alloc_rdy;
    cmt_read_q.deq_rdy.assign().reads(read_alloc_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[0];
    };

    // ---- cleanPoS 三分叉（io.cleanPoS / unlock / cleanDB） ----
    clean_arb.in.assign().reads(commit.clean_pos, repl_cm.clean_pos) = [](auto src) {
        auto [c, r] = src;
        std::array<Valid<PosClean>, 2> a;
        a[0] = c;
        a[1] = r;
        return a;
    };
    clean_q.enq = clean_arb.out;
    clean_arb.out_rdy = clean_q.enq_rdy;
    commit.clean_pos_rdy.assign().reads(clean_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[0];
    };
    repl_cm.clean_pos_rdy.assign().reads(clean_arb.in_rdy) = [](auto src) {
        auto [rdy] = src;
        return rdy[1];
    };
    clean_q.deq_rdy = clean_db_rdy;
    clean_pos.assign().reads(clean_q.deq, clean_db_rdy) = [](auto src) {
        auto [deq, clean_db_rdy] = src;
        return Valid<PosClean>{deq.valid && clean_db_rdy, deq.bits};
    };
    unlock.assign().reads(clean_q.deq, clean_db_rdy) = [](auto src) {
        auto [deq, clean_db_rdy] = src;
        return Valid<uint8_t>{deq.valid && clean_db_rdy, deq.bits.hnIdx};
    };
    clean_db.assign().reads(clean_q.deq) = [](auto src) {
        auto [deq] = src;
        return Valid<ReqDB>{deq.valid, {deq.bits.hnIdx, 0x3}};
    };
}

}  // namespace zj::dj
