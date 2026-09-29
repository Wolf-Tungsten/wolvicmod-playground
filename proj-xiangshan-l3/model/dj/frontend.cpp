#include <wolvicmod/wolvicmod.h>

#include "model/dj/frontend.h"

namespace zj::dj {

namespace dc = dectab;

namespace {
constexpr uint8_t kChReq = 0, kChSnp = 3;
constexpr uint8_t kFullVec = 0x3;
constexpr uint8_t kDBIDResp = 0x6, kReadReceipt = 0x8, kCompDataOp = 0x4;
}  // namespace

// ---------------- ReqToChiTask ----------------

ReqToChiTask::ReqToChiTask() {
    chi_task.assign().reads(cfg_ci, rx_req) = [](auto src) {
        auto [cfg_ci, rx_req] = src;
        ChiTask t;
        t.addr = rx_req.bits.addr;
        t.qos = rx_req.bits.qos;
        t.chi.toLAN = ciOf(rx_req.bits.addr) == cfg_ci;
        t.chi.fromLAN = true;  // NocType.rxIs(req, LAN)：ring 侧 tgt 恒 LAN
        t.chi.nodeId = rx_req.bits.src_id;
        t.chi.channel = 0;
        t.chi.opcode = rx_req.bits.opcode;
        t.chi.txnID = rx_req.bits.txn_id;
        t.chi.order = rx_req.bits.order;
        t.chi.snpAttr = rx_req.bits.snp_attr;
        t.chi.snoopMe = rx_req.bits.excl;
        t.chi.memAttr = rx_req.bits.mem_attr;
        t.chi.expCompAck = rx_req.bits.exp_comp_ack;
        t.chi.size = rx_req.bits.size;
        const bool full = rx_req.bits.size == 6;
        const bool hi = (rx_req.bits.addr >> 5) & 1;
        t.chi.dataVec = (full || hi) ? 0x2 : 0x0;
        t.chi.dataVec |= (full || !hi) ? 0x1 : 0x0;
        return Valid<ChiTask>{rx_req.valid, t};
    };
    rx_req_rdy.assign().reads(chi_task_rdy) = [](auto src) {
        auto [chi_task_rdy] = src;
        return chi_task_rdy;
    };
}


// ---------------- Block ----------------

Block::Block() {
    valid_reg_s1.update().on(posedge(clk)).reads(chi_task_s0) = [](auto src) {
        auto [chi_task_s0] = src;
        return chi_task_s0.valid;
    };
    task_reg_s1.update().on(posedge(clk)).reads(chi_task_s0, task_reg_s1) = [](auto src) {
        auto [chi_task_s0, task_reg_s1] = src;
        return chi_task_s0.valid ? chi_task_s0.bits : task_reg_s1;
    };
    s_receipt_reg_s1.update().on(posedge(clk)).reads(chi_task_s0) = [](auto src) {
        auto [chi_task_s0] = src;
        return chi_task_s0.bits.chi.isRead() &&
               (chi_task_s0.bits.chi.isEO() || chi_task_s0.bits.chi.isRO());
    };
    s_dbid_reg_s1.update().on(posedge(clk)).reads(chi_task_s0) = [](auto src) {
        auto [chi_task_s0] = src;
        return chi_task_s0.bits.chi.isWrite() && !chi_task_s0.bits.chi.isCopyBackWrite();
    };
    w_should_resp_s1.assign().reads(s_receipt_reg_s1, s_dbid_reg_s1, req_db_s1_rdy) =
        [](auto src) {
            auto [s_receipt_reg_s1, s_dbid_reg_s1, req_db_s1_rdy] = src;
            return s_receipt_reg_s1 || (s_dbid_reg_s1 && req_db_s1_rdy);
        };
    w_block_by_db_s1.assign().reads(s_dbid_reg_s1, req_db_s1_rdy) = [](auto src) {
        auto [s_dbid_reg_s1, req_db_s1_rdy] = src;
        return s_dbid_reg_s1 && !req_db_s1_rdy;
    };
    w_block_pos.assign().reads(pos_block_s1) = [](auto src) {
        auto [pos_block_s1] = src;
        return pos_block_s1;
    };
    w_block_dir.assign().reads(task_reg_s1, read_dir_s1_rdy) = [](auto src) {
        auto [task_reg_s1, read_dir_s1_rdy] = src;
        return task_reg_s1.chi.memCacheable() && !read_dir_s1_rdy;
    };
    w_block_resp.assign().reads(w_block_by_db_s1, w_should_resp_s1, fast_resp_s1_rdy) =
        [](auto src) {
            auto [w_block_by_db_s1, w_should_resp_s1, fast_resp_s1_rdy] = src;
            return w_block_by_db_s1 || (w_should_resp_s1 && !fast_resp_s1_rdy);
        };
    w_block_any.assign().reads(w_block_pos, w_block_dir, w_block_resp) = [](auto src) {
        auto [w_block_pos, w_block_dir, w_block_resp] = src;
        return w_block_pos || w_block_dir || w_block_resp;
    };
    retry_s1.assign().reads(valid_reg_s1, w_block_any) = [](auto src) {
        auto [valid_reg_s1, w_block_any] = src;
        return valid_reg_s1 && w_block_any;
    };
    task_s1.assign().reads(valid_reg_s1, w_block_any, task_reg_s1, hn_idx_s1, req_db_s1,
                           req_db_s1_rdy, fast_resp_s1, fast_resp_s1_rdy) = [](auto src) {
        auto [valid_reg_s1, w_block_any, task_reg_s1, hn_idx_s1, req_db_s1, req_db_s1_rdy,
              fast_resp_s1, fast_resp_s1_rdy] = src;
        TaskS1 t;
        t.chi = task_reg_s1.chi;
        t.addr = task_reg_s1.addr;
        t.qos = task_reg_s1.qos;
        t.hnIdx = hn_idx_s1;
        t.alr.reqDB = req_db_s1.valid && req_db_s1_rdy;
        t.alr.sData = false;
        t.alr.sDBID = fast_resp_s1.valid && fast_resp_s1_rdy &&
                      fast_resp_s1.bits.opcode == kDBIDResp;
        return Valid<TaskS1>{valid_reg_s1 && !w_block_any, t};
    };
    read_dir_s1.assign().reads(valid_reg_s1, task_reg_s1, w_block_pos, w_block_resp,
                               hn_idx_s1) = [](auto src) {
        auto [valid_reg_s1, task_reg_s1, w_block_pos, w_block_resp, hn_idx_s1] = src;
        DirRdReq r;
        r.addr = task_reg_s1.addr;
        r.hnIdx = hn_idx_s1;
        return Valid<DirRdReq>{valid_reg_s1 && task_reg_s1.chi.memCacheable() &&
                                   !(w_block_pos || w_block_resp),
                               r};
    };
    req_db_s1.assign().reads(valid_reg_s1, s_dbid_reg_s1, fast_resp_s1_rdy, w_block_pos,
                             w_block_dir, hn_idx_s1) = [](auto src) {
        auto [valid_reg_s1, s_dbid_reg_s1, fast_resp_s1_rdy, w_block_pos, w_block_dir,
              hn_idx_s1] = src;
        ReqDB r;
        r.hnTxnID = hn_idx_s1;
        r.dataVec = kFullVec;
        return Valid<ReqDB>{valid_reg_s1 && s_dbid_reg_s1 && fast_resp_s1_rdy &&
                                !(w_block_pos || w_block_dir),
                            r};
    };
    fast_resp_s1.assign().reads(valid_reg_s1, w_should_resp_s1, w_block_pos, w_block_dir,
                                s_receipt_reg_s1, s_dbid_reg_s1, task_reg_s1,
                                hn_idx_s1) = [](auto src) {
        auto [valid_reg_s1, w_should_resp_s1, w_block_pos, w_block_dir, s_receipt_reg_s1,
              s_dbid_reg_s1, task_reg_s1, hn_idx_s1] = src;
        RespFlit f{};
        f.qos = task_reg_s1.qos;
        f.src_id = task_reg_s1.chi.getNoC();
        f.tgt_id = task_reg_s1.chi.nodeId;
        f.txn_id = task_reg_s1.chi.txnID;
        f.dbid = hn_idx_s1;
        f.resp_err = 0;
        f.opcode = s_receipt_reg_s1 ? kReadReceipt : kDBIDResp;
        return Valid<RespFlit>{valid_reg_s1 && w_should_resp_s1 && !(w_block_pos || w_block_dir),
                               f};
    };
}

}  // namespace zj::dj

