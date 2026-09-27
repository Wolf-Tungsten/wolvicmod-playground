#include "model/home/dongjiang.h"

namespace zj::home {

// ReqFlit(false) → ReqFlit(true)：基字段直拷，DMT 的 Return* 置 0（rx 侧无此字段）
static HReqFlit toHReq(const RReqFlit& r) {
    HReqFlit h;
    h.exp_comp_ack = r.exp_comp_ack;
    h.excl = r.excl;
    h.snp_attr = r.snp_attr;
    h.mem_attr = r.mem_attr;
    h.order = r.order;
    h.addr = r.addr;
    h.size = r.size;
    h.opcode = r.opcode;
    h.txn_id = r.txn_id;
    h.src_id = r.src_id;
    h.tgt_id = r.tgt_id;
    h.qos = r.qos;
    return h;
}

DongJiang::DongJiang() {
    // ---- clk / config ----
    fe0.clk = clk;
    fe1.clk = clk;
    backend.clk = clk;
    directory.clk = clk;
    datablock.clk = clk;
    chixbar.clk = clk;
    fast_resp_arb.clk = clk;
    reqdb_s3_arb.clk = clk;
    reqdb_s1_arb.clk = clk;
    cleandb_in_arb.clk = clk;
    reqdb_arb.clk = clk;
    cleandb_arb.clk = clk;
    task_in_arb.clk = clk;
    task_arb.clk = clk;

    fe0.cfg_ci = ci;
    fe1.cfg_ci = ci;
    backend.cfg_ci = ci;
    fe0.cfg_bank_id = bank_id;
    fe1.cfg_bank_id = bank_id;
    backend.cfg_bank_id = bank_id;
    directory.cfg_bank_id = bank_id;
    fe0.dir_bank = static_cast<uint8_t>(0);
    fe1.dir_bank = static_cast<uint8_t>(1);

    // ---- lan rx（setRx：tgt := LAN(0)）----
    chixbar.rx_req_in.assign().reads(hnx_rx_req) = [](auto src) {
        auto [v] = src;
        Valid<RReqFlit> o{v.valid, v.bits};
        o.bits.tgt_id = 0;
        return o;
    };
    hnx_rx_req_rdy = chixbar.rx_req_in_rdy;
    backend.rx_rsp.assign().reads(hnx_rx_resp) = [](auto src) {
        auto [v] = src;
        Valid<RespFlit> o{v.valid, v.bits};
        o.bits.tgt_id = 0;
        return o;
    };
    hnx_rx_resp_rdy = backend.rx_rsp_rdy;
    datablock.rx_dat.assign().reads(hnx_rx_data) = [](auto src) {
        auto [v] = src;
        Valid<DataFlit> o{v.valid, v.bits};
        o.bits.tgt_id = 0;
        return o;
    };
    hnx_rx_data_rdy = datablock.rx_dat_rdy;
    // backend.rxDat：valid := rxDat fire（DataBlock 消费拍抄送）
    backend.rx_dat.assign().reads(hnx_rx_data, datablock.rx_dat_rdy) = [](auto src) {
        auto [v, rdy] = src;
        Valid<DataFlit> o;
        o.valid = v.valid && rdy;
        o.bits = v.bits;
        o.bits.tgt_id = 0;
        return o;
    };

    // ---- ChiXbar ↔ 2×Frontend（rx req/hpr）----
    fe0.rx_req.assign().reads(chixbar.rx_req_out) = [](auto src) {
        auto [o] = src;
        return Valid<HReqFlit>{o[0].valid, toHReq(o[0].bits)};
    };
    fe1.rx_req.assign().reads(chixbar.rx_req_out) = [](auto src) {
        auto [o] = src;
        return Valid<HReqFlit>{o[1].valid, toHReq(o[1].bits)};
    };
    chixbar.rx_req_out_rdy.assign().reads(fe0.rx_req_rdy, fe1.rx_req_rdy) = [](auto src) {
        auto [a, b] = src;
        return A2Bool{a, b};
    };
    fe0.rx_hpr.assign().reads(chixbar.rx_hpr_out) = [](auto src) {
        auto [o] = src;
        return Valid<HReqFlit>{o[0].valid, toHReq(o[0].bits)};
    };
    fe1.rx_hpr.assign().reads(chixbar.rx_hpr_out) = [](auto src) {
        auto [o] = src;
        return Valid<HReqFlit>{o[1].valid, toHReq(o[1].bits)};
    };
    chixbar.rx_hpr_out_rdy.assign().reads(fe0.rx_hpr_rdy, fe1.rx_hpr_rdy) = [](auto src) {
        auto [a, b] = src;
        return A2Bool{a, b};
    };

    // ---- tx（Backend/DataBlock → ChiXbar → lan）----
    chixbar.tx_req_in = backend.tx_req;
    backend.tx_req_rdy = chixbar.tx_req_in_rdy;
    chixbar.tx_snp_in = backend.tx_snp;
    backend.tx_snp_rdy = chixbar.tx_snp_in_rdy;
    chixbar.tx_rsp_in = backend.tx_rsp;
    backend.tx_rsp_rdy = chixbar.tx_rsp_in_rdy;
    chixbar.tx_dat_in = datablock.tx_dat;
    datablock.tx_dat_rdy = chixbar.tx_dat_in_rdy;
    hnx_tx_erq = chixbar.tx_req_out;
    chixbar.tx_req_out_rdy = hnx_tx_erq_rdy;
    hnx_tx_snoop = chixbar.tx_snp_out;
    chixbar.tx_snp_out_rdy = hnx_tx_snoop_rdy;
    hnx_tx_resp = chixbar.tx_rsp_out;
    chixbar.tx_rsp_out_rdy = hnx_tx_resp_rdy;
    hnx_tx_data = chixbar.tx_dat_out;
    chixbar.tx_dat_out_rdy = hnx_tx_data_rdy;
    chixbar.c_busy = cbusy_reg;

    // ---- cBusy = RegNext({0, posBusy})：alrUsePoS 合计分档（nrPoS=128）----
    cbusy_reg.update().on(posedge(clk)).reads(fe0.alr_use_pos, fe1.alr_use_pos) =
        [](auto src) -> uint8_t {
            auto [a, b] = src;
            const uint32_t alr = static_cast<uint32_t>(a) + b;
            if (alr < 64) return 0;
            if (alr < 96) return 1;
            if (alr < 115) return 2;
            return 3;
        };

    // ---- working：10 级移位或 ----
    work_sft.update().on(posedge(clk)).reads(fe0.working, fe1.working, work_sft) =
        [](auto src) -> uint16_t {
            auto [w0, w1, sft] = src;
            return static_cast<uint16_t>(((w0 || w1) << 9) | (sft >> 1));
        };
    working.assign().reads(work_sft) = [](auto src) {
        auto [sft] = src;
        return sft != 0;
    };

    // ---- Frontend ↔ Directory ----
    directory.read_0 = fe0.read_dir;
    fe0.read_dir_rdy = directory.read_0_rdy;
    directory.read_1 = fe1.read_dir;
    fe1.read_dir_rdy = directory.read_1_rdy;
    fe0.resp_dir = directory.rresp_0;
    fe1.resp_dir = directory.rresp_1;
    directory.write = backend.write_dir;
    backend.write_dir_rdy = directory.write_rdy;
    backend.resp_dir_llc = directory.wresp_llc;
    backend.resp_dir_sf = directory.wresp_sf;
    directory.unlock = backend.unlock;

    // ---- Frontend ↔ Backend ----
    backend.cmt_task_0 = fe0.cmt_task;
    backend.cmt_task_1 = fe1.cmt_task;
    fe0.req_pos_vec.assign().reads(backend.req_pos_vec) = [](auto src) {
        auto [v] = src;
        return v[0];
    };
    fe1.req_pos_vec.assign().reads(backend.req_pos_vec) = [](auto src) {
        auto [v] = src;
        return v[1];
    };
    backend.pos_resp_vec.assign().reads(fe0.pos_resp_vec, fe1.pos_resp_vec) = [](auto src) {
        auto [a, b] = src;
        return dj::Backend::PosRespArr{a, b};
    };
    fe0.upd_pos_tag = backend.upd_pos_tag;
    fe1.upd_pos_tag = backend.upd_pos_tag;
    fe0.clean_pos = backend.clean_pos;
    fe1.clean_pos = backend.clean_pos;
    // getAddrVec：hnIdx 广播两 FE；result 按 hnIdx.dirBank 选择
    fe0.get_addr_hnidx.assign().reads(backend.get_addr_0_hnidx, backend.get_addr_1_hnidx,
                                      backend.get_addr_2_hnidx) = [](auto src) {
        auto [a, b, c] = src;
        return U8x3{a, b, c};
    };
    fe1.get_addr_hnidx.assign().reads(backend.get_addr_0_hnidx, backend.get_addr_1_hnidx,
                                      backend.get_addr_2_hnidx) = [](auto src) {
        auto [a, b, c] = src;
        return U8x3{a, b, c};
    };
    backend.get_addr_0_result.assign().reads(backend.get_addr_0_hnidx, fe0.get_addr_result,
                                             fe1.get_addr_result) = [](auto src) {
        auto [h, r0, r1] = src;
        return dj::hnIdxDirBank(h) ? r1[0] : r0[0];
    };
    backend.get_addr_1_result.assign().reads(backend.get_addr_1_hnidx, fe0.get_addr_result,
                                             fe1.get_addr_result) = [](auto src) {
        auto [h, r0, r1] = src;
        return dj::hnIdxDirBank(h) ? r1[1] : r0[1];
    };
    backend.get_addr_2_result.assign().reads(backend.get_addr_2_hnidx, fe0.get_addr_result,
                                             fe1.get_addr_result) = [](auto src) {
        auto [h, r0, r1] = src;
        return dj::hnIdxDirBank(h) ? r1[2] : r0[2];
    };

    // ---- fastResp 合流：fastRRArb(fe.fastResp) → backend.fastResp ----
    fast_resp_arb.in.assign().reads(fe0.fast_resp, fe1.fast_resp) = [](auto src) {
        auto [a, b] = src;
        return A2VRsp{a, b};
    };
    backend.fast_resp = fast_resp_arb.out;
    fast_resp_arb.out_rdy = backend.fast_resp_rdy;
    fe0.fast_resp_rdy.assign().reads(fast_resp_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[0];
    };
    fe1.fast_resp_rdy.assign().reads(fast_resp_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[1];
    };

    // ---- reqDB：fastArb(backend.reqDB, fastRRArb(fe.reqDB_s3), fastRRArb(fe.reqDB_s1)) ----
    reqdb_s3_arb.in.assign().reads(fe0.req_db_s3, fe1.req_db_s3) = [](auto src) {
        auto [a, b] = src;
        return A2VReq{a, b};
    };
    fe0.req_db_s3_rdy.assign().reads(reqdb_s3_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[0];
    };
    fe1.req_db_s3_rdy.assign().reads(reqdb_s3_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[1];
    };
    reqdb_s1_arb.in.assign().reads(fe0.req_db_s1, fe1.req_db_s1) = [](auto src) {
        auto [a, b] = src;
        return A2VReq{a, b};
    };
    fe0.req_db_s1_rdy.assign().reads(reqdb_s1_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[0];
    };
    fe1.req_db_s1_rdy.assign().reads(reqdb_s1_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[1];
    };
    reqdb_arb.in.assign().reads(backend.req_db, reqdb_s3_arb.out, reqdb_s1_arb.out) =
        [](auto src) {
            auto [a, b, c] = src;
            return A3VReq{a, b, c};
        };
    datablock.req_db = reqdb_arb.out;
    reqdb_arb.out_rdy = datablock.req_db_rdy;
    backend.req_db_rdy.assign().reads(reqdb_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[0];
    };
    reqdb_s3_arb.out_rdy.assign().reads(reqdb_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[1];
    };
    reqdb_s1_arb.out_rdy.assign().reads(reqdb_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[2];
    };

    // ---- cleanDB：fastArb.validOut(fastRRArb(fe.cleanDB), backend.cleanDB) ----
    cleandb_in_arb.in.assign().reads(fe0.clean_db_s3, fe1.clean_db_s3) = [](auto src) {
        auto [a, b] = src;
        return A2VReq{a, b};
    };
    cleandb_arb.in.assign().reads(cleandb_in_arb.out, backend.clean_db) = [](auto src) {
        auto [a, b] = src;
        return A2VReq{a, b};
    };
    cleandb_in_arb.out_rdy.assign().reads(cleandb_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[0];
    };
    backend.clean_db_rdy.assign().reads(cleandb_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[1];
    };
    cleandb_arb.out_rdy = true;  // validOut：out.ready := true.B
    datablock.clean_db.assign().reads(cleandb_arb.out) = [](auto src) {
        auto [o] = src;
        Valid<dj::CleanBits> c;
        c.valid = o.valid;
        c.bits.hnTxnID = o.bits.hnTxnID;
        c.bits.dataVec = o.bits.dataVec;
        return c;
    };

    // ---- dataTask：fastArb.validOut(backend.dataTask, fastRRArb(fe.fastData)) ----
    task_in_arb.in.assign().reads(fe0.fast_data_s3, fe1.fast_data_s3) = [](auto src) {
        auto [a, b] = src;
        return A2VTask{a, b};
    };
    fe0.fast_data_s3_rdy.assign().reads(task_in_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[0];
    };
    fe1.fast_data_s3_rdy.assign().reads(task_in_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[1];
    };
    task_arb.in.assign().reads(backend.data_task, task_in_arb.out) = [](auto src) {
        auto [a, b] = src;
        return A2VTask{a, b};
    };
    backend.data_task_rdy.assign().reads(task_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[0];
    };
    task_in_arb.out_rdy.assign().reads(task_arb.in_rdy) = [](auto src) {
        auto [r] = src;
        return r[1];
    };
    task_arb.out_rdy = true;  // validOut
    datablock.task = task_arb.out;

    // ---- Backend ↔ DataBlock 直连 ----
    datablock.upd_hn_txn_id = backend.upd_hn_txn_id;
    backend.data_resp = datablock.resp;
}

}  // namespace zj::home
