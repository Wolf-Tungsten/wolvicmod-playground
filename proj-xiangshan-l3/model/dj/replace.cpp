#include <wolvicmod/wolvicmod.h>

#include "model/dj/replace.h"

namespace zj::dj {

namespace dc = dectab;

namespace {
constexpr uint8_t kChReq = 0, kChDat = 1, kChSnp = 3;
constexpr uint8_t kFullVec = 0x3;
// ReqOpcode
constexpr uint8_t kWriteNoSnpFull = 0x1d, kWriteBackFull = 0x1b, kWriteEvictOrEvict = 0x42;
// SnpOpcode
constexpr uint8_t kSnpUnique = 0x17;
// ChiState
constexpr uint8_t kStI = 0, kStSC = 1, kStUD = 2, kStUC = 3;
// ChiResp
constexpr uint8_t kRespI = 0, kRespSC = 1, kRespUC = 2, kRespUdPd = 6;
}  // namespace

ReplaceEntry::ReplaceEntry() {
    alloc_rdy.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        return reg.state == replst::kFree;
    };

    // ---- reqPoS / posResp ----
    req_pos.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        ReplReqPos r;
        const uint8_t db = hnIdxDirBank(reg.hnTxnID);
        const uint8_t ps = hnIdxPosSet(reg.hnTxnID);
        r.hnIdx = hnIdxOf(db, ps, 0);  // pos.way = DontCare
        r.channel = (reg.wriSF && !reg.dir.sf.hit && !reg.directAllocSF) ? kChSnp : kChReq;
        return Valid<ReplReqPos>{reg.state == replst::kReqPos, r};
    };
    w_pos_resp_hit.assign().reads(reg, pos_resp) = [](auto src) {
        auto [reg, pos_resp] = src;
        const uint8_t db = hnIdxDirBank(reg.hnTxnID);
        const uint8_t ps = hnIdxPosSet(reg.hnTxnID);
        return reg.state == replst::kWaitPos && pos_resp[db][ps].valid;
    };

    // ---- writeDir ----
    write_dir.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        DirWrBoth w;
        w.llcValid = reg.wriLLC;
        w.llc.addr = reg.hnTxnID;  // hnTxnID 低 7 位即 pos 地址切片（RTL 直接位拼接）
        w.llc.wayOH = reg.dir.llc.wayOH;
        w.llc.hit = reg.dir.llc.hit;
        w.llc.meta = reg.dir.llc.meta;
        w.llc.hnIdx = reg.hnTxnID & 0x7F;
        w.llc.directAlloc = false;
        w.sfValid = reg.wriSF;
        w.sf.addr = reg.hnTxnID;
        w.sf.wayOH = reg.dir.sf.wayOH;
        w.sf.hit = reg.dir.sf.hit;
        w.sf.meta = reg.dir.sf.meta;
        w.sf.hnIdx = reg.hnTxnID & 0x7F;
        w.sf.directAlloc = reg.wriSF && !reg.dir.sf.hit && reg.directAllocSF;
        return Valid<DirWrBoth>{reg.state == replst::kWriDir, w};
    };

    // ---- respDir 命中 ----
    w_sf_resp_hit.assign().reads(reg, resp_dir_sf) = [](auto src) {
        auto [reg, resp_dir_sf] = src;
        return reg.state == replst::kWaitDir && resp_dir_sf.valid &&
               resp_dir_sf.bits.hnTxnID == reg.hnTxnID;
    };
    w_llc_resp_hit.assign().reads(reg, resp_dir_llc) = [](auto src) {
        auto [reg, resp_dir_llc] = src;
        return reg.state == replst::kWaitDir && resp_dir_llc.valid &&
               resp_dir_llc.bits.hnTxnID == reg.hnTxnID;
    };
    upd_pos_tag.assign().reads(w_sf_resp_hit, w_llc_resp_hit, resp_dir_sf, resp_dir_llc, reg) =
        [](auto src) {
            auto [w_sf_resp_hit, w_llc_resp_hit, resp_dir_sf, resp_dir_llc, reg] = src;
            UpdPosTag u;
            u.addrVal = w_sf_resp_hit ? resp_dir_sf.bits.meta != 0 : resp_dir_llc.bits.meta != 0;
            u.addr = w_sf_resp_hit ? resp_dir_sf.bits.addr : resp_dir_llc.bits.addr;
            u.hnIdx = reg.replHnTxnID & 0x7F;
            return Valid<UpdPosTag>{w_sf_resp_hit || w_llc_resp_hit, u};
        };

    // ---- cmTask 两路 ----
    cm_task_snp.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        CMTask t{};
        t.chi.channel = kChSnp;
        t.chi.opcode = kSnpUnique;
        t.chi.dataVec = kFullVec;
        t.chi.retToSrc = true;
        t.chi.size = 6;
        t.hnTxnID = reg.replHnTxnID;
        t.snpVec = reg.dir.sf.meta != 0 ? 1 : 0;
        t.fromRepl = true;
        t.qos = reg.qos;
        return Valid<CMTask>{reg.state == replst::kSnoop, t};
    };
    cm_task_wri.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        CMTask t{};
        t.chi.channel = kChReq;
        const bool dirty = reg.dir.llc.meta == kStUD;
        t.chi.opcode = reg.replToLan ? kWriteNoSnpFull
                                     : (dirty ? kWriteBackFull : kWriteEvictOrEvict);
        t.chi.dataVec = kFullVec;
        t.chi.memAttr = 0b0101;  // allocate=0 device=0 cacheable=1 ewa=1（先声明 MSB）
        t.chi.toLAN = reg.replToLan;
        t.chi.size = 6;
        t.hnTxnID = reg.replHnTxnID;
        t.fromRepl = true;
        t.ds = reg.ds;
        t.cbResp = reg.replToLan ? kRespI
                                 : (reg.dir.llc.meta == kStI    ? kRespI
                                    : reg.dir.llc.meta == kStSC ? kRespSC
                                    : reg.dir.llc.meta == kStUC ? kRespUC
                                                                : kRespUdPd);
        t.dataOp.repl = true;
        t.qos = reg.qos;
        return Valid<CMTask>{reg.state == replst::kWrite, t};
    };

    // ---- reqDB / updHnTxnID / dataTask / cleanPoS / resp ----
    req_db.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        ReqDBQos r;
        r.hnTxnID = reg.replHnTxnID;
        r.dataVec = kFullVec;
        r.qos = reg.qos;
        return Valid<ReqDBQos>{reg.state == replst::kReqDB, r};
    };
    upd_hn_txn_id.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        UpdHnTxnID u;
        u.before = reg.hnTxnID;
        u.next = reg.replHnTxnID;
        return Valid<UpdHnTxnID>{reg.state == replst::kUpdateId, u};
    };
    data_task.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        DataTask t{};
        t.hnTxnID = reg.hnTxnID;
        t.dataOp.save = true;
        t.dataVec = kFullVec;
        t.ds = reg.ds;
        t.qos = reg.qos;
        return Valid<DataTask>{reg.state == replst::kSaveData, t};
    };
    clean_pos.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        PosClean p;
        const bool isT = reg.state == replst::kCleanPosT;
        p.hnIdx = isT ? reg.hnTxnID & 0x7F : reg.replHnTxnID & 0x7F;
        p.channel = isT ? kChSnp
                        : ((reg.wriSF && !reg.dir.sf.hit && !reg.directAllocSF) ? kChSnp
                                                                               : kChReq);
        p.qos = reg.qos;
        return Valid<PosClean>{reg.state == replst::kCleanPosT ||
                                   reg.state == replst::kCleanPosR,
                               p};
    };
    resp.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        return Valid<uint8_t>{reg.state == replst::kRespCmt, reg.hnTxnID};
    };

    // ---- 命中派生 ----
    w_cm_resp_hit.assign().reads(reg, cm_resp) = [](auto src) {
        auto [reg, cm_resp] = src;
        return reg.state != replst::kFree && cm_resp.valid &&
               cm_resp.bits.hnTxnID == reg.replHnTxnID;
    };
    w_data_resp_hit.assign().reads(reg, data_resp) = [](auto src) {
        auto [reg, data_resp] = src;
        return reg.state != replst::kFree && data_resp.valid && data_resp.bits == reg.hnTxnID;
    };
    w_wri_dir_done_hit.assign().reads(reg, write_dir_done) = [](auto src) {
        auto [reg, write_dir_done] = src;
        return reg.state == replst::kWaitWriDir && write_dir_done.valid &&
               write_dir_done.bits == reg.hnTxnID;
    };

    // ---- 次态 ----
    w_next.assign().reads(reg, alloc, alloc_rdy, req_pos, req_pos_rdy, w_pos_resp_hit, pos_resp,
                          write_dir, write_dir_rdy, w_sf_resp_hit, w_llc_resp_hit, resp_dir_sf,
                          resp_dir_llc, cfg_ci, upd_hn_txn_id, upd_hn_txn_id_rdy, resp,
                          resp_rdy, req_db, req_db_rdy, cm_task_wri, cm_task_wri_rdy,
                          cm_task_snp, cm_task_snp_rdy, w_cm_resp_hit, cm_resp, data_task,
                          data_task_rdy, w_data_resp_hit, clean_pos, clean_pos_rdy,
                          w_wri_dir_done_hit) = [](auto src) {
        auto [reg, alloc, alloc_rdy, req_pos, req_pos_rdy, w_pos_resp_hit, pos_resp, write_dir,
              write_dir_rdy, w_sf_resp_hit, w_llc_resp_hit, resp_dir_sf, resp_dir_llc, cfg_ci,
              upd_hn_txn_id, upd_hn_txn_id_rdy, resp, resp_rdy, req_db, req_db_rdy, cm_task_wri,
              cm_task_wri_rdy, cm_task_snp, cm_task_snp_rdy, w_cm_resp_hit, cm_resp, data_task,
              data_task_rdy, w_data_resp_hit, clean_pos, clean_pos_rdy,
              w_wri_dir_done_hit] = src;
        ReplReg n = reg;
        const bool isReplDIR = (reg.wriSF && !reg.dir.sf.hit && !reg.directAllocSF) ||
                               (reg.wriLLC && !reg.dir.llc.hit);
        const bool isReplSF = reg.wriSF && !reg.dir.sf.hit && !reg.directAllocSF;
        const bool isReplLLC = reg.wriLLC && !reg.dir.llc.hit;
        const uint8_t db = hnIdxDirBank(reg.hnTxnID);
        const uint8_t ps = hnIdxPosSet(reg.hnTxnID);

        // 状态机
        switch (reg.state) {
            case replst::kFree:
                if (alloc.valid && alloc_rdy) {
                    const bool aReplDIR = (alloc.bits.wriSF && !alloc.bits.dir.sf.hit &&
                                           !alloc.bits.directAllocSF) ||
                                          (alloc.bits.wriLLC && !alloc.bits.dir.llc.hit);
                    n.state = aReplDIR ? replst::kReqPos : replst::kWriDir;
                }
                break;
            case replst::kReqPos:
                if (req_pos.valid && req_pos_rdy) n.state = replst::kWaitPos;
                break;
            case replst::kWaitPos: n.state = w_pos_resp_hit ? replst::kWriDir : replst::kReqPos; break;
            case replst::kWriDir:
                if (write_dir.valid && write_dir_rdy) {
                    const bool isDirectAllocSF =
                        reg.wriSF && !reg.dir.sf.hit && reg.directAllocSF;
                    n.state = isReplDIR ? replst::kWaitDir
                                        : (isDirectAllocSF ? replst::kWaitWriDir
                                                           : replst::kRespCmt);
                }
                break;
            case replst::kWaitWriDir:
                if (w_wri_dir_done_hit) n.state = replst::kRespCmt;
                break;
            case replst::kWaitDir:
                if (w_sf_resp_hit || w_llc_resp_hit) {
                    if (w_sf_resp_hit) {
                        n.state = replst::kRespCmt;
                    } else {
                        const bool needReplLLC = resp_dir_llc.bits.meta != 0;
                        const bool toLan = ciOf(resp_dir_llc.bits.addr) == cfg_ci;
                        const bool dirty = resp_dir_llc.bits.meta == kStUD;
                        const bool localClean = toLan && !dirty;
                        n.state = needReplLLC ? (localClean ? replst::kSaveData
                                                            : replst::kUpdateId)
                                              : replst::kSaveData;
                    }
                }
                break;
            case replst::kUpdateId:
                if (upd_hn_txn_id.valid && upd_hn_txn_id_rdy) n.state = replst::kWrite;
                break;
            case replst::kRespCmt:
                if (resp.valid && resp_rdy) {
                    if (isReplSF) {
                        n.state = reg.needSnp ? replst::kReqDB : replst::kCleanPosR;
                    } else if (isReplLLC) {
                        n.state = replst::kCleanPosR;
                    } else {
                        n.state = replst::kFree;
                    }
                }
                break;
            case replst::kReqDB:
                if (req_db.valid && req_db_rdy) n.state = replst::kSnoop;
                break;
            case replst::kWrite:
                if (cm_task_wri.valid && cm_task_wri_rdy) n.state = replst::kWaitRWri;
                break;
            case replst::kSnoop:
                if (cm_task_snp.valid && cm_task_snp_rdy) n.state = replst::kWaitRSnp;
                break;
            case replst::kWaitRWri:
                if (w_cm_resp_hit) n.state = reg.alrReplSF ? replst::kCleanPosT : replst::kRespCmt;
                break;
            case replst::kWaitRSnp:
                if (w_cm_resp_hit) {
                    const bool cmRespData =
                        dc::tiValid(cm_resp.bits.taskInst) && dc::tiChannel(cm_resp.bits.taskInst) == kChDat;
                    n.state = cmRespData ? replst::kCopyId : replst::kCleanPosR;
                }
                break;
            case replst::kCopyId: n.state = replst::kReqPos; break;
            case replst::kSaveData:
                if (data_task.valid && data_task_rdy) n.state = replst::kWaitResp;
                break;
            case replst::kWaitResp:
                if (w_data_resp_hit)
                    n.state = reg.alrReplSF ? replst::kCleanPosT : replst::kRespCmt;
                break;
            case replst::kCleanPosT:
                if (clean_pos.valid && clean_pos_rdy) n.state = replst::kCleanPosR;
                break;
            case replst::kCleanPosR:
                if (clean_pos.valid && clean_pos_rdy) n.state = replst::kFree;
                break;
            default: break;
        }

        // hnTxnID：COPYID 换槽 / posResp 选中新槽
        if (reg.state == replst::kCopyId) {
            n.hnTxnID = reg.replHnTxnID;
        } else if (w_pos_resp_hit) {
            n.replHnTxnID = hnIdxOf(db, ps, pos_resp[db][ps].bits);
        }

        // llcRespHit 副作用：toLan / ds
        if (w_llc_resp_hit) {
            n.replToLan = ciOf(resp_dir_llc.bits.addr) == cfg_ci;
            DsIdx nds;
            nds.set(resp_dir_llc.bits.addr, ohToUInt(resp_dir_llc.bits.wayOH));
            n.ds = nds;
        }

        // sfRespHit 副作用
        if (w_sf_resp_hit) {
            n.needSnp = resp_dir_sf.bits.meta != 0;
            n.alrReplSF = true;
            n.dir.sf.wayOH = resp_dir_sf.bits.wayOH;
            n.dir.sf.meta = resp_dir_sf.bits.meta;
            n.dir.sf.hit = resp_dir_sf.bits.hit;
        } else if (n.state == replst::kFree) {
            n.needSnp = false;
            n.alrReplSF = false;
        }

        // cmRespData 副作用：改写为 wriLLC 任务
        const bool cmRespDataHit =
            w_cm_resp_hit && dc::tiValid(cm_resp.bits.taskInst) &&
            dc::tiChannel(cm_resp.bits.taskInst) == kChDat;
        if (cmRespDataHit) {
            const bool dirty = (dc::tiResp(cm_resp.bits.taskInst) >> 2) & 1;  // passDirty
            n.wriSF = false;
            n.wriLLC = true;
            n.dir.llc.hit = false;
            n.dir.llc.meta = dirty ? kStUD : kStSC;
        }
        return n;
    };
    w_set.assign().reads(reg, alloc, alloc_rdy) = [](auto src) {
        auto [reg, alloc, alloc_rdy] = src;
        return (alloc.valid && alloc_rdy) || reg.state != replst::kFree;
    };
    reg.update().on(posedge(clk)).reads(reg, w_next, w_set, alloc, alloc_rdy) = [](auto src) {
        auto [reg, w_next, w_set, alloc, alloc_rdy] = src;
        if (!w_set) return reg;
        if (alloc.valid && alloc_rdy) {
            ReplReg a{};
            a.dir = alloc.bits.dir;
            a.hnTxnID = alloc.bits.hnTxnID;
            a.qos = alloc.bits.qos;
            a.wriSF = alloc.bits.wriSF;
            a.wriLLC = alloc.bits.wriLLC;
            a.directAllocSF = alloc.bits.directAllocSF;
            a.replHnTxnID = alloc.bits.hnTxnID;
            a.state = w_next.state;  // 状态机已按 alloc 计算
            return a;
        }
        return w_next;
    };
}