namespace zj::dj {

// ---------------- PosTable ----------------

PosTable::PosTable() {
    // ---- s0 组合（per set 数组化，各一条 assign） ----
    w_mat_tag_vec.assign().reads(entries, alloc_s0_addr) = [](auto src) {
        auto [entries, alloc_s0_addr] = src;
        U32Arr4 v{};
        const uint64_t tag = posTagOf(alloc_s0_addr);
        for (uint32_t s = 0; s < 4; ++s)
            for (uint32_t i = 0; i < 16; ++i) {
                const PosState& st = entries[s * 16 + i].state;
                if (st.valid() && st.tagVal && st.tag == tag) v[s] |= (1u << i);
            }
        return v;
    };
    w_free_vec.assign().reads(entries, s1) = [](auto src) {
        auto [entries, s1] = src;
        U32Arr4 v{};
        for (uint32_t s = 0; s < 4; ++s) {
            const uint32_t useWay = s1[s].allocValid ? ~(1u << s1[s].allocWay) : 0xFFFFu;
            uint32_t f = 0;
            for (uint32_t i = 0; i < 16; ++i)
                if (!entries[s * 16 + i].state.valid()) f |= (1u << i);
            v[s] = f & useWay;
        }
        return v;
    };
    // 原 RTL 的 blockReq/blockSnp 因 canNest 恒 false 而同式，isSnp 分流略去
    w_block_s0.assign().reads(w_mat_tag_vec, w_free_vec, alloc_s0_addr, s1,
                              req_pos_vec) = [](auto src) {
        auto [mat_tag, free_vec, alloc_s0_addr, s1, req_pos_vec] = src;
        BoolArr4 b{};
        for (uint32_t s = 0; s < 4; ++s) {
            const bool hasMatTag = mat_tag[s] != 0;
            const bool hasFree = (free_vec[s] & 0x3FFFu) != 0;  // way0-13 有 free
            const bool matchReqS1 =
                s1[s].allocValid && posTagOf(alloc_s0_addr) == posTagOf(s1[s].allocAddr);
            b[s] = hasMatTag || !hasFree || matchReqS1 || s1[s].lock || req_pos_vec[s].valid;
        }
        return b;
    };

    // ---- reqPoS：replSelWay / reqPosFire ----
    w_free_vec2.assign().reads(entries) = [](auto src) -> U32Arr4 {
        auto [entries] = src;
        U32Arr4 v{};
        for (uint32_t s = 0; s < 4; ++s)
            for (uint32_t i = 0; i < 16; ++i)
                if (!entries[s * 16 + i].state.valid()) v[s] |= (1u << i);
        return v;
    };
    w_repl_sel_way.assign().reads(w_free_vec2, req_pos_vec) = [](auto src) {
        auto [free_vec, req_pos_vec] = src;
        U8Arr4 w{};
        for (uint32_t s = 0; s < 4; ++s) {
            const uint32_t fv = free_vec[s];
            const uint8_t ch = req_pos_vec[s].bits.channel;
            const bool isReq = ch == 0;
            const bool isSnp = ch == 3;
            uint8_t sel = 0;
            if (isReq && ((fv >> 15) & 1u)) {
                sel = 15;
            } else if (isSnp && ((fv >> 14) & 1u)) {
                sel = 14;
            } else {
                for (uint32_t i = 0; i < 14; ++i)
                    if ((fv >> i) & 1u) {
                        sel = static_cast<uint8_t>(i);
                        break;
                    }
            }
            w[s] = sel;
        }
        return w;
    };
    w_req_pos_fire.assign().reads(req_pos_vec, w_free_vec2, w_repl_sel_way, s1) = [](auto src) {
        auto [req_pos_vec, free_vec, sel_way, s1] = src;
        BoolArr4 f{};
        for (uint32_t s = 0; s < 4; ++s)
            f[s] = req_pos_vec[s].valid && ((free_vec[s] >> sel_way[s]) & 1u) && !s1[s].lock;
        return f;
    };

    // ---- s1 流水 + 控制寄存器（整项；alloc 经 per-set 门控的 alloc_s0_valid） ----
    s1.update().on(posedge(clk)).reads(s1, alloc_s0_valid, alloc_s0_addr, alloc_s0_channel,
                                       w_block_s0, w_free_vec, w_mat_tag_vec, w_req_pos_fire,
                                       w_repl_sel_way, upd_tag, dir_bank) = [](auto src) {
        auto [s1, alloc_s0_valid, alloc_s0_addr, alloc_s0_channel, block_s0, free_vec, mat_tag,
              fire, sel_way, upd_tag, dir_bank] = src;
        SetArr n = s1;
        const uint32_t aset = posSetOf(alloc_s0_addr);
        for (uint32_t s = 0; s < 4; ++s) {
            const bool allocS0 = alloc_s0_valid && aset == s;
            n[s].allocValid = allocS0 && !block_s0[s];
            if (allocS0) {
                n[s].allocAddr = alloc_s0_addr;
                n[s].allocChannel = alloc_s0_channel;
                // freeWay_s0 = PriorityEncoder(freeVec)
                uint8_t way = 0;
                for (uint32_t i = 0; i < 16; ++i)
                    if ((free_vec[s] >> i) & 1u) {
                        way = static_cast<uint8_t>(i);
                        break;
                    }
                n[s].allocWay = way;
            }
            n[s].sleep = allocS0 && mat_tag[s] != 0;
            n[s].block = allocS0 && block_s0[s];
            n[s].hnIdxValid = allocS0;
            n[s].posRespValid = fire[s];
            if (fire[s]) n[s].posRespWay = sel_way[s];
            if (fire[s]) {
                n[s].lock = true;
            } else if (upd_tag.valid && hnIdxDirBank(upd_tag.bits.hnIdx) == dir_bank &&
                       hnIdxPosSet(upd_tag.bits.hnIdx) == s) {
                n[s].lock = false;
            }
        }
        return n;
    };

    // ---- 64 项表项（alloc/updTag/clean 驱动；wakeup 为 RegNext，读旧 state） ----
    entries.update().on(posedge(clk)).reads(entries, s1, retry_s1, req_pos_vec, w_req_pos_fire,
                                            w_repl_sel_way, upd_tag, clean, dir_bank,
                                            cfg_bank_id) = [](auto src) {
        auto [entries, s1, retry_s1, req_pos_vec, fire, sel_way, upd_tag, clean, dir_bank,
              cfg_bank_id] = src;
        EntryArr n = entries;
        for (uint32_t s = 0; s < 4; ++s) {
            const bool reqPosV = req_pos_vec[s].valid;
            const uint8_t reqPosCh = req_pos_vec[s].bits.channel;
            for (uint32_t i = 0; i < 16; ++i) {
                const uint8_t hn = hnIdxOf(dir_bank, s, i);
                const PosEntryV& cur = entries[s * 16 + i];
                PosEntryV& ne = n[s * 16 + i];
                // entry alloc 驱动（s1 拍或 reqPoS 拍）
                const bool allocV = reqPosV ? (fire[s] && sel_way[s] == i)
                                            : (s1[s].allocValid && !retry_s1 &&
                                               s1[s].allocWay == i);
                const bool allocAddrVal = !reqPosV;
                const uint64_t allocAddr = reqPosV ? 0ull : s1[s].allocAddr;
                const uint8_t allocChannel = reqPosV ? reqPosCh : s1[s].allocChannel;
                const bool updTagHit = upd_tag.valid && upd_tag.bits.hnIdx == hn;
                const bool cleanHit = clean.valid && clean.bits.hnIdx == hn;
                PosState st = cur.state;
                if (allocV) {
                    st.tagVal = allocAddrVal;
                    st.tag = posTagOf(allocAddr);
                    st.offset = static_cast<uint8_t>(allocAddr & 0x3F);
                } else if (updTagHit) {
                    st.tagVal = upd_tag.bits.addrVal;
                    st.tag = posTagOf(upd_tag.bits.addr);
                    st.offset = static_cast<uint8_t>(upd_tag.bits.addr & 0x3F);
                }
                if (cleanHit && clean.bits.channel == 0) {
                    st.req = false;
                } else if (allocV && allocChannel == 0) {
                    st.req = true;
                }
                if (cleanHit && clean.bits.channel == 3) {
                    st.snp = false;
                } else if (allocV && allocChannel == 3) {
                    st.snp = true;
                }
                // 更新条件：alloc | updTagHit | cleanHit（RTL 同）
                if (allocV || updTagHit || cleanHit) ne.state = st;
                // wakeup = RegNext(cleanHit & one & tagVal)
                const bool wake = cleanHit && cur.state.one() && cur.state.tagVal;
                ne.wakeup = wake;
                if (wake)
                    ne.wakeupAddr = catPosAddr(cfg_bank_id, cur.state.tag, hnIdxPosSet(hn),
                                               hnIdxDirBank(hn));
            }
        }
        return n;
    };

    // ---- 出口 ----
    sleep_s1.assign().reads(s1) = [](auto src) {
        auto [s1] = src;
        for (const auto& v : s1)
            if (v.sleep) return true;
        return false;
    };
    block_s1.assign().reads(s1, req_pos_vec) = [](auto src) {
        auto [s1, req_pos_vec] = src;
        for (uint32_t s = 0; s < 4; ++s)
            if (s1[s].block || req_pos_vec[s].valid) return true;
        return false;
    };
    hn_idx_s1.assign().reads(s1, req_pos_vec, dir_bank) = [](auto src) {
        auto [s1, req_pos_vec, dir_bank] = src;
        for (uint32_t s = 0; s < 4; ++s)
            if (s1[s].hnIdxValid && !req_pos_vec[s].valid) return hnIdxOf(dir_bank, s, s1[s].allocWay);
        return static_cast<uint8_t>(0);
    };
    hn_idx_s1_valid.assign().reads(s1, req_pos_vec) = [](auto src) {
        auto [s1, req_pos_vec] = src;
        for (uint32_t s = 0; s < 4; ++s)
            if (s1[s].hnIdxValid && !req_pos_vec[s].valid) return true;
        return false;
    };
    pos_resp_vec.assign().reads(s1) = [](auto src) {
        auto [s1] = src;
        PosRespArr4 r;
        for (uint32_t s = 0; s < 4; ++s) r[s] = Valid<uint8_t>{s1[s].posRespValid, s1[s].posRespWay};
        return r;
    };
    // wakeup Mux1H：set 优先、way 次之（与原两层 mux 同一优先级序）
    wakeup.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        for (const auto& e : entries)
            if (e.wakeup) return Valid<uint64_t>{true, e.wakeupAddr};
        return Valid<uint64_t>{false, 0};
    };
    addr_vec2.assign().reads(entries, cfg_bank_id, dir_bank) = [](auto src) {
        auto [entries, cfg_bank_id, dir_bank] = src;
        AddrVec2 r;
        for (uint32_t s = 0; s < 4; ++s)
            for (uint32_t i = 0; i < 16; ++i) {
                const PosState& st = entries[s * 16 + i].state;
                r[s][i] = catPosAddr(cfg_bank_id, st.tag, s, dir_bank, st.offset);
            }
        return r;
    };
    alr_use_pos.assign().reads(entries) = [](auto src) -> uint8_t {
        auto [entries] = src;
        uint8_t cnt = 0;
        for (const auto& e : entries) cnt += e.state.valid();
        return cnt;
    };
    working.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        for (const auto& e : entries)
            if (e.state.valid()) return true;
        return false;
    };
}

