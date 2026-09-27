#include <wolvicmod/wolvicmod.h>

#include "model/dj/data.h"

namespace zj::dj {

// ---------------- BeatStorage ----------------

BeatStorage::BeatStorage() {
    array.clk = clk;
    array.clk_en = clk_en;
    resp_pipe.clk = clk;

    // reqReady：5bit 移位里 bit4（d1）无请求 → 请求隔 2 拍（对齐 SRAM interval=2）
    w_req_ready.assign().reads(sft_read, sft_write) = [](auto src) {
        auto [sft_read, sft_write] = src;
        return (((sft_read | sft_write) >> 4) & 1u) == 0;
    };
    write_rdy.assign().reads(rst_done, w_req_ready) = [](auto src) {
        auto [rst_done, w_req_ready] = src;
        return rst_done && w_req_ready;
    };
    read_rdy.assign().reads(rst_done, w_req_ready, write) = [](auto src) {
        auto [rst_done, w_req_ready, write] = src;
        return rst_done && w_req_ready && !write.valid;
    };
    w_write_fire.assign().reads(write, write_rdy) = [](auto src) {
        auto [write, write_rdy] = src;
        return write.valid && write_rdy;
    };
    w_read_fire.assign().reads(read, read_rdy) = [](auto src) {
        auto [read, read_rdy] = src;
        return read.valid && read_rdy;
    };

    // SRAM 请求（valid 用原始 valid——reqReady/rst_done 与 fire 等价，写优先已由
    // read_rdy 保证）
    array.req.assign().reads(read, write, w_req_ready, rst_done) = [](auto src) {
        auto [read, write, w_req_ready, rst_done] = src;
        typename DatRam::ReqBits b;
        b.write = write.valid;
        b.addr = write.valid ? write.bits.ds.idx : read.bits.ds.idx;
        b.data[0] = write.bits.beat;
        return Valid<typename DatRam::ReqBits>{(read.valid || write.valid) && w_req_ready &&
                                                   rst_done,
                                               b};
    };

    sft_read.update().on(posedge(clk)).reads(sft_read, w_read_fire) = [](auto src) {
        auto [sft_read, w_read_fire] = src;
        return static_cast<uint8_t>((w_read_fire << 4) | (sft_read >> 1));
    };
    sft_write.update().on(posedge(clk)).reads(sft_write, w_write_fire) = [](auto src) {
        auto [sft_write, w_write_fire] = src;
        return static_cast<uint8_t>((w_write_fire << 4) | (sft_write >> 1));
    };
    rst_done.update().on(posedge(clk)).reads(rst_done, array.req_rdy) = [](auto src) {
        auto [rst_done, array_req_rdy] = src;
        return rst_done || array_req_rdy;
    };

    resp_pipe.enq.assign().reads(w_read_fire, read) = [](auto src) {
        auto [w_read_fire, read] = src;
        RespInfo i;
        i.dcid = read.bits.dcid;
        i.dbid = read.bits.dbid;
        i.beatNum = read.bits.beatNum;
        i.toCHI = read.bits.toCHI;
        return Valid<RespInfo>{w_read_fire, i};
    };

    resp.assign().reads(sft_read, array.resp, resp_pipe.deq) = [](auto src) {
        auto [sft_read, array_resp, resp_pipe_deq] = src;
        DsResp r;
        r.beat = array_resp.bits.data[0];
        r.dcid = resp_pipe_deq.bits.dcid;
        r.dbid = resp_pipe_deq.bits.dbid;
        r.beatNum = resp_pipe_deq.bits.beatNum;
        r.toCHI = resp_pipe_deq.bits.toCHI;
        return Valid<DsResp>{(sft_read & 1u) != 0, r};
    };
}

// ---------------- DBIDPool ----------------

DBIDPool::DBIDPool() {
    q0.clk = clk;
    q1.clk = clk;

    // Counter(64).inc() 每拍调用；值 63 的那拍返回真 → rst_done 次拍锁存。
    // clk_en=0（门控冻结期）计数保持——HomeWrapper ICG 冻结预充至 DongJiang 唤醒
    rst_cnt.update().on(posedge(clk)).reads(rst_cnt, clk_en) = [](auto src) {
        auto [rst_cnt, clk_en] = src;
        if (!clk_en) return rst_cnt;
        return static_cast<uint8_t>(rst_cnt == 63 ? 0 : rst_cnt + 1);
    };
    rst_done.update().on(posedge(clk)).reads(rst_done, rst_cnt, clk_en) = [](auto src) {
        auto [rst_done, rst_cnt, clk_en] = src;
        return rst_done || (clk_en && rst_cnt == 63);
    };

    w_enq_one.assign().reads(enq0, enq1) = [](auto src) {
        auto [enq0, enq1] = src;
        return enq0.valid != enq1.valid;
    };
    w_enq_two.assign().reads(enq0, enq1) = [](auto src) {
        auto [enq0, enq1] = src;
        return enq0.valid && enq1.valid;
    };
    w_enq_sel_q0.assign().reads(q0.count, q1.count) = [](auto src) {
        auto [q0_count, q1_count] = src;
        return q0_count <= q1_count;
    };
    // 预充：q0 ← Cat(0,cnt)（0..63），q1 ← Cat(1,cnt)（64..127）
    q0.enq.assign().reads(rst_done, rst_cnt, w_enq_one, w_enq_two, w_enq_sel_q0, enq0, enq1) =
        [](auto src) {
            auto [rst_done, rst_cnt, w_enq_one, w_enq_two, w_enq_sel_q0, enq0, enq1] = src;
            if (!rst_done) return Valid<uint8_t>{true, rst_cnt};
            if (w_enq_one) return Valid<uint8_t>{w_enq_sel_q0, enq0.valid ? enq0.bits : enq1.bits};
            return Valid<uint8_t>{w_enq_two, enq0.bits};
        };
    q1.enq.assign().reads(rst_done, rst_cnt, w_enq_one, w_enq_two, w_enq_sel_q0, enq0, enq1) =
        [](auto src) {
            auto [rst_done, rst_cnt, w_enq_one, w_enq_two, w_enq_sel_q0, enq0, enq1] = src;
            if (!rst_done) return Valid<uint8_t>{true, static_cast<uint8_t>(0x40 | rst_cnt)};
            if (w_enq_one)
                return Valid<uint8_t>{!w_enq_sel_q0, enq0.valid ? enq0.bits : enq1.bits};
            return Valid<uint8_t>{w_enq_two, enq1.bits};
        };

    w_deq_one.assign().reads(deq0_rdy, deq1_rdy) = [](auto src) {
        auto [deq0_rdy, deq1_rdy] = src;
        return deq0_rdy != deq1_rdy;
    };
    w_deq_two.assign().reads(deq0_rdy, deq1_rdy) = [](auto src) {
        auto [deq0_rdy, deq1_rdy] = src;
        return deq0_rdy && deq1_rdy;
    };
    w_deq_sel_q0.assign().reads(q0.count, q1.count) = [](auto src) {
        auto [q0_count, q1_count] = src;
        return q0_count >= q1_count;
    };
    q0.deq_rdy.assign().reads(w_deq_one, w_deq_two, w_deq_sel_q0) = [](auto src) {
        auto [w_deq_one, w_deq_two, w_deq_sel_q0] = src;
        return w_deq_one ? w_deq_sel_q0 : w_deq_two;
    };
    q1.deq_rdy.assign().reads(w_deq_one, w_deq_two, w_deq_sel_q0) = [](auto src) {
        auto [w_deq_one, w_deq_two, w_deq_sel_q0] = src;
        return w_deq_one ? !w_deq_sel_q0 : w_deq_two;
    };
    deq0.assign().reads(q0.deq, q1.deq, w_deq_one, w_deq_sel_q0, rst_done) = [](auto src) {
        auto [q0_deq, q1_deq, w_deq_one, w_deq_sel_q0, rst_done] = src;
        const uint8_t bits = (w_deq_one && !w_deq_sel_q0) ? q1_deq.bits : q0_deq.bits;
        return Valid<uint8_t>{q0_deq.valid && rst_done, bits};
    };
    deq1.assign().reads(q0.deq, q1.deq, w_deq_one, w_deq_sel_q0, rst_done) = [](auto src) {
        auto [q0_deq, q1_deq, w_deq_one, w_deq_sel_q0, rst_done] = src;
        const uint8_t bits = (w_deq_one && w_deq_sel_q0) ? q0_deq.bits : q1_deq.bits;
        return Valid<uint8_t>{q1_deq.valid && rst_done, bits};
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

    // ---- 读请求侧 ----
    w_has_free_chi.assign().reads(to_chi_q.free_num, r_chi_sft) = [](auto src) {
        auto [free_num, r_chi_sft] = src;
        return free_num > __builtin_popcount(r_chi_sft);
    };
    w_has_free_ds.assign().reads(to_ds_q.free_num, r_ds_sft) = [](auto src) {
        auto [free_num, r_ds_sft] = src;
        return free_num > __builtin_popcount(r_ds_sft);
    };
    read_to_chi_rdy.assign().reads(w_has_free_chi, read_to_ds) = [](auto src) {
        auto [w_has_free_chi, read_to_ds] = src;
        return w_has_free_chi && !read_to_ds.valid;
    };
    read_to_ds_rdy.assign().reads(w_has_free_ds) = [](auto src) {
        auto [w_has_free_ds] = src;
        return w_has_free_ds;
    };
    w_rd_chi_fire.assign().reads(read_to_chi, read_to_chi_rdy) = [](auto src) {
        auto [read_to_chi, read_to_chi_rdy] = src;
        return read_to_chi.valid && read_to_chi_rdy;
    };
    w_rd_ds_fire.assign().reads(read_to_ds, read_to_ds_rdy) = [](auto src) {
        auto [read_to_ds, read_to_ds_rdy] = src;
        return read_to_ds.valid && read_to_ds_rdy;
    };
    r_chi_sft.update().on(posedge(clk)).reads(r_chi_sft, w_rd_chi_fire) = [](auto src) {
        auto [r_chi_sft, w_rd_chi_fire] = src;
        return static_cast<uint8_t>((w_rd_chi_fire << 1) | (r_chi_sft >> 1));
    };
    r_ds_sft.update().on(posedge(clk)).reads(r_ds_sft, w_rd_ds_fire) = [](auto src) {
        auto [r_ds_sft, w_rd_ds_fire] = src;
        return static_cast<uint8_t>((w_rd_ds_fire << 1) | (r_ds_sft >> 1));
    };

    // datBuf 读口：rreq = RegNext(fires)，addr = RegEnable(dbid, fires)（DS 优先）
    rreq_val_reg.update().on(posedge(clk)).reads(w_rd_chi_fire, w_rd_ds_fire) = [](auto src) {
        auto [w_rd_chi_fire, w_rd_ds_fire] = src;
        return w_rd_chi_fire || w_rd_ds_fire;
    };
    rreq_addr_reg.update().on(posedge(clk)).reads(w_rd_chi_fire, w_rd_ds_fire, read_to_chi,
                                                  read_to_ds, rreq_addr_reg) = [](auto src) {
        auto [w_rd_chi_fire, w_rd_ds_fire, read_to_chi, read_to_ds, rreq_addr_reg] = src;
        if (!(w_rd_chi_fire || w_rd_ds_fire)) return rreq_addr_reg;
        return read_to_ds.valid ? read_to_ds.bits.dbid : read_to_chi.bits.dbid;
    };
    dat_buf.rreq.assign().reads(rreq_val_reg, rreq_addr_reg) = [](auto src) {
        auto [rreq_val_reg, rreq_addr_reg] = src;
        return Valid<uint32_t>{rreq_val_reg, static_cast<uint32_t>(rreq_addr_reg)};
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
    w_wri_val.assign().reads(ds_resp, from_chi) = [](auto src) {
        auto [ds_resp, from_chi] = src;
        return ds_resp.valid || from_chi.valid;
    };
    w_wri_dbid.assign().reads(ds_resp, from_chi) = [](auto src) {
        auto [ds_resp, from_chi] = src;
        return ds_resp.valid ? ds_resp.bits.dbid : from_chi.bits.dbid;
    };
    w_read_or_snp.assign().reads(from_chi) = [](auto src) {
        auto [from_chi] = src;
        const uint8_t op = from_chi.bits.dat.opcode;
        return op == dat_op::kSnpRespDataFwded || op == dat_op::kSnpRespData ||
               op == dat_op::kCompData;
    };
    wval_reg.update().on(posedge(clk)).reads(w_wri_val) = [](auto src) {
        auto [w_wri_val] = src;
        return w_wri_val;
    };
    ds_wri_reg.update().on(posedge(clk)).reads(ds_resp) = [](auto src) {
        auto [ds_resp] = src;
        return ds_resp.valid;
    };
    repl_reg.update().on(posedge(clk)).reads(repl_vec, w_wri_dbid) = [](auto src) {
        auto [repl_vec, w_wri_dbid] = src;
        return repl_vec[w_wri_dbid];
    };
    mask_reg.update().on(posedge(clk)).reads(w_wri_val, mask_vec, w_wri_dbid, mask_reg) =
        [](auto src) {
            auto [w_wri_val, mask_vec, w_wri_dbid, mask_reg] = src;
            return w_wri_val ? mask_vec[w_wri_dbid] : mask_reg;
        };
    be_reg.update().on(posedge(clk)).reads(from_chi, be_reg) = [](auto src) {
        auto [from_chi, be_reg] = src;
        return from_chi.valid ? from_chi.bits.dat.be : be_reg;
    };
    read_or_snp_reg.update().on(posedge(clk)).reads(w_read_or_snp) = [](auto src) {
        auto [w_read_or_snp] = src;
        return w_read_or_snp;
    };
    waddr_reg.update().on(posedge(clk)).reads(w_wri_val, w_wri_dbid, waddr_reg) = [](auto src) {
        auto [w_wri_val, w_wri_dbid, waddr_reg] = src;
        return w_wri_val ? w_wri_dbid : waddr_reg;
    };
    wdata_reg.update().on(posedge(clk)).reads(w_wri_val, ds_resp, from_chi, wdata_reg) =
        [](auto src) {
            auto [w_wri_val, ds_resp, from_chi, wdata_reg] = src;
            if (!w_wri_val) return wdata_reg;
            return ds_resp.valid ? beatToBytes(ds_resp.bits.beat)
                                 : beatToBytes(from_chi.bits.dat.data);
        };
    dat_buf.wreq.assign().reads(wval_reg, waddr_reg, ds_wri_reg, repl_reg, mask_reg, be_reg,
                                read_or_snp_reg, wdata_reg) = [](auto src) {
        auto [wval_reg, waddr_reg, ds_wri_reg, repl_reg, mask_reg, be_reg, read_or_snp_reg,
              wdata_reg] = src;
        typename DatBuf::WrBits b;
        b.addr = waddr_reg;
        const uint32_t full = 0xFFFFFFFFu;
        b.mask = ds_wri_reg ? (repl_reg ? full : ~mask_reg)
                            : (read_or_snp_reg ? ~mask_reg : static_cast<uint32_t>(be_reg));
        b.data = wdata_reg;
        return Valid<typename DatBuf::WrBits>{wval_reg, b};
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
    const auto enD1 = [](uint8_t sft) { return ((sft >> 1) & 1u) != 0; };
    chi_dbid_d1.update().on(posedge(clk)).reads(w_rd_chi_fire, read_to_chi, chi_dbid_d1) =
        [](auto src) {
            auto [w_rd_chi_fire, read_to_chi, chi_dbid_d1] = src;
            return w_rd_chi_fire ? read_to_chi.bits.dbid : chi_dbid_d1;
        };
    chi_dbid_d2.update().on(posedge(clk)).reads(r_chi_sft, chi_dbid_d1, chi_dbid_d2) =
        [=](auto src) {
            auto [r_chi_sft, chi_dbid_d1, chi_dbid_d2] = src;
            return enD1(r_chi_sft) ? chi_dbid_d1 : chi_dbid_d2;
        };
    chi_beat_d1.update().on(posedge(clk)).reads(w_rd_chi_fire, read_to_chi, chi_beat_d1) =
        [](auto src) {
            auto [w_rd_chi_fire, read_to_chi, chi_beat_d1] = src;
            return w_rd_chi_fire ? read_to_chi.bits.beatNum : chi_beat_d1;
        };
    chi_beat_d2.update().on(posedge(clk)).reads(r_chi_sft, chi_beat_d1, chi_beat_d2) =
        [=](auto src) {
            auto [r_chi_sft, chi_beat_d1, chi_beat_d2] = src;
            return enD1(r_chi_sft) ? chi_beat_d1 : chi_beat_d2;
        };
    chi_dcid_d1.update().on(posedge(clk)).reads(w_rd_chi_fire, read_to_chi, chi_dcid_d1) =
        [](auto src) {
            auto [w_rd_chi_fire, read_to_chi, chi_dcid_d1] = src;
            return w_rd_chi_fire ? read_to_chi.bits.dcid : chi_dcid_d1;
        };
    chi_dcid_d2.update().on(posedge(clk)).reads(r_chi_sft, chi_dcid_d1, chi_dcid_d2) =
        [=](auto src) {
            auto [r_chi_sft, chi_dcid_d1, chi_dcid_d2] = src;
            return enD1(r_chi_sft) ? chi_dcid_d1 : chi_dcid_d2;
        };
    ds_dcid_d1.update().on(posedge(clk)).reads(w_rd_ds_fire, read_to_ds, ds_dcid_d1) =
        [](auto src) {
            auto [w_rd_ds_fire, read_to_ds, ds_dcid_d1] = src;
            return w_rd_ds_fire ? read_to_ds.bits.dcid : ds_dcid_d1;
        };
    ds_dcid_d2.update().on(posedge(clk)).reads(r_ds_sft, ds_dcid_d1, ds_dcid_d2) = [=](auto src) {
        auto [r_ds_sft, ds_dcid_d1, ds_dcid_d2] = src;
        return enD1(r_ds_sft) ? ds_dcid_d1 : ds_dcid_d2;
    };
    ds_beat_d1.update().on(posedge(clk)).reads(w_rd_ds_fire, read_to_ds, ds_beat_d1) =
        [](auto src) {
            auto [w_rd_ds_fire, read_to_ds, ds_beat_d1] = src;
            return w_rd_ds_fire ? read_to_ds.bits.beatNum : ds_beat_d1;
        };
    ds_beat_d2.update().on(posedge(clk)).reads(r_ds_sft, ds_beat_d1, ds_beat_d2) = [=](auto src) {
        auto [r_ds_sft, ds_beat_d1, ds_beat_d2] = src;
        return enD1(r_ds_sft) ? ds_beat_d1 : ds_beat_d2;
    };
    ds_ds_d1.update().on(posedge(clk)).reads(w_rd_ds_fire, read_to_ds, ds_ds_d1) = [](auto src) {
        auto [w_rd_ds_fire, read_to_ds, ds_ds_d1] = src;
        return w_rd_ds_fire ? read_to_ds.bits.ds : ds_ds_d1;
    };
    ds_ds_d2.update().on(posedge(clk)).reads(r_ds_sft, ds_ds_d1, ds_ds_d2) = [=](auto src) {
        auto [r_ds_sft, ds_ds_d1, ds_ds_d2] = src;
        return enD1(r_ds_sft) ? ds_ds_d1 : ds_ds_d2;
    };

    to_chi_q.enq.assign().reads(r_chi_sft, chi_dbid_d2, chi_beat_d2, chi_dcid_d2, mask_vec,
                                dat_buf.rresp) = [](auto src) {
        auto [r_chi_sft, chi_dbid_d2, chi_beat_d2, chi_dcid_d2, mask_vec, rresp] = src;
        ToCHIEntry e;
        e.dcid = chi_dcid_d2;
        e.beatNum = chi_beat_d2;
        e.dat.be = mask_vec[chi_dbid_d2];
        e.dat.data = bytesToBeat(rresp.bits.data);
        e.dat.data_id = static_cast<uint8_t>(chi_beat_d2 << 1);
        return Valid<ToCHIEntry>{(r_chi_sft & 1u) != 0, e};
    };
    to_ds_q.enq.assign().reads(r_ds_sft, ds_ds_d2, ds_dcid_d2, ds_beat_d2, dat_buf.rresp) =
        [](auto src) {
            auto [r_ds_sft, ds_ds_d2, ds_dcid_d2, ds_beat_d2, rresp] = src;
            WriteDS w;
            w.ds = ds_ds_d2;
            w.dcid = ds_dcid_d2;
            w.beatNum = ds_beat_d2;
            w.beat = bytesToBeat(rresp.bits.data);
            return Valid<WriteDS>{(r_ds_sft & 1u) != 0, w};
        };

    to_chi = to_chi_q.deq;
    to_chi_q.deq_rdy = to_chi_rdy;
    write_ds = to_ds_q.deq;
    to_ds_q.deq_rdy = write_ds_rdy;
}

}  // namespace zj::dj