// ---------------- ReplaceCM ----------------

ReplaceCM::ReplaceCM() {
    alloc_arb.clk = clk;
    resp_arb.clk = clk;
    upd_id_arb.clk = clk;
    upd_tag_arb.clk = clk;
    clean_arb.clk = clk;
    data_task_arb.clk = clk;
    wdir_arb.clk = clk;
    snp_arb.clk = clk;
    wri_arb.clk = clk;
    req_db_arb.clk = clk;
    for (uint32_t i = 0; i < 8; ++i) req_pos_arbs[i].clk = clk;

    for (uint32_t i = 0; i < kEntries; ++i) {
        auto& e = entries[i];
        e.clk = clk;
        e.cfg_ci = cfg_ci;
        e.cm_resp = cm_resp;
        e.resp_dir_llc = resp_dir_llc;
        e.resp_dir_sf = resp_dir_sf;
        e.data_resp = data_resp;
        e.pos_resp = pos_resp_vec;
        e.write_dir_done = write_dir_done;
    }

    // Alloc 池化
    alloc_arb.in = task;
    task_rdy = alloc_arb.in_rdy;
    for (uint32_t i = 0; i < kEntries; ++i) {
        entries[i].alloc.assign().reads(alloc_arb.out) = [i](auto src) {
            auto [out] = src;
            return out[i];
        };
        entries[i].alloc_rdy.assign().reads(alloc_arb.out_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
    }

    // reqPoS 矩阵
    combine(w_req_pos_in, entries,
            [](ReplaceEntry& e) -> wolvicmod::Out<Valid<ReplReqPos>>& { return e.req_pos; });
    for (uint32_t b = 0; b < 2; ++b) {
        for (uint32_t s = 0; s < 4; ++s) {
            const uint32_t m = b * 4 + s;
            auto& arb = req_pos_arbs[m];
            arb.in.assign().reads(w_req_pos_in, entries[0].reg) = [this, b, s](auto src) {
                auto [in, reg0] = src;
                (void)reg0;
                ReqPosInArr a;
                for (uint32_t i = 0; i < kEntries; ++i) {
                    const auto& r = entries[i].reg.get();
                    const bool hit = hnIdxDirBank(r.hnTxnID) == b && hnIdxPosSet(r.hnTxnID) == s;
                    a[i].valid = in[i].valid && hit;
                    a[i].bits = in[i].bits;
                }
                return a;
            };
            arb.out_rdy = true;
        }
    }
    combine(w_req_pos_out, req_pos_arbs,
            [](ReqPosArbT& a) -> wolvicmod::Out<Valid<ReplReqPos>>& { return a.out; });
    req_pos_vec.assign().reads(w_req_pos_out) = [](auto src) {
        auto [outs] = src;
        ReqPosArr r;
        for (uint32_t b = 0; b < 2; ++b)
            for (uint32_t s = 0; s < 4; ++s) r[b][s] = outs[b * 4 + s];
        return r;
    };
    for (uint32_t i = 0; i < kEntries; ++i) {
        entries[i].req_pos_rdy.assign().reads(
            req_pos_arbs[0].in_rdy, entries[i].reg) =
            [this, i](auto src) {
                auto [rdy0, r] = src;
                const uint32_t b = hnIdxDirBank(r.hnTxnID);
                const uint32_t s = hnIdxPosSet(r.hnTxnID);
                return req_pos_arbs[b * 4 + s].in_rdy.get()[i];
            };
    }

    // 输出汇集
    combine(w_resp_in, entries,
            [](ReplaceEntry& e) -> wolvicmod::Out<Valid<uint8_t>>& { return e.resp; });
    combine(w_upd_id_in, entries,
            [](ReplaceEntry& e) -> wolvicmod::Out<Valid<UpdHnTxnID>>& {
                return e.upd_hn_txn_id;
            });
    combine(w_upd_tag_in, entries,
            [](ReplaceEntry& e) -> wolvicmod::Out<Valid<UpdPosTag>>& { return e.upd_pos_tag; });
    combine(w_clean_in, entries,
            [](ReplaceEntry& e) -> wolvicmod::Out<Valid<PosClean>>& { return e.clean_pos; });
    combine(w_data_task_in, entries,
            [](ReplaceEntry& e) -> wolvicmod::Out<Valid<DataTask>>& { return e.data_task; });
    combine(w_wdir_in, entries,
            [](ReplaceEntry& e) -> wolvicmod::Out<Valid<DirWrBoth>>& { return e.write_dir; });
    combine(w_snp_in, entries,
            [](ReplaceEntry& e) -> wolvicmod::Out<Valid<CMTask>>& { return e.cm_task_snp; });
    combine(w_wri_in, entries,
            [](ReplaceEntry& e) -> wolvicmod::Out<Valid<CMTask>>& { return e.cm_task_wri; });
    combine(w_req_db_in, entries,
            [](ReplaceEntry& e) -> wolvicmod::Out<Valid<ReqDBQos>>& { return e.req_db; });

    resp_arb.in = w_resp_in;
    resp_arb.out_rdy = true;
    resp = resp_arb.out;
    upd_id_arb.in = w_upd_id_in;
    upd_id_arb.out_rdy = true;
    upd_hn_txn_id = upd_id_arb.out;
    upd_tag_arb.in = w_upd_tag_in;
    upd_tag_arb.out_rdy = true;
    upd_pos_tag = upd_tag_arb.out;
    clean_arb.in = w_clean_in;
    clean_arb.out_rdy = clean_pos_rdy;
    clean_pos = clean_arb.out;
    data_task_arb.in = w_data_task_in;
    data_task_arb.out_rdy = data_task_rdy;
    data_task = data_task_arb.out;
    wdir_arb.in = w_wdir_in;
    wdir_arb.out_rdy = write_dir_rdy;
    write_dir = wdir_arb.out;
    snp_arb.in = w_snp_in;
    snp_arb.out_rdy = cm_task_snp_rdy;
    cm_task_snp = snp_arb.out;
    wri_arb.in = w_wri_in;
    wri_arb.out_rdy = cm_task_wri_rdy;
    cm_task_wri = wri_arb.out;
    req_db_arb.in = w_req_db_in;
    req_db_arb.out_rdy = req_db_rdy;
    req_db.assign().reads(req_db_arb.out) = [](auto src) {
        auto [o] = src;
        return Valid<ReqDB>{o.valid, {o.bits.hnTxnID, o.bits.dataVec}};
    };

    for (uint32_t i = 0; i < kEntries; ++i) {
        entries[i].resp_rdy = true;
        entries[i].upd_hn_txn_id_rdy = true;
        entries[i].clean_pos_rdy.assign().reads(clean_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].data_task_rdy.assign().reads(data_task_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].write_dir_rdy.assign().reads(wdir_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].cm_task_snp_rdy.assign().reads(snp_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].cm_task_wri_rdy.assign().reads(wri_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].req_db_rdy.assign().reads(req_db_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
    }
}
}  // namespace zj::dj