// ---------------- FrontendDecode ----------------

FrontendDecode::FrontendDecode() {
    valid_reg_s3.update().on(posedge(clk)).reads(task_s2) = [](auto src) {
        auto [task_s2] = src;
        return task_s2.valid;
    };
    task_reg_s3.update().on(posedge(clk)).reads(task_s2, task_reg_s3) = [](auto src) {
        auto [task_s2, task_reg_s3] = src;
        return task_s2.valid ? task_s2.bits : task_reg_s3;
    };
    // fstDec：chiInst_s2 → decList_s2（组合）
    w_chi_inst_s2.assign().reads(task_s2) = [](auto src) {
        auto [task_s2] = src;
        uint32_t ci = task_s2.bits.chi.getChiInst();
        // valid 由 task_s2.valid 门控
        ci = (ci & ~(1u << 17)) | (static_cast<uint32_t>(task_s2.valid) << 17);
        return ci;
    };
    w_dec_list_s2.assign().reads(w_chi_inst_s2) = [](auto src) {
        auto [w_chi_inst_s2] = src;
        std::array<uint8_t, 4> l{0, 0, 0, 0};
        l[0] = static_cast<uint8_t>(dc::decChi(w_chi_inst_s2));
        return l;
    };
    dec_list_reg_s3.update().on(posedge(clk)).reads(task_s2, w_dec_list_s2, dec_list_reg_s3) =
        [](auto src) {
            auto [task_s2, w_dec_list_s2, dec_list_reg_s3] = src;
            return task_s2.valid ? w_dec_list_s2 : dec_list_reg_s3;
        };
    // stateInst_s3 / secDec / GetDecRes
    w_state_inst_s3.assign().reads(resp_dir_s3, valid_reg_s3, task_reg_s3) = [](auto src) {
        auto [resp_dir_s3, valid_reg_s3, task_reg_s3] = src;
        if (!resp_dir_s3.valid) {
            // Lit(valid -> true.B)：仅 valid=1，src/oth/llcState=0
            return (1u << 4) | (static_cast<uint32_t>(valid_reg_s3) << 4);
        }
        const auto& d = resp_dir_s3.bits;
        const uint32_t srcHit = d.sf.hit && d.sf.meta != 0;
        const uint32_t othHit = 0;  // nrSfMetas=1 → othVec 恒 0
        const uint32_t llcState = d.llc.hit ? d.llc.meta : 0;
        return (static_cast<uint32_t>(valid_reg_s3) << 4) | (srcHit << 3) | (othHit << 2) |
               llcState;
    };
    w_dec_list_s3.assign().reads(w_state_inst_s3, dec_list_reg_s3) = [](auto src) {
        auto [w_state_inst_s3, dec_list_reg_s3] = src;
        auto l = dec_list_reg_s3;
        l[1] = static_cast<uint8_t>(dc::decState(l[0], w_state_inst_s3));
        return l;
    };
    w_task_code_s3.assign().reads(w_dec_list_s3) = [](auto src) {
        auto [w_dec_list_s3] = src;
        return dc::getTaskCode(w_dec_list_s3[0], w_dec_list_s3[1]);
    };
    w_cmt_code_s3.assign().reads(w_dec_list_s3) = [](auto src) {
        auto [w_dec_list_s3] = src;
        return dc::getCommitCode(w_dec_list_s3[0], w_dec_list_s3[1], w_dec_list_s3[2],
                                 w_dec_list_s3[3]);
    };
    w_resp_comp_data_s3.assign().reads(valid_reg_s3, w_task_code_s3, w_cmt_code_s3) =
        [](auto src) {
            auto [valid_reg_s3, w_task_code_s3, w_cmt_code_s3] = src;
            return valid_reg_s3 && !dc::tcIsValid(w_task_code_s3) &&
                   dc::ccSendResp(w_cmt_code_s3) && dc::ccChannel(w_cmt_code_s3) == 1 &&
                   dc::ccOpcode(w_cmt_code_s3) == kCompDataOp;
        };

    // cmtTask_s3 组装
    cmt_task_s3.assign().reads(valid_reg_s3, task_reg_s3, resp_dir_s3, w_dec_list_s3,
                               w_task_code_s3, w_cmt_code_s3, req_db_s3, req_db_s3_rdy,
                               fast_data_s3, fast_data_s3_rdy) = [](auto src) {
        auto [valid_reg_s3, task_reg_s3, resp_dir_s3, w_dec_list_s3, w_task_code_s3,
              w_cmt_code_s3, req_db_s3, req_db_s3_rdy, fast_data_s3, fast_data_s3_rdy] = src;
        CommitTask t;
        t.hnTxnID = task_reg_s3.hnIdx;
        t.qos = task_reg_s3.qos;
        t.chi = task_reg_s3.chi;
        t.dir = resp_dir_s3.valid ? resp_dir_s3.bits : DirMsg{};
        t.alr = task_reg_s3.alr;
        t.alr.reqDB = (req_db_s3.valid && req_db_s3_rdy) || task_reg_s3.alr.reqDB;
        t.alr.sData = fast_data_s3.valid && fast_data_s3_rdy;
        t.decList = w_dec_list_s3;
        t.task = w_task_code_s3;
        t.cmt = dc::tcIsValid(w_task_code_s3) ? 0 : w_cmt_code_s3;
        const uint32_t way = resp_dir_s3.valid ? ohToUInt(resp_dir_s3.bits.llc.wayOH) : 0;
        t.ds.set(task_reg_s3.addr, way);
        return Valid<CommitTask>{valid_reg_s3, t};
    };

    // reqDB_s3 / fastData_s3 快路径
    req_db_s3.assign().reads(w_resp_comp_data_s3, task_reg_s3) = [](auto src) {
        auto [w_resp_comp_data_s3, task_reg_s3] = src;
        ReqDB r;
        r.hnTxnID = task_reg_s3.hnIdx;
        r.dataVec = task_reg_s3.chi.dataVec;
        return Valid<ReqDB>{w_resp_comp_data_s3, r};
    };
    fast_data_s3.assign().reads(w_resp_comp_data_s3, req_db_s3_rdy, task_reg_s3,
                                w_cmt_code_s3, resp_dir_s3) = [](auto src) {
        auto [w_resp_comp_data_s3, req_db_s3_rdy, task_reg_s3, w_cmt_code_s3, resp_dir_s3] =
            src;
        DataTask t{};
        t.hnTxnID = task_reg_s3.hnIdx;
        t.dataOp.read = true;
        t.dataOp.send = true;
        t.dataVec = task_reg_s3.chi.dataVec;
        t.qos = 0;  // fastData.bits.qos 在 RTL 中属 DontCare（下件为 0）
        const uint32_t way = resp_dir_s3.valid ? ohToUInt(resp_dir_s3.bits.llc.wayOH) : 0;
        t.ds.set(task_reg_s3.addr, way);
        t.txDat.dbid = task_reg_s3.hnIdx;
        t.txDat.resp = static_cast<uint8_t>(dc::ccResp(w_cmt_code_s3));
        t.txDat.opcode = kCompDataOp;
        t.txDat.txn_id = task_reg_s3.chi.txnID;
        t.txDat.src_id = task_reg_s3.chi.getNoC();
        t.txDat.tgt_id = task_reg_s3.chi.nodeId;
        return Valid<DataTask>{w_resp_comp_data_s3 && req_db_s3_rdy, t};
    };

    // cleanUnuseDB
    w_clean_unuse_db_s3.assign().reads(valid_reg_s3, task_reg_s3, resp_dir_s3) = [](auto src) {
        auto [valid_reg_s3, task_reg_s3, resp_dir_s3] = src;
        const bool sfHit = resp_dir_s3.valid && resp_dir_s3.bits.sf.hit;
        const bool llcHit = resp_dir_s3.valid && resp_dir_s3.bits.llc.hit;
        return valid_reg_s3 && task_reg_s3.alr.reqDB && !task_reg_s3.chi.isFullSize() &&
               !(sfHit || llcHit);
    };
    clean_db_s3.assign().reads(w_clean_unuse_db_s3, task_reg_s3) = [](auto src) {
        auto [w_clean_unuse_db_s3, task_reg_s3] = src;
        ReqDB r;
        r.hnTxnID = task_reg_s3.hnIdx;
        r.dataVec = (~task_reg_s3.chi.dataVec) & 0x3;
        return Valid<ReqDB>{w_clean_unuse_db_s3, r};
    };
}

