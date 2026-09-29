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

    // ---- Alloc 池化 ----
    w_alloc_rdy_all.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        RdyArrN r{};
        for (uint32_t i = 0; i < kEntries; ++i) r[i] = entries[i].state == replst::kFree;
        return r;
    };
    alloc_arb.in = task;
    task_rdy = alloc_arb.in_rdy;
    alloc_arb.out_rdy = w_alloc_rdy_all;

    // ---- per-entry 命中（upd_pos_tag 与次态共用；sf/llc 两路读集重叠，并一路） ----
    w_dir_hits.assign().reads(entries, resp_dir_sf, resp_dir_llc) = [](auto src) {
        auto [entries, resp_dir_sf, resp_dir_llc] = src;
        DirHits o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const bool waitDir = entries[i].state == replst::kWaitDir;
            o.sf[i] = waitDir && resp_dir_sf.valid &&
                      resp_dir_sf.bits.hnTxnID == entries[i].hnTxnID;
            o.llc[i] = waitDir && resp_dir_llc.valid &&
                       resp_dir_llc.bits.hnTxnID == entries[i].hnTxnID;
        }
        return o;
    };

    // ---- per-entry 输出数组（直喂仲裁器/reqPoS 矩阵） ----
    // reqPoS 矩阵输入：选中 id + 请求载荷（同为 reads(entries) 提取，并一路）
    w_req_pos_feed.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        ReqPosFeed o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const ReplReg& e = entries[i];
            o.ids[i] = e.hnTxnID;
            ReplReqPos r;
            const uint8_t db = hnIdxDirBank(e.hnTxnID);
            const uint8_t ps = hnIdxPosSet(e.hnTxnID);
            r.hnIdx = hnIdxOf(db, ps, 0);  // pos.way = DontCare
            r.channel = (e.wriSF && !e.dir.sf.hit && !e.directAllocSF) ? kChSnp : kChReq;
            o.in[i] = Valid<ReplReqPos>{e.state == replst::kReqPos, r};
        }
        return o;
    };
    w_resp_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        RespInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i)
            o[i] = Valid<uint8_t>{entries[i].state == replst::kRespCmt, entries[i].hnTxnID};
        return o;
    };
    w_upd_id_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        UpdIdInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const ReplReg& e = entries[i];
            UpdHnTxnID u;
            u.before = e.hnTxnID;
            u.next = e.replHnTxnID;
            o[i] = Valid<UpdHnTxnID>{e.state == replst::kUpdateId, u};
        }
        return o;
    };
    w_upd_tag_in.assign().reads(entries, w_dir_hits, resp_dir_sf,
                                resp_dir_llc) = [](auto src) {
        auto [entries, hits, resp_dir_sf, resp_dir_llc] = src;
        UpdTagInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const ReplReg& e = entries[i];
            UpdPosTag u;
            u.addrVal = hits.sf[i] ? resp_dir_sf.bits.meta != 0 : resp_dir_llc.bits.meta != 0;
            u.addr = hits.sf[i] ? resp_dir_sf.bits.addr : resp_dir_llc.bits.addr;
            u.hnIdx = e.replHnTxnID & 0x7F;
            o[i] = Valid<UpdPosTag>{hits.sf[i] || hits.llc[i], u};
        }
        return o;
    };
    w_clean_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        CleanInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const ReplReg& e = entries[i];
            PosClean p;
            const bool isT = e.state == replst::kCleanPosT;
            p.hnIdx = isT ? e.hnTxnID & 0x7F : e.replHnTxnID & 0x7F;
            p.channel = isT ? kChSnp
                            : ((e.wriSF && !e.dir.sf.hit && !e.directAllocSF) ? kChSnp : kChReq);
            p.qos = e.qos;
            o[i] = Valid<PosClean>{e.state == replst::kCleanPosT ||
                                       e.state == replst::kCleanPosR,
                                   p};
        }
        return o;
    };
    w_data_task_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        DataTaskInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const ReplReg& e = entries[i];
            DataTask t{};
            t.hnTxnID = e.hnTxnID;
            t.dataOp.save = true;
            t.dataVec = kFullVec;
            t.ds = e.ds;
            t.qos = e.qos;
            o[i] = Valid<DataTask>{e.state == replst::kSaveData, t};
        }
        return o;
    };
    w_wdir_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        WdirInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const ReplReg& e = entries[i];
            DirWrBoth w;
            w.llcValid = e.wriLLC;
            w.llc.addr = e.hnTxnID;  // hnTxnID 低 7 位即 pos 地址切片（RTL 直接位拼接）
            w.llc.wayOH = e.dir.llc.wayOH;
            w.llc.hit = e.dir.llc.hit;
            w.llc.meta = e.dir.llc.meta;
            w.llc.hnIdx = e.hnTxnID & 0x7F;
            w.llc.directAlloc = false;
            w.sfValid = e.wriSF;
            w.sf.addr = e.hnTxnID;
            w.sf.wayOH = e.dir.sf.wayOH;
            w.sf.hit = e.dir.sf.hit;
            w.sf.meta = e.dir.sf.meta;
            w.sf.hnIdx = e.hnTxnID & 0x7F;
            w.sf.directAlloc = e.wriSF && !e.dir.sf.hit && e.directAllocSF;
            o[i] = Valid<DirWrBoth>{e.state == replst::kWriDir, w};
        }
        return o;
    };
    w_snp_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        CmTaskInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const ReplReg& e = entries[i];
            CMTask t{};
            t.chi.channel = kChSnp;
            t.chi.opcode = kSnpUnique;
            t.chi.dataVec = kFullVec;
            t.chi.retToSrc = true;
            t.chi.size = 6;
            t.hnTxnID = e.replHnTxnID;
            t.snpVec = e.dir.sf.meta != 0 ? 1 : 0;
            t.fromRepl = true;
            t.qos = e.qos;
            o[i] = Valid<CMTask>{e.state == replst::kSnoop, t};
        }
        return o;
    };
    w_wri_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        CmTaskInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const ReplReg& e = entries[i];
            CMTask t{};
            t.chi.channel = kChReq;
            const bool dirty = e.dir.llc.meta == kStUD;
            t.chi.opcode = e.replToLan ? kWriteNoSnpFull
                                       : (dirty ? kWriteBackFull : kWriteEvictOrEvict);
            t.chi.dataVec = kFullVec;
            t.chi.memAttr = 0b0101;  // allocate=0 device=0 cacheable=1 ewa=1（先声明 MSB）
            t.chi.toLAN = e.replToLan;
            t.chi.size = 6;
            t.hnTxnID = e.replHnTxnID;
            t.fromRepl = true;
            t.ds = e.ds;
            t.cbResp = e.replToLan ? kRespI
                                   : (e.dir.llc.meta == kStI    ? kRespI
                                      : e.dir.llc.meta == kStSC ? kRespSC
                                      : e.dir.llc.meta == kStUC ? kRespUC
                                                                : kRespUdPd);
            t.dataOp.repl = true;
            t.qos = e.qos;
            o[i] = Valid<CMTask>{e.state == replst::kWrite, t};
        }
        return o;
    };
    w_req_db_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        ReqDbInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const ReplReg& e = entries[i];
            ReqDBQos r;
            r.hnTxnID = e.replHnTxnID;
            r.dataVec = kFullVec;
            r.qos = e.qos;
            o[i] = Valid<ReqDBQos>{e.state == replst::kReqDB, r};
        }
        return o;
    };

    // ---- reqPoS 矩阵 ----
    for (uint32_t b = 0; b < 2; ++b) {
        for (uint32_t s = 0; s < 4; ++s) {
            const uint32_t m = b * 4 + s;
            auto& arb = req_pos_arbs[m];
            arb.in.assign().reads(w_req_pos_feed) = [b, s](auto src) {
                auto [feed] = src;
                ReqPosInArr a;
                for (uint32_t i = 0; i < kEntries; ++i) {
                    const bool hit =
                        hnIdxDirBank(feed.ids[i]) == b && hnIdxPosSet(feed.ids[i]) == s;
                    a[i].valid = feed.in[i].valid && hit;
                    a[i].bits = feed.in[i].bits;
                }
                return a;
            };
            arb.out_rdy = true;
        }
    }
    // 8 路仲裁输出直接 reshape 为 (dirBank × posSet)（原 w_req_pos_out 中转线消除）
    req_pos_vec.assign().reads(req_pos_arbs[0].out, req_pos_arbs[1].out, req_pos_arbs[2].out,
                               req_pos_arbs[3].out, req_pos_arbs[4].out, req_pos_arbs[5].out,
                               req_pos_arbs[6].out, req_pos_arbs[7].out) =
        [](auto src) {
            auto [o0, o1, o2, o3, o4, o5, o6, o7] = src;
            const Valid<ReplReqPos>* outs[8] = {&o0, &o1, &o2, &o3, &o4, &o5, &o6, &o7};
            ReqPosArr r;
            for (uint32_t b = 0; b < 2; ++b)
                for (uint32_t s = 0; s < 4; ++s) r[b][s] = *outs[b * 4 + s];
            return r;
        };
    // per-entry reqPos rdy：按本项 (bank,set) 从对应仲裁器的 in_rdy 选
    w_req_pos_rdy_all.assign().reads(
        w_req_pos_feed, req_pos_arbs[0].in_rdy, req_pos_arbs[1].in_rdy, req_pos_arbs[2].in_rdy,
        req_pos_arbs[3].in_rdy, req_pos_arbs[4].in_rdy, req_pos_arbs[5].in_rdy,
        req_pos_arbs[6].in_rdy, req_pos_arbs[7].in_rdy) =
        [](auto src) {
            auto [feed, r0, r1, r2, r3, r4, r5, r6, r7] = src;
            const RdyArrN* rdys[8] = {&r0, &r1, &r2, &r3, &r4, &r5, &r6, &r7};
            RdyArrN o{};
            for (uint32_t i = 0; i < kEntries; ++i) {
                const uint32_t b = hnIdxDirBank(feed.ids[i]);
                const uint32_t s = hnIdxPosSet(feed.ids[i]);
                o[i] = (*rdys[b * 4 + s])[i];
            }
            return o;
        };

    // ---- 输出仲裁 ----
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

    // ---- 64 项状态（一条 update 循环；w_set = allocFire || 非空闲） ----
    // 读集不含各 w_*_in 输出数组：其次态只用到 valid，而 valid ≡ state==本态，
    // 由 switch 分支隐含（fire 条件退化为纯 rdy）。
    entries.update().on(posedge(clk)).reads(
        entries, alloc_arb.out, pos_resp_vec, w_dir_hits, resp_dir_sf, resp_dir_llc, cfg_ci,
        req_db_arb.in_rdy, wri_arb.in_rdy, snp_arb.in_rdy, cm_resp, data_task_arb.in_rdy,
        data_resp, clean_arb.in_rdy, write_dir_done, w_req_pos_rdy_all,
        wdir_arb.in_rdy) = [](auto src) {
        auto [entries, alloc_out, pos_resp, hits, resp_dir_sf, resp_dir_llc, cfg_ci, req_db_rdy,
              wri_rdy, snp_rdy, cm_resp, data_task_rdy, data_resp, clean_rdy, write_dir_done,
              req_pos_rdy, wdir_rdy] = src;
        EntryArr n = entries;
        for (uint32_t i = 0; i < kEntries; ++i) {
            const ReplReg& reg = entries[i];
            const bool allocFire = alloc_out[i].valid && reg.state == replst::kFree;
            if (!allocFire && reg.state == replst::kFree) continue;  // !w_set

            ReplReg next = reg;  // 原 w_next
            const bool isReplDIR = (reg.wriSF && !reg.dir.sf.hit && !reg.directAllocSF) ||
                                   (reg.wriLLC && !reg.dir.llc.hit);
            const bool isReplSF = reg.wriSF && !reg.dir.sf.hit && !reg.directAllocSF;
            const bool isReplLLC = reg.wriLLC && !reg.dir.llc.hit;
            const uint8_t db = hnIdxDirBank(reg.hnTxnID);
            const uint8_t ps = hnIdxPosSet(reg.hnTxnID);
            // 命中派生（原 w_*_hit 线）
            const bool posRespHit = reg.state == replst::kWaitPos && pos_resp[db][ps].valid;
            const bool cmRespHit = reg.state != replst::kFree && cm_resp.valid &&
                                   cm_resp.bits.hnTxnID == reg.replHnTxnID;
            const bool dataRespHit = reg.state != replst::kFree && data_resp.valid &&
                                     data_resp.bits == reg.hnTxnID;
            const bool wriDoneHit = reg.state == replst::kWaitWriDir && write_dir_done.valid &&
                                    write_dir_done.bits == reg.hnTxnID;

            // 状态机（fire 条件 = 输出 valid && 仲裁 rdy；输出 valid ≡ state==本态，
            // 由所在分支隐含，故只读 rdy；upd_id/resp 的 rdy 恒真）
            switch (reg.state) {
                case replst::kFree:
                    if (allocFire) {
                        const auto& ab = alloc_out[i].bits;
                        const bool aReplDIR = (ab.wriSF && !ab.dir.sf.hit && !ab.directAllocSF) ||
                                              (ab.wriLLC && !ab.dir.llc.hit);
                        next.state = aReplDIR ? replst::kReqPos : replst::kWriDir;
                    }
                    break;
                case replst::kReqPos:
                    if (req_pos_rdy[i]) next.state = replst::kWaitPos;
                    break;
                case replst::kWaitPos:
                    next.state = posRespHit ? replst::kWriDir : replst::kReqPos;
                    break;
                case replst::kWriDir:
                    if (wdir_rdy[i]) {
                        const bool isDirectAllocSF =
                            reg.wriSF && !reg.dir.sf.hit && reg.directAllocSF;
                        next.state = isReplDIR ? replst::kWaitDir
                                               : (isDirectAllocSF ? replst::kWaitWriDir
                                                                  : replst::kRespCmt);
                    }
                    break;
                case replst::kWaitWriDir:
                    if (wriDoneHit) next.state = replst::kRespCmt;
                    break;
                case replst::kWaitDir:
                    if (hits.sf[i] || hits.llc[i]) {
                        if (hits.sf[i]) {
                            next.state = replst::kRespCmt;
                        } else {
                            const bool needReplLLC = resp_dir_llc.bits.meta != 0;
                            const bool toLan = ciOf(resp_dir_llc.bits.addr) == cfg_ci;
                            const bool dirty = resp_dir_llc.bits.meta == kStUD;
                            const bool localClean = toLan && !dirty;
                            next.state = needReplLLC ? (localClean ? replst::kSaveData
                                                                   : replst::kUpdateId)
                                                     : replst::kSaveData;
                        }
                    }
                    break;
                case replst::kUpdateId: next.state = replst::kWrite; break;
                case replst::kRespCmt:
                    if (isReplSF) {
                        next.state = reg.needSnp ? replst::kReqDB : replst::kCleanPosR;
                    } else if (isReplLLC) {
                        next.state = replst::kCleanPosR;
                    } else {
                        next.state = replst::kFree;
                    }
                    break;
                case replst::kReqDB:
                    if (req_db_rdy[i]) next.state = replst::kSnoop;
                    break;
                case replst::kWrite:
                    if (wri_rdy[i]) next.state = replst::kWaitRWri;
                    break;
                case replst::kSnoop:
                    if (snp_rdy[i]) next.state = replst::kWaitRSnp;
                    break;
                case replst::kWaitRWri:
                    if (cmRespHit)
                        next.state = reg.alrReplSF ? replst::kCleanPosT : replst::kRespCmt;
                    break;
                case replst::kWaitRSnp:
                    if (cmRespHit) {
                        const bool cmRespData =
                            dc::tiValid(cm_resp.bits.taskInst) &&
                            dc::tiChannel(cm_resp.bits.taskInst) == kChDat;
                        next.state = cmRespData ? replst::kCopyId : replst::kCleanPosR;
                    }
                    break;
                case replst::kCopyId: next.state = replst::kReqPos; break;
                case replst::kSaveData:
                    if (data_task_rdy[i]) next.state = replst::kWaitResp;
                    break;
                case replst::kWaitResp:
                    if (dataRespHit)
                        next.state = reg.alrReplSF ? replst::kCleanPosT : replst::kRespCmt;
                    break;
                case replst::kCleanPosT:
                    if (clean_rdy[i]) next.state = replst::kCleanPosR;
                    break;
                case replst::kCleanPosR:
                    if (clean_rdy[i]) next.state = replst::kFree;
                    break;
                default: break;
            }

            // hnTxnID：COPYID 换槽 / posResp 选中新槽
            if (reg.state == replst::kCopyId) {
                next.hnTxnID = reg.replHnTxnID;
            } else if (posRespHit) {
                next.replHnTxnID = hnIdxOf(db, ps, pos_resp[db][ps].bits);
            }

            // llcRespHit 副作用：toLan / ds
            if (hits.llc[i]) {
                next.replToLan = ciOf(resp_dir_llc.bits.addr) == cfg_ci;
                DsIdx nds;
                nds.set(resp_dir_llc.bits.addr, ohToUInt(resp_dir_llc.bits.wayOH));
                next.ds = nds;
            }

            // sfRespHit 副作用
            if (hits.sf[i]) {
                next.needSnp = resp_dir_sf.bits.meta != 0;
                next.alrReplSF = true;
                next.dir.sf.wayOH = resp_dir_sf.bits.wayOH;
                next.dir.sf.meta = resp_dir_sf.bits.meta;
                next.dir.sf.hit = resp_dir_sf.bits.hit;
            } else if (next.state == replst::kFree) {
                next.needSnp = false;
                next.alrReplSF = false;
            }

            // cmRespData 副作用：改写为 wriLLC 任务
            const bool cmRespDataHit =
                cmRespHit && dc::tiValid(cm_resp.bits.taskInst) &&
                dc::tiChannel(cm_resp.bits.taskInst) == kChDat;
            if (cmRespDataHit) {
                const bool dirty = (dc::tiResp(cm_resp.bits.taskInst) >> 2) & 1;  // passDirty
                next.wriSF = false;
                next.wriLLC = true;
                next.dir.llc.hit = false;
                next.dir.llc.meta = dirty ? kStUD : kStSC;
            }

            // alloc 覆盖（原 reg update 的 alloc 分支）
            if (allocFire) {
                const auto& ab = alloc_out[i].bits;
                ReplReg a{};
                a.dir = ab.dir;
                a.hnTxnID = ab.hnTxnID;
                a.qos = ab.qos;
                a.wriSF = ab.wriSF;
                a.wriLLC = ab.wriLLC;
                a.directAllocSF = ab.directAllocSF;
                a.replHnTxnID = ab.hnTxnID;
                a.state = next.state;  // 状态机已按 alloc 计算
                n[i] = a;
            } else {
                n[i] = next;
            }
        }
        return n;
    };
}
}  // namespace zj::dj
