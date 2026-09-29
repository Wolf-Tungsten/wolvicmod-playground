#include <wolvicmod/wolvicmod.h>

#include "model/dj/data.h"

namespace zj::dj {

// ---------------- BeatStorage ----------------

BeatStorage::BeatStorage() {
    array.clk = clk;
    array.clk_en = clk_en;
    resp_pipe.clk = clk;

    // reqReady：5bit 移位里 bit4（d1）无请求 → 请求隔 2 拍（对齐 SRAM interval=2）
    w_req_ready.assign().reads(st) = [](auto src) {
        auto [st] = src;
        return (((st.sftRead | st.sftWrite) >> 4) & 1u) == 0;
    };
    write_rdy.assign().reads(st, w_req_ready) = [](auto src) {
        auto [st, w_req_ready] = src;
        return st.rstDone && w_req_ready;
    };
    read_rdy.assign().reads(st, w_req_ready, write) = [](auto src) {
        auto [st, w_req_ready, write] = src;
        return st.rstDone && w_req_ready && !write.valid;
    };
    w_fires.assign().reads(read, read_rdy, write, write_rdy) = [](auto src) {
        auto [read, read_rdy, write, write_rdy] = src;
        return Fires{read.valid && read_rdy, write.valid && write_rdy};
    };

    // SRAM 请求（valid 用原始 valid——reqReady/rst_done 与 fire 等价，写优先已由
    // read_rdy 保证）
    array.req.assign().reads(read, write, w_req_ready, st) = [](auto src) {
        auto [read, write, w_req_ready, st] = src;
        typename DatRam::ReqBits b;
        b.write = write.valid;
        b.addr = write.valid ? write.bits.ds.idx : read.bits.ds.idx;
        b.data[0] = write.bits.beat;
        return Valid<typename DatRam::ReqBits>{(read.valid || write.valid) && w_req_ready &&
                                                   st.rstDone,
                                               b};
    };

    st.update().on(posedge(clk)).reads(st, w_fires, array.req_rdy) = [](auto src) {
        auto [st, fires, req_rdy] = src;
        St next;
        next.sftRead = static_cast<uint8_t>((fires.rd << 4) | (st.sftRead >> 1));
        next.sftWrite = static_cast<uint8_t>((fires.wr << 4) | (st.sftWrite >> 1));
        next.rstDone = st.rstDone || req_rdy;
        return next;
    };

    resp_pipe.enq.assign().reads(w_fires, read) = [](auto src) {
        auto [fires, read] = src;
        RespInfo i;
        i.dcid = read.bits.dcid;
        i.dbid = read.bits.dbid;
        i.beatNum = read.bits.beatNum;
        i.toCHI = read.bits.toCHI;
        return Valid<RespInfo>{fires.rd, i};
    };

    resp.assign().reads(st, array.resp, resp_pipe.deq) = [](auto src) {
        auto [st, array_resp, resp_pipe_deq] = src;
        DsResp r;
        r.beat = array_resp.bits.data[0];
        r.dcid = resp_pipe_deq.bits.dcid;
        r.dbid = resp_pipe_deq.bits.dbid;
        r.beatNum = resp_pipe_deq.bits.beatNum;
        r.toCHI = resp_pipe_deq.bits.toCHI;
        return Valid<DsResp>{(st.sftRead & 1u) != 0, r};
    };
}

// ---------------- DBIDPool ----------------

DBIDPool::DBIDPool() {
    q0.clk = clk;
    q1.clk = clk;

    // Counter(64).inc() 每拍调用；值 63 的那拍返回真 → done 次拍锁存。
    // clk_en=0（门控冻结期）计数保持——HomeWrapper ICG 冻结预充至 DongJiang 唤醒
    rst.update().on(posedge(clk)).reads(rst, clk_en) = [](auto src) {
        auto [rst, clk_en] = src;
        Rst next;
        next.cnt = !clk_en ? rst.cnt : static_cast<uint8_t>(rst.cnt == 63 ? 0 : rst.cnt + 1);
        next.done = rst.done || (clk_en && rst.cnt == 63);
        return next;
    };

    w_enq_cnt.assign().reads(enq0, enq1) = [](auto src) {
        auto [enq0, enq1] = src;
        return Cnt{enq0.valid != enq1.valid, enq0.valid && enq1.valid};
    };
    w_enq_sel_q0.assign().reads(q0.count, q1.count) = [](auto src) {
        auto [q0_count, q1_count] = src;
        return q0_count <= q1_count;
    };
    // 预充：q0 ← Cat(0,cnt)（0..63），q1 ← Cat(1,cnt)（64..127）
    q0.enq.assign().reads(rst, w_enq_cnt, w_enq_sel_q0, enq0, enq1) = [](auto src) {
        auto [rst, w_enq_cnt, w_enq_sel_q0, enq0, enq1] = src;
        if (!rst.done) return Valid<uint8_t>{true, rst.cnt};
        if (w_enq_cnt.one)
            return Valid<uint8_t>{w_enq_sel_q0, enq0.valid ? enq0.bits : enq1.bits};
        return Valid<uint8_t>{w_enq_cnt.two, enq0.bits};
    };
    q1.enq.assign().reads(rst, w_enq_cnt, w_enq_sel_q0, enq0, enq1) = [](auto src) {
        auto [rst, w_enq_cnt, w_enq_sel_q0, enq0, enq1] = src;
        if (!rst.done) return Valid<uint8_t>{true, static_cast<uint8_t>(0x40 | rst.cnt)};
        if (w_enq_cnt.one)
            return Valid<uint8_t>{!w_enq_sel_q0, enq0.valid ? enq0.bits : enq1.bits};
        return Valid<uint8_t>{w_enq_cnt.two, enq1.bits};
    };

    w_deq_cnt.assign().reads(deq0_rdy, deq1_rdy) = [](auto src) {
        auto [deq0_rdy, deq1_rdy] = src;
        return Cnt{deq0_rdy != deq1_rdy, deq0_rdy && deq1_rdy};
    };
    w_deq_sel_q0.assign().reads(q0.count, q1.count) = [](auto src) {
        auto [q0_count, q1_count] = src;
        return q0_count >= q1_count;
    };
    q0.deq_rdy.assign().reads(w_deq_cnt, w_deq_sel_q0) = [](auto src) {
        auto [w_deq_cnt, w_deq_sel_q0] = src;
        return w_deq_cnt.one ? w_deq_sel_q0 : w_deq_cnt.two;
    };
    q1.deq_rdy.assign().reads(w_deq_cnt, w_deq_sel_q0) = [](auto src) {
        auto [w_deq_cnt, w_deq_sel_q0] = src;
        return w_deq_cnt.one ? !w_deq_sel_q0 : w_deq_cnt.two;
    };
    deq0.assign().reads(q0.deq, q1.deq, w_deq_cnt, w_deq_sel_q0, rst) = [](auto src) {
        auto [q0_deq, q1_deq, w_deq_cnt, w_deq_sel_q0, rst] = src;
        const uint8_t bits = (w_deq_cnt.one && !w_deq_sel_q0) ? q1_deq.bits : q0_deq.bits;
        return Valid<uint8_t>{q0_deq.valid && rst.done, bits};
    };
    deq1.assign().reads(q0.deq, q1.deq, w_deq_cnt, w_deq_sel_q0, rst) = [](auto src) {
        auto [q0_deq, q1_deq, w_deq_cnt, w_deq_sel_q0, rst] = src;
        const uint8_t bits = (w_deq_cnt.one && w_deq_sel_q0) ? q0_deq.bits : q1_deq.bits;
        return Valid<uint8_t>{q1_deq.valid && rst.done, bits};
    };
}

// ---------------- DBIDCtrl ----------------

DBIDCtrl::DBIDCtrl() {
    pool.clk = clk;
    pool.clk_en = clk_en;

    w_has_two.assign().reads(pool.deq0, pool.deq1) = [](auto src) {
        auto [pool_deq0, pool_deq1] = src;
        return pool_deq0.valid && pool_deq1.valid;
    };
    req_rdy.assign().reads(w_has_two) = [](auto src) {
        auto [w_has_two] = src;
        return w_has_two;
    };
    pool.deq0_rdy.assign().reads(req, w_has_two) = [](auto src) {
        auto [req, w_has_two] = src;
        return req.valid && (req.bits & 1u) != 0 && w_has_two;
    };
    pool.deq1_rdy.assign().reads(req, w_has_two) = [](auto src) {
        auto [req, w_has_two] = src;
        return req.valid && ((req.bits >> 1) & 1u) != 0 && w_has_two;
    };
    resp.assign().reads(pool.deq0, pool.deq1) = [](auto src) {
        auto [pool_deq0, pool_deq1] = src;
        return RespArr{pool_deq0.bits, pool_deq1.bits};
    };
    pool.enq0.assign().reads(release) = [](auto src) {
        auto [release] = src;
        return Valid<uint8_t>{release.valid && (release.bits.dataVec & 1u) != 0,
                              release.bits.dbidVec[0]};
    };
    pool.enq1.assign().reads(release) = [](auto src) {
        auto [release] = src;
        return Valid<uint8_t>{release.valid && ((release.bits.dataVec >> 1) & 1u) != 0,
                              release.bits.dbidVec[1]};
    };
}

// ---------------- DataBuffer ----------------

DataBuffer::DataBuffer() {
    dat_buf.clk = clk;
    dat_buf.clk_en = clk_en;
    to_ds_q.clk = clk;
    to_chi_q.clk = clk;

    // ---- 读请求侧（w_has_free_* 内联：空位须多于在途读） ----
    read_to_chi_rdy.assign().reads(to_chi_q.free_num, rd_ctl, read_to_ds) = [](auto src) {
        auto [free_num, rd_ctl, read_to_ds] = src;
        return free_num > __builtin_popcount(rd_ctl.chiSft) && !read_to_ds.valid;
    };
    read_to_ds_rdy.assign().reads(to_ds_q.free_num, rd_ctl) = [](auto src) {
        auto [free_num, rd_ctl] = src;
        return free_num > __builtin_popcount(rd_ctl.dsSft);
    };
    w_rd_chi_fire.assign().reads(read_to_chi, read_to_chi_rdy) = [](auto src) {
        auto [read_to_chi, read_to_chi_rdy] = src;
        return read_to_chi.valid && read_to_chi_rdy;
    };
    w_rd_ds_fire.assign().reads(read_to_ds, read_to_ds_rdy) = [](auto src) {
        auto [read_to_ds, read_to_ds_rdy] = src;
        return read_to_ds.valid && read_to_ds_rdy;
    };
    // 移位 + datBuf 读口请求：rreq = RegNext(fires)，addr = RegEnable(dbid, fires)
    // （DS 优先）——同沿同源，一条 update
    rd_ctl.update().on(posedge(clk)).reads(rd_ctl, w_rd_chi_fire, w_rd_ds_fire, read_to_chi,
                                           read_to_ds) = [](auto src) {
        auto [rd_ctl, w_rd_chi_fire, w_rd_ds_fire, read_to_chi, read_to_ds] = src;
        RdCtl next;
        next.chiSft = static_cast<uint8_t>((w_rd_chi_fire << 1) | (rd_ctl.chiSft >> 1));
        next.dsSft = static_cast<uint8_t>((w_rd_ds_fire << 1) | (rd_ctl.dsSft >> 1));
        next.rreqVal = w_rd_chi_fire || w_rd_ds_fire;
        next.rreqAddr = (w_rd_chi_fire || w_rd_ds_fire)
                            ? (read_to_ds.valid ? read_to_ds.bits.dbid : read_to_chi.bits.dbid)
                            : rd_ctl.rreqAddr;
        return next;
    };
    dat_buf.rreq.assign().reads(rd_ctl) = [](auto src) {
        auto [rd_ctl] = src;
        return Valid<uint32_t>{rd_ctl.rreqVal, static_cast<uint32_t>(rd_ctl.rreqAddr)};
    };

    // repl 跟踪（readToDS 的 repl 读置位，clean 清除）
    repl_vec.update().on(posedge(clk)).reads(repl_vec, w_rd_ds_fire, read_to_ds, clean) =
        [](auto src) {
            auto [repl_vec, w_rd_ds_fire, read_to_ds, clean] = src;
            auto next = repl_vec;
            for (uint32_t i = 0; i < kNrDataBuf; ++i) {
                const bool replHit =
                    w_rd_ds_fire && read_to_ds.bits.repl && read_to_ds.bits.dbid == i;
                bool cleanHit = false;
                for (uint32_t b = 0; b < kNrBeat; ++b)
                    cleanHit = cleanHit || (clean.valid && ((clean.bits.dataVec >> b) & 1u) != 0 &&
                                            clean.bits.dbidVec[b] == i);
                if (replHit) {
                    next[i] = true;
                } else if (cleanHit) {
                    next[i] = false;
                }
            }
            return next;
        };

    // ---- 写口（dsResp 优先） ----
    from_chi_rdy.assign().reads(ds_resp) = [](auto src) {
        auto [ds_resp] = src;
        return !ds_resp.valid;
    };
    // 提交寄存组：wval/dsWri/repl/readOrSnp 为 RegNext（无条件采样），
    // mask/waddr/wdata 以 wriVal 为使能，be 以 from_chi.valid 为使能——同沿一条
    // update；w_wri_val/w_wri_dbid/w_read_or_snp 中转逻辑内联
    wr.update().on(posedge(clk)).reads(wr, ds_resp, from_chi, repl_vec, mask_vec) =
        [](auto src) {
            auto [wr, ds_resp, from_chi, repl_vec, mask_vec] = src;
            WrReg next = wr;
            const bool wriVal = ds_resp.valid || from_chi.valid;
            const uint8_t dbid = ds_resp.valid ? ds_resp.bits.dbid : from_chi.bits.dbid;
            const uint8_t op = from_chi.bits.dat.opcode;
            next.wval = wriVal;
            next.dsWri = ds_resp.valid;
            next.repl = repl_vec[dbid];
            next.readOrSnp = op == dat_op::kSnpRespDataFwded || op == dat_op::kSnpRespData ||
                             op == dat_op::kCompData;
            if (wriVal) {
                next.mask = mask_vec[dbid];
                next.waddr = dbid;
                next.wdata = ds_resp.valid ? beatToBytes(ds_resp.bits.beat)
                                           : beatToBytes(from_chi.bits.dat.data);
            }
            if (from_chi.valid) next.be = from_chi.bits.dat.be;
            return next;
        };
    dat_buf.wreq.assign().reads(wr) = [](auto src) {
        auto [wr] = src;
        typename DatBuf::WrBits b;
        b.addr = wr.waddr;
        const uint32_t full = 0xFFFFFFFFu;
        b.mask = wr.dsWri ? (wr.repl ? full : ~wr.mask)
                          : (wr.readOrSnp ? ~wr.mask : static_cast<uint32_t>(wr.be));
        b.data = wr.wdata;
        return Valid<typename DatBuf::WrBits>{wr.wval, b};
    };

    // mask 跟踪：clean→0；dsResp→全 1；CompData/SnpResp 系→全 1；其余→m|BE
    mask_vec.update().on(posedge(clk)).reads(mask_vec, clean, ds_resp, from_chi, from_chi_rdy) =
        [](auto src) {
            auto [mask_vec, clean, ds_resp, from_chi, from_chi_rdy] = src;
            auto next = mask_vec;
            const uint32_t full = 0xFFFFFFFFu;
            const bool chiFire = from_chi.valid && from_chi_rdy;
            const uint8_t op = from_chi.bits.dat.opcode;
            const bool fullChi = op == dat_op::kCompData || op == dat_op::kSnpRespData ||
                                 op == dat_op::kSnpRespDataFwded;
            for (uint32_t i = 0; i < kNrDataBuf; ++i) {
                bool cleanHit = false;
                for (uint32_t b = 0; b < kNrBeat; ++b)
                    cleanHit = cleanHit || (clean.valid && ((clean.bits.dataVec >> b) & 1u) != 0 &&
                                            clean.bits.dbidVec[b] == i);
                const bool dsHit = ds_resp.valid && ds_resp.bits.dbid == i;
                const bool chiHit = chiFire && from_chi.bits.dbid == i;
                if (cleanHit) {
                    next[i] = 0;
                } else if (dsHit) {
                    next[i] = full;
                } else if (chiHit) {
                    next[i] = fullChi ? full : (next[i] | static_cast<uint32_t>(from_chi.bits.dat.be));
                }
            }
            return next;
        };

    // ---- 读数据回送队列（fire+2 拍 enq，对齐 rresp） ----
    // d1 以 fire 为使能采样读请求，d2 以 enD1(sft) 为使能跟移 d1——同沿一条 update
    const auto enD1 = [](uint8_t sft) { return ((sft >> 1) & 1u) != 0; };
    chi_pipe.update().on(posedge(clk)).reads(chi_pipe, w_rd_chi_fire, read_to_chi, rd_ctl) =
        [=](auto src) {
            auto [p, w_rd_chi_fire, read_to_chi, rd_ctl] = src;
            ChiPipe next = p;
            if (w_rd_chi_fire) {
                next.dbidD1 = read_to_chi.bits.dbid;
                next.beatD1 = read_to_chi.bits.beatNum;
                next.dcidD1 = read_to_chi.bits.dcid;
            }
            if (enD1(rd_ctl.chiSft)) {
                next.dbidD2 = p.dbidD1;
                next.beatD2 = p.beatD1;
                next.dcidD2 = p.dcidD1;
            }
            return next;
        };
    ds_pipe.update().on(posedge(clk)).reads(ds_pipe, w_rd_ds_fire, read_to_ds, rd_ctl) =
        [=](auto src) {
            auto [p, w_rd_ds_fire, read_to_ds, rd_ctl] = src;
            DsPipe next = p;
            if (w_rd_ds_fire) {
                next.dcidD1 = read_to_ds.bits.dcid;
                next.beatD1 = read_to_ds.bits.beatNum;
                next.dsD1 = read_to_ds.bits.ds;
            }
            if (enD1(rd_ctl.dsSft)) {
                next.dcidD2 = p.dcidD1;
                next.beatD2 = p.beatD1;
                next.dsD2 = p.dsD1;
            }
            return next;
        };

    to_chi_q.enq.assign().reads(rd_ctl, chi_pipe, mask_vec, dat_buf.rresp) = [](auto src) {
        auto [rd_ctl, p, mask_vec, rresp] = src;
        ToCHIEntry e;
        e.dcid = p.dcidD2;
        e.beatNum = p.beatD2;
        e.dat.be = mask_vec[p.dbidD2];
        e.dat.data = bytesToBeat(rresp.bits.data);
        e.dat.data_id = static_cast<uint8_t>(p.beatD2 << 1);
        return Valid<ToCHIEntry>{(rd_ctl.chiSft & 1u) != 0, e};
    };
    to_ds_q.enq.assign().reads(rd_ctl, ds_pipe, dat_buf.rresp) = [](auto src) {
        auto [rd_ctl, p, rresp] = src;
        WriteDS w;
        w.ds = p.dsD2;
        w.dcid = p.dcidD2;
        w.beatNum = p.beatD2;
        w.beat = bytesToBeat(rresp.bits.data);
        return Valid<WriteDS>{(rd_ctl.dsSft & 1u) != 0, w};
    };

    to_chi = to_chi_q.deq;
    to_chi_q.deq_rdy = to_chi_rdy;
    write_ds = to_ds_q.deq;
    to_ds_q.deq_rdy = write_ds_rdy;
}

}  // namespace zj::dj