// ---------------- Frontend ----------------

Frontend::Frontend() {
    rx_q.clk = clk;
    rx_hpr_q.clk = clk;
    req2task.clk = clk;
    hpr2task.clk = clk;
    req_task_buf.clk = clk;
    hpr_task_buf.clk = clk;
    block.clk = clk;
    pos_table.clk = clk;
    s1_pipe.clk = clk;
    decode.clk = clk;
    fast_resp_q.clk = clk;

    req2task.cfg_ci = cfg_ci;
    hpr2task.cfg_ci = cfg_ci;
    block.cfg_ci = cfg_ci;
    decode.cfg_ci = cfg_ci;
    pos_table.cfg_bank_id = cfg_bank_id;
    pos_table.dir_bank = dir_bank;

    // rxReq → FastQueue → ReqToChiTask → TaskBuffer
    rx_q.enq = rx_req;
    rx_req_rdy = rx_q.enq_rdy;
    req2task.rx_req = rx_q.deq;
    rx_q.deq_rdy = req2task.rx_req_rdy;
    req_task_buf.chi_task_in = req2task.chi_task;
    req2task.chi_task_rdy = req_task_buf.chi_task_in_rdy;

    // rxHpr → FastQueue → ReqToChiTask → HprTaskBuffer（同构，16 项）
    rx_hpr_q.enq = rx_hpr;
    rx_hpr_rdy = rx_hpr_q.enq_rdy;
    hpr2task.rx_req = rx_hpr_q.deq;
    rx_hpr_q.deq_rdy = hpr2task.rx_req_rdy;
    hpr_task_buf.chi_task_in = hpr2task.chi_task;
    hpr2task.chi_task_rdy = hpr_task_buf.chi_task_in_rdy;
    hpr_task_buf.retry_s1 = block.retry_s1;
    hpr_task_buf.sleep_s1 = pos_table.sleep_s1;
    hpr_task_buf.wakeup = pos_table.wakeup;

    // selectReq = !hprBuf.s0.valid & !hprBuf.lockTask；hpr ready 恒 true
    w_select_req.assign().reads(hpr_task_buf.chi_task_s0, hpr_task_buf.lock_task) =
        [](auto src) {
            auto [hpr_s0, hpr_lock] = src;
            return !hpr_s0.valid && !hpr_lock;
        };
    block.chi_task_s0.assign().reads(w_select_req, req_task_buf.chi_task_s0,
                                     hpr_task_buf.chi_task_s0) = [](auto src) {
        auto [sel, req_s0, hpr_s0] = src;
        return sel ? req_s0 : hpr_s0;
    };
    req_task_buf.chi_task_s0_rdy = w_select_req;
    hpr_task_buf.chi_task_s0_rdy = true;
    req_task_buf.retry_s1 = block.retry_s1;
    req_task_buf.sleep_s1 = pos_table.sleep_s1;
    req_task_buf.wakeup = pos_table.wakeup;

    // posAlloc_s0：与 chiTask_s0（req/hpr 选择后）同步（channel 恒 REQ，无 BBN）
    pos_table.alloc_s0_valid.assign().reads(block.chi_task_s0) = [](auto src) {
        auto [s0] = src;
        return s0.valid;
    };
    pos_table.alloc_s0_addr.assign().reads(block.chi_task_s0) = [](auto src) {
        auto [s0] = src;
        return s0.bits.addr;
    };
    pos_table.alloc_s0_channel = static_cast<uint8_t>(0);  // ChiChannel.REQ
    pos_table.retry_s1 = block.retry_s1;
    pos_table.req_pos_vec = req_pos_vec;
    pos_resp_vec = pos_table.pos_resp_vec;
    pos_table.upd_tag = upd_pos_tag;
    pos_table.clean = clean_pos;

    block.pos_block_s1 = pos_table.block_s1;
    block.hn_idx_s1 = pos_table.hn_idx_s1;

    // Block s1 → Pipe(3) → Decode
    s1_pipe.enq = block.task_s1;
    decode.task_s2 = s1_pipe.deq;
    decode.resp_dir_s3 = resp_dir;

    // 出口
    read_dir = block.read_dir_s1;
    block.read_dir_s1_rdy = read_dir_rdy;
    req_db_s1 = block.req_db_s1;
    block.req_db_s1_rdy = req_db_s1_rdy;
    fast_resp_q.enq = block.fast_resp_s1;
    block.fast_resp_s1_rdy = fast_resp_q.enq_rdy;
    fast_resp = fast_resp_q.deq;
    fast_resp_q.deq_rdy = fast_resp_rdy;
    req_db_s3 = decode.req_db_s3;
    decode.req_db_s3_rdy = req_db_s3_rdy;
    fast_data_s3 = decode.fast_data_s3;
    decode.fast_data_s3_rdy = fast_data_s3_rdy;
    clean_db_s3 = decode.clean_db_s3;
    cmt_task = decode.cmt_task_s3;
    get_addr_result.assign().reads(get_addr_hnidx, pos_table.addr_vec2) = [](auto src) {
        auto [hnidxs, vec2] = src;
        U64x3 r;
        for (uint32_t i = 0; i < 3; ++i)
            r[i] = vec2[hnIdxPosSet(hnidxs[i])][hnIdxPosWay(hnidxs[i])];
        return r;
    };
    alr_use_pos = pos_table.alr_use_pos;
    working.assign().reads(req_task_buf.working, hpr_task_buf.working, pos_table.working) =
        [](auto src) {
            auto [a, h, b] = src;
            return a || h || b;
        };
}

}  // namespace zj::dj
