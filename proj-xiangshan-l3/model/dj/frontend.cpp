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

// ---------------- TaskEntry ----------------

TaskEntry::TaskEntry() {
    chi_task_in_rdy.assign().reads(task_reg) = [](auto src) {
        auto [task_reg] = src;
        return task_reg.state == taskst::kFree;
    };
    chi_task_s0.assign().reads(task_reg, nid_reg) = [](auto src) {
        auto [task_reg, nid_reg] = src;
        ChiTask t;
        t.chi = task_reg.chi;
        t.addr = task_reg.addr;
        t.qos = task_reg.qos;
        return Valid<ChiTask>{task_reg.state == taskst::kSend && nid_reg == 0, t};
    };
    // sort：nid 维护
    nid_reg.update().on(posedge(clk)).reads(nid_reg, chi_task_in, chi_task_in_rdy, task_reg,
                                            oth_rel, init_nid) = [](auto src) -> uint8_t {
        auto [nid_reg, chi_task_in, chi_task_in_rdy, task_reg, oth_rel, init_nid] = src;
        if (chi_task_in.valid && chi_task_in_rdy) return init_nid;
        if (task_reg.state != taskst::kFree && oth_rel)
            return static_cast<uint8_t>(nid_reg > 0 ? nid_reg - 1 : 0);
        return nid_reg;
    };
    // 超时计数：isWait & retry_s1
    retry_num_reg.update().on(posedge(clk)).reads(retry_num_reg, chi_task_in, chi_task_in_rdy,
                                                  task_reg, retry_s1) = [](auto src) -> uint8_t {
        auto [retry_num_reg, chi_task_in, chi_task_in_rdy, task_reg, retry_s1] = src;
        if (chi_task_in.valid && chi_task_in_rdy) return 0;
        if (task_reg.state == taskst::kWait && retry_s1 && retry_num_reg < 7)
            return static_cast<uint8_t>(retry_num_reg + 1);
        return retry_num_reg;
    };
    timeout_reg.update().on(posedge(clk)).reads(retry_num_reg) = [](auto src) {
        auto [retry_num_reg] = src;
        return retry_num_reg == 7;
    };
    // 状态机
    w_wakeup_hit.assign().reads(task_reg, wakeup) = [](auto src) {
        auto [task_reg, wakeup] = src;
        return wakeup.valid && useAddr(task_reg.addr) == useAddr(wakeup.bits);
    };
    task_reg.update().on(posedge(clk)).reads(task_reg, chi_task_in, chi_task_in_rdy,
                                             chi_task_s0, chi_task_s0_rdy, retry_s1, sleep_s1,
                                             w_wakeup_hit) = [](auto src) {
        auto [task_reg, chi_task_in, chi_task_in_rdy, chi_task_s0, chi_task_s0_rdy, retry_s1,
              sleep_s1, w_wakeup_hit] = src;
        EntryReg n = task_reg;
        switch (task_reg.state) {
            case taskst::kFree:
                if (chi_task_in.valid && chi_task_in_rdy) {
                    n.state = taskst::kSend;
                    n.chi = chi_task_in.bits.chi;
                    n.addr = chi_task_in.bits.addr;
                    n.qos = chi_task_in.bits.qos;
                }
                break;
            case taskst::kSend:
                if (chi_task_s0.valid && chi_task_s0_rdy) n.state = taskst::kWait;
                break;
            case taskst::kWait:
                if (w_wakeup_hit) {
                    n.state = taskst::kSend;
                } else if (sleep_s1) {
                    n.state = taskst::kSleep;
                } else if (retry_s1) {
                    n.state = taskst::kSend;
                } else {
                    n.state = taskst::kFree;
                }
                break;
            case taskst::kSleep:
                if (w_wakeup_hit) n.state = taskst::kSend;
                break;
            default: break;
        }
        return n;
    };
    // 状态输出
    st_valid.assign().reads(task_reg) = [](auto src) {
        auto [task_reg] = src;
        return task_reg.state != taskst::kFree;
    };
    valid_d1.update().on(posedge(clk)).reads(task_reg) = [](auto src) {
        auto [task_reg] = src;
        return task_reg.state != taskst::kFree;
    };
    st_release.assign().reads(valid_d1, task_reg) = [](auto src) {
        auto [valid_d1, task_reg] = src;
        return valid_d1 && task_reg.state == taskst::kFree;
    };
    st_addr.assign().reads(task_reg) = [](auto src) {
        auto [task_reg] = src;
        return task_reg.addr;
    };
    st_value.assign().reads(task_reg) = [](auto src) {
        auto [task_reg] = src;
        return task_reg.state;
    };
    st_nid = nid_reg;
    st_lock.assign().reads(task_reg, timeout_reg) = [](auto src) {
        auto [task_reg, timeout_reg] = src;
        return (task_reg.state == taskst::kSend || task_reg.state == taskst::kWait) &&
               timeout_reg;
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

// ---------------- PosEntry ----------------

PosEntry::PosEntry() {
    // alloc/updTag/clean 驱动的状态更新
    state_reg.update().on(posedge(clk)).reads(
        state_reg, alloc_valid, alloc_addr, alloc_addr_val, alloc_channel, upd_tag, clean,
        hn_idx) = [](auto src) {
        auto [state_reg, alloc_valid, alloc_addr, alloc_addr_val, alloc_channel, upd_tag,
              clean, hn_idx] = src;
        PosState n = state_reg;
        const bool updTagHit = upd_tag.valid && upd_tag.bits.hnIdx == hn_idx;
        const bool cleanHit = clean.valid && clean.bits.hnIdx == hn_idx;
        if (alloc_valid) {
            n.tagVal = alloc_addr_val;
            n.tag = posTagOf(alloc_addr);
            n.offset = static_cast<uint8_t>(alloc_addr & 0x3F);
        } else if (updTagHit) {
            n.tagVal = upd_tag.bits.addrVal;
            n.tag = posTagOf(upd_tag.bits.addr);
            n.offset = static_cast<uint8_t>(upd_tag.bits.addr & 0x3F);
        }
        if (cleanHit && clean.bits.channel == 0) {
            n.req = false;
        } else if (alloc_valid && alloc_channel == 0) {
            n.req = true;
        }
        if (cleanHit && clean.bits.channel == 3) {
            n.snp = false;
        } else if (alloc_valid && alloc_channel == 3) {
            n.snp = true;
        }
        // 更新条件：alloc | updTagHit | cleanHit（RTL 同）
        return (alloc_valid || updTagHit || cleanHit) ? n : state_reg;
    };
    // wakeup = RegNext(cleanHit & one & tagVal)
    wakeup_reg.update().on(posedge(clk)).reads(state_reg, clean, hn_idx) = [](auto src) {
        auto [state_reg, clean, hn_idx] = src;
        const bool cleanHit = clean.valid && clean.bits.hnIdx == hn_idx;
        return cleanHit && state_reg.one() && state_reg.tagVal;
    };
    wakeup_addr_reg.update().on(posedge(clk)).reads(state_reg, clean, hn_idx, cfg_bank_id,
                                                    wakeup_addr_reg) = [](auto src) {
        auto [state_reg, clean, hn_idx, cfg_bank_id, wakeup_addr_reg] = src;
        const bool cleanHit = clean.valid && clean.bits.hnIdx == hn_idx;
        if (cleanHit && state_reg.one() && state_reg.tagVal)
            return catPosAddr(cfg_bank_id, state_reg.tag, hnIdxPosSet(hn_idx),
                              hnIdxDirBank(hn_idx));
        return wakeup_addr_reg;
    };
    wakeup.assign().reads(wakeup_reg, wakeup_addr_reg) = [](auto src) {
        auto [wakeup_reg, wakeup_addr_reg] = src;
        return Valid<uint64_t>{wakeup_reg, wakeup_addr_reg};
    };
    state = state_reg;
    state_addr.assign().reads(state_reg, hn_idx, cfg_bank_id) = [](auto src) {
        auto [state_reg, hn_idx, cfg_bank_id] = src;
        return catPosAddr(cfg_bank_id, state_reg.tag, hnIdxPosSet(hn_idx),
                          hnIdxDirBank(hn_idx), state_reg.offset);
    };
}

// ---------------- PosSet ----------------

PosSet::PosSet() {
    for (uint32_t i = 0; i < 16; ++i) {
        auto& e = entries[i];
        e.clk = clk;
        e.cfg_bank_id = cfg_bank_id;
        e.hn_idx.assign().reads(dir_bank, pos_set) = [i](auto src) {
            auto [dir_bank, pos_set] = src;
            return hnIdxOf(dir_bank, pos_set, i);
        };
        e.upd_tag = upd_tag;
        e.clean = clean;
    }
    combine(w_states, entries,
            [](PosEntry& e) -> wolvicmod::Out<PosState>& { return e.state; });
    combine(w_addrs, entries,
            [](PosEntry& e) -> wolvicmod::Out<uint64_t>& { return e.state_addr; });

    // s0：matTag / free / block
    w_mat_tag_vec.assign().reads(w_states, alloc_s0_addr) = [](auto src) -> uint32_t {
        auto [states, alloc_s0_addr] = src;
        const uint64_t tag = posTagOf(alloc_s0_addr);
        uint32_t v = 0;
        for (uint32_t i = 0; i < 16; ++i)
            if (states[i].valid() && states[i].tagVal && states[i].tag == tag) v |= (1u << i);
        return v;
    };
    w_free_vec.assign().reads(w_states, alloc_reg_s1, alloc_way_reg_s1) =
        [](auto src) -> uint32_t {
            auto [states, alloc_reg_s1, alloc_way_reg_s1] = src;
            const uint32_t useWay = alloc_reg_s1.valid ? ~(1u << alloc_way_reg_s1) : 0xFFFFu;
            uint32_t v = 0;
            for (uint32_t i = 0; i < 16; ++i)
                if (!states[i].valid()) v |= (1u << i);
            return v & useWay;
        };
    w_block_s0.assign().reads(w_mat_tag_vec, w_free_vec, alloc_s0_valid, alloc_s0_addr,
                              alloc_s0_channel, alloc_reg_s1, lock_reg, req_pos_valid) =
        [](auto src) {
            auto [w_mat_tag_vec, w_free_vec, alloc_s0_valid, alloc_s0_addr, alloc_s0_channel,
                  alloc_reg_s1, lock_reg, req_pos_valid] = src;
            const bool hasMatTag = w_mat_tag_vec != 0;
            const bool hasFree = (w_free_vec & 0x3FFFu) != 0;  // way0-13 有 free
            const bool blockReq = hasMatTag || !hasFree;
            const bool blockSnp = hasMatTag || !hasFree;  // 无 BBN canNest 恒 false
            const bool matchReqS1 = alloc_reg_s1.valid &&
                                    posTagOf(alloc_s0_addr) == posTagOf(alloc_reg_s1.addr);
            const bool isSnp = alloc_s0_channel == 3;
            return (isSnp ? blockSnp : blockReq) || matchReqS1 || lock_reg || req_pos_valid;
        };

    // s1 寄存
    alloc_reg_s1.update().on(posedge(clk)).reads(alloc_s0_valid, w_block_s0, alloc_s0_addr,
                                                alloc_s0_channel, alloc_reg_s1) =
        [](auto src) {
            auto [alloc_s0_valid, w_block_s0, alloc_s0_addr, alloc_s0_channel,
                  alloc_reg_s1] = src;
            AllocS1 n = alloc_reg_s1;
            n.valid = alloc_s0_valid && !w_block_s0;
            if (alloc_s0_valid) {
                n.addr = alloc_s0_addr;
                n.channel = alloc_s0_channel;
            }
            return n;
        };
    alloc_way_reg_s1.update().on(posedge(clk)).reads(alloc_s0_valid, w_free_vec,
                                                    alloc_way_reg_s1) =
        [](auto src) -> uint8_t {
            auto [alloc_s0_valid, w_free_vec, alloc_way_reg_s1] = src;
            if (!alloc_s0_valid) return alloc_way_reg_s1;
            // freeWay_s0 = PriorityEncoder(freeVec)
            for (uint32_t i = 0; i < 16; ++i)
                if ((w_free_vec >> i) & 1u) return static_cast<uint8_t>(i);
            return 0;
        };
    sleep_reg.update().on(posedge(clk)).reads(alloc_s0_valid, w_mat_tag_vec) = [](auto src) {
        auto [alloc_s0_valid, w_mat_tag_vec] = src;
        return alloc_s0_valid && w_mat_tag_vec != 0;
    };
    block_reg.update().on(posedge(clk)).reads(alloc_s0_valid, w_block_s0) = [](auto src) {
        auto [alloc_s0_valid, w_block_s0] = src;
        return alloc_s0_valid && w_block_s0;
    };
    hn_idx_valid_reg.update().on(posedge(clk)).reads(alloc_s0_valid) = [](auto src) {
        auto [alloc_s0_valid] = src;
        return alloc_s0_valid;
    };
    sleep_s1 = sleep_reg;
    block_s1.assign().reads(block_reg, req_pos_valid) = [](auto src) {
        auto [block_reg, req_pos_valid] = src;
        return block_reg || req_pos_valid;
    };
    hn_idx_s1.assign().reads(hn_idx_valid_reg, req_pos_valid, alloc_way_reg_s1, dir_bank,
                             pos_set) =
        [](auto src) {
            auto [hn_idx_valid_reg, req_pos_valid, alloc_way_reg_s1, dir_bank, pos_set] = src;
            return Valid<uint8_t>{hn_idx_valid_reg && !req_pos_valid,
                                  hnIdxOf(dir_bank, pos_set, alloc_way_reg_s1)};
        };

    // reqPoS：replSelWay / reqPosFire / posResp / lockReg
    w_free_vec2.assign().reads(w_states) = [](auto src) -> uint32_t {
        auto [states] = src;
        uint32_t v = 0;
        for (uint32_t i = 0; i < 16; ++i)
            if (!states[i].valid()) v |= (1u << i);
        return v;
    };
    w_repl_sel_way.assign().reads(w_free_vec2, req_pos_channel) = [](auto src) -> uint8_t {
        auto [free_vec, req_pos_channel] = src;
        const bool isReq = req_pos_channel == 0;
        const bool isSnp = req_pos_channel == 3;
        if (isReq && ((free_vec >> 15) & 1u)) return 15;
        if (isSnp && ((free_vec >> 14) & 1u)) return 14;
        for (uint32_t i = 0; i < 14; ++i)
            if ((free_vec >> i) & 1u) return static_cast<uint8_t>(i);
        return 0;
    };
    w_req_pos_fire.assign().reads(req_pos_valid, w_free_vec2, w_repl_sel_way, lock_reg) =
        [](auto src) {
            auto [req_pos_valid, free_vec, sel_way, lock_reg] = src;
            return req_pos_valid && ((free_vec >> sel_way) & 1u) && !lock_reg;
        };
    pos_resp_valid_reg.update().on(posedge(clk)).reads(w_req_pos_fire) = [](auto src) {
        auto [w_req_pos_fire] = src;
        return w_req_pos_fire;
    };
    pos_resp_way_reg.update().on(posedge(clk)).reads(w_req_pos_fire, w_repl_sel_way,
                                                     pos_resp_way_reg) = [](auto src) {
        auto [w_req_pos_fire, w_repl_sel_way, pos_resp_way_reg] = src;
        return w_req_pos_fire ? w_repl_sel_way : pos_resp_way_reg;
    };
    pos_resp.assign().reads(pos_resp_valid_reg, pos_resp_way_reg) = [](auto src) {
        auto [pos_resp_valid_reg, pos_resp_way_reg] = src;
        return Valid<uint8_t>{pos_resp_valid_reg, pos_resp_way_reg};
    };
    lock_reg.update().on(posedge(clk)).reads(lock_reg, w_req_pos_fire, upd_tag, dir_bank,
                                             pos_set) = [](auto src) {
        auto [lock_reg, w_req_pos_fire, upd_tag, dir_bank, pos_set] = src;
        if (w_req_pos_fire) return true;
        if (upd_tag.valid && hnIdxDirBank(upd_tag.bits.hnIdx) == dir_bank &&
            hnIdxPosSet(upd_tag.bits.hnIdx) == pos_set)
            return false;
        return lock_reg;
    };

    // entry alloc 驱动（s1 拍或 reqPoS 拍）
    for (uint32_t i = 0; i < 16; ++i) {
        auto& e = entries[i];
        e.alloc_valid.assign().reads(req_pos_valid, w_req_pos_fire, w_repl_sel_way,
                                     alloc_reg_s1, retry_s1, alloc_way_reg_s1) =
            [i](auto src) {
                auto [req_pos_valid, w_req_pos_fire, w_repl_sel_way, alloc_reg_s1, retry_s1,
                      alloc_way_reg_s1] = src;
                if (req_pos_valid) return w_req_pos_fire && w_repl_sel_way == i;
                return alloc_reg_s1.valid && !retry_s1 && alloc_way_reg_s1 == i;
            };
        e.alloc_addr_val.assign().reads(req_pos_valid) = [](auto src) {
            auto [req_pos_valid] = src;
            return !req_pos_valid;
        };
        e.alloc_addr.assign().reads(req_pos_valid, alloc_reg_s1) = [](auto src) {
            auto [req_pos_valid, alloc_reg_s1] = src;
            return req_pos_valid ? 0ull : alloc_reg_s1.addr;
        };
        e.alloc_channel.assign().reads(req_pos_valid, req_pos_channel, alloc_reg_s1) =
            [](auto src) {
                auto [req_pos_valid, req_pos_channel, alloc_reg_s1] = src;
                return req_pos_valid ? req_pos_channel : alloc_reg_s1.channel;
            };
    }
    // wakeup Mux1H
    combine(w_wakeup_entries, entries,
            [](PosEntry& e) -> wolvicmod::Out<Valid<uint64_t>>& { return e.wakeup; });
    wakeup.assign().reads(w_wakeup_entries) = [](auto src) {
        auto [ws] = src;
        for (const auto& w : ws)
            if (w.valid) return w;
        return Valid<uint64_t>{false, 0};
    };
    state_vec = w_states;
    addr_vec = w_addrs;
}

}  // namespace zj::dj

namespace zj::dj {

// ---------------- PosTable ----------------

PosTable::PosTable() {
    for (uint32_t i = 0; i < 4; ++i) {
        auto& s = sets[i];
        s.clk = clk;
        s.cfg_bank_id = cfg_bank_id;
        s.dir_bank = dir_bank;
        s.pos_set = static_cast<uint8_t>(i);
        s.alloc_s0_valid.assign().reads(alloc_s0_valid, alloc_s0_addr) = [i](auto src) {
            auto [alloc_s0_valid, alloc_s0_addr] = src;
            return alloc_s0_valid && posSetOf(alloc_s0_addr) == i;
        };
        s.alloc_s0_addr = alloc_s0_addr;
        s.alloc_s0_channel = alloc_s0_channel;
        s.retry_s1 = retry_s1;
        s.req_pos_valid.assign().reads(req_pos_vec) = [i](auto src) {
            auto [req_pos_vec] = src;
            return req_pos_vec[i].valid;
        };
        s.req_pos_channel.assign().reads(req_pos_vec) = [i](auto src) {
            auto [req_pos_vec] = src;
            return req_pos_vec[i].bits.channel;
        };
        s.upd_tag = upd_tag;
        s.clean = clean;
    }
    combine(w_sleep_all, sets, [](PosSet& s) -> wolvicmod::Out<bool>& { return s.sleep_s1; });
    combine(w_block_all, sets, [](PosSet& s) -> wolvicmod::Out<bool>& { return s.block_s1; });
    combine(w_hn_valid_all, sets,
            [](PosSet& s) -> wolvicmod::Out<Valid<uint8_t>>& { return s.hn_idx_s1; });
    combine(w_wakeup_all, sets,
            [](PosSet& s) -> wolvicmod::Out<Valid<uint64_t>>& { return s.wakeup; });
    sleep_s1.assign().reads(w_sleep_all) = [](auto src) {
        auto [v] = src;
        for (bool x : v) {
            if (x) return true;
        }
        return false;
    };
    block_s1.assign().reads(w_block_all) = [](auto src) {
        auto [v] = src;
        for (bool x : v) {
            if (x) return true;
        }
        return false;
    };
    hn_idx_s1.assign().reads(w_hn_valid_all) = [](auto src) {
        auto [v] = src;
        for (const auto& x : v)
            if (x.valid) return x.bits;
        return static_cast<uint8_t>(0);
    };
    hn_idx_s1_valid.assign().reads(w_hn_valid_all) = [](auto src) {
        auto [v] = src;
        for (const auto& x : v) {
            if (x.valid) return true;
        }
        return false;
    };
    wakeup.assign().reads(w_wakeup_all) = [](auto src) {
        auto [v] = src;
        for (const auto& x : v)
            if (x.valid) return x;
        return Valid<uint64_t>{false, 0};
    };
    combine(w_pos_resp_all, sets,
            [](PosSet& s) -> wolvicmod::Out<Valid<uint8_t>>& { return s.pos_resp; });
    pos_resp_vec = w_pos_resp_all;
    // alrUsePoS / working / addr_vec2
    addr_vec2.assign().reads(sets[0].addr_vec) = [this](auto src) {
        auto [a0] = src;
        AddrVec2 r;
        r[0] = a0;
        for (uint32_t i = 1; i < 4; ++i) r[i] = sets[i].addr_vec.get();
        return r;
    };
    alr_use_pos.assign().reads(sets[0].state_vec) = [this](auto src) -> uint8_t {
        auto [s0] = src;
        uint8_t cnt = 0;
        for (const auto& st : s0) cnt += st.valid();
        for (uint32_t i = 1; i < 4; ++i)
            for (const auto& st : sets[i].state_vec.get()) cnt += st.valid();
        return cnt;
    };
    working.assign().reads(sets[0].state_vec) = [this](auto src) {
        auto [s0] = src;
        for (const auto& st : s0)
            if (st.valid()) return true;
        for (uint32_t i = 1; i < 4; ++i)
            for (const auto& st : sets[i].state_vec.get())
                if (st.valid()) return true;
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
