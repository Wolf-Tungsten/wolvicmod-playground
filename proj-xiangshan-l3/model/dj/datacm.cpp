#include <wolvicmod/wolvicmod.h>

#include "model/dj/data.h"

namespace zj::dj {

namespace {

// chisel PriorityEncoder 语义（0 输入 → 0）
constexpr uint8_t pe(uint64_t v) {
    if (v == 0) return 0;
    uint8_t i = 0;
    while (((v >> i) & 1u) == 0) ++i;
    return i;
}
constexpr uint8_t popcount2(uint8_t v) {
    return static_cast<uint8_t>((v & 1u) + ((v >> 1) & 1u));
}

}  // namespace

// ---------------- DataCtrlEntry ----------------

DataCtrlEntry::DataCtrlEntry() {
    // ---- 状态查询（组合） ----
    alloc_rdy.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        return isFree(reg);
    };
    read_for_repl.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        return isRepl(reg);
    };
    state.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        EntryState s;
        s.hnTxnID = reg.task.hnTxnID;
        s.dataVec = reg.dataVec;
        s.dbidVec = reg.dbidVec;
        return Valid<EntryState>{!isFree(reg), s};
    };
    tx_dat_bits.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        return reg.task.txDat;
    };

    // ---- 输出通道 ----
    read_to_db.assign().reads(reg, dcid) = [](auto src) {
        auto [reg, dcid] = src;
        const bool valid = isRepl(reg) || (reg.state == ctrl::kRead && reg.s_read != 0);
        ReadDS r;
        r.ds = reg.task.ds;
        r.dcid = dcid;
        const uint8_t beat = pe(reg.s_read);
        r.dbid = reg.dbidVec[beat];
        r.beatNum = beat;
        r.qos = reg.task.qos;
        r.critical = reg.critical;
        r.toCHI = (reg.task.dataOp.repl || reg.task.dataOp.send) && !reg.task.dataOp.merge;
        return Valid<ReadDS>{valid, r};
    };
    read_to_ds.assign().reads(reg, dcid) = [](auto src) {
        auto [reg, dcid] = src;
        const bool valid = isRepl(reg) || (reg.state == ctrl::kSave && reg.s_save != 0);
        ReadDB r;
        r.ds = reg.task.ds;
        r.dcid = dcid;
        const uint8_t beat = pe(reg.s_save);
        r.dbid = reg.dbidVec[beat];
        r.beatNum = beat;
        r.qos = reg.task.qos;
        r.critical = reg.critical;
        r.repl = isRepl(reg);
        return Valid<ReadDB>{valid, r};
    };
    read_to_chi.assign().reads(reg, dcid) = [](auto src) {
        auto [reg, dcid] = src;
        const bool valid = reg.state == ctrl::kSend && reg.s_send != 0;
        ReadDB r;
        r.dcid = dcid;
        const uint8_t beat = pe(reg.s_send);
        r.dbid = reg.dbidVec[beat];
        r.beatNum = beat;
        r.qos = reg.task.qos;
        r.critical = reg.critical;
        r.repl = false;
        return Valid<ReadDB>{valid, r};
    };
    release.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        DBIDVecC r;
        r.dataVec = reg.dataVec & reg.task.dataVec;
        r.dbidVec = reg.dbidVec;
        return Valid<DBIDVecC>{reg.state == ctrl::kClean, r};
    };
    resp.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        return Valid<uint8_t>{reg.state == ctrl::kResp, reg.task.hnTxnID};
    };

    // ---- 次态（单一大组合，与 RTL 的 when/elsewhen 链同构） ----
    w_next.assign().reads(reg, dcid, alloc, task, clean, upd_hn_txn_id, ds_wri_db, tx_dat_fire,
                          db_wri_ds, read_to_db_rdy, read_to_ds_rdy, read_to_chi_rdy,
                          release_rdy, resp_rdy) = [](auto src) {
        auto [reg, dcid, alloc, task, clean, upd_hn_txn_id, ds_wri_db, tx_dat_fire, db_wri_ds,
              read_to_db_rdy, read_to_ds_rdy, read_to_chi_rdy, release_rdy, resp_rdy] = src;
        EntryReg next = reg;
        const bool isValid = reg.state != ctrl::kFree;
        const bool allocFire = alloc.valid && isFree(reg);
        const bool taskHit = isValid && task.valid && task.bits.hnTxnID == reg.task.hnTxnID;
        const bool cleanHit = isValid && clean.valid && clean.bits.hnTxnID == reg.task.hnTxnID;
        const bool updHit =
            isValid && upd_hn_txn_id.valid && upd_hn_txn_id.bits.before == reg.task.hnTxnID;
        const bool dsWriDBHit = isValid && ds_wri_db.valid && ds_wri_db.bits.dcid == dcid;
        const bool txDatHit = isValid && tx_dat_fire.valid && tx_dat_fire.bits.dcid == dcid;
        const bool dbWriDSHit = isValid && db_wri_ds.valid && db_wri_ds.bits.dcid == dcid;
        // 通道 fire（valid 由 reg 派生，与输出 assign 一致）
        const bool dbValid = isRepl(reg) || (reg.state == ctrl::kRead && reg.s_read != 0);
        const bool dsValid = isRepl(reg) || (reg.state == ctrl::kSave && reg.s_save != 0);
        const bool chiValid = reg.state == ctrl::kSend && reg.s_send != 0;
        const bool dbFire = dbValid && read_to_db_rdy;
        const bool dsFire = dsValid && read_to_ds_rdy;
        const bool chiFire = chiValid && read_to_chi_rdy;
        const bool relFire = reg.state == ctrl::kClean && release_rdy;
        const bool respFire = reg.state == ctrl::kResp && resp_rdy;

        if (taskHit) next.task = task.bits;

        // dataVec
        if (allocFire) {
            next.dataVec = alloc.bits.dataVec;
        } else if (cleanHit) {
            next.task.dataVec = clean.bits.dataVec;
        } else if (relFire) {
            next.dataVec = reg.dataVec & ~(reg.dataVec & reg.task.dataVec);
        }

        // 三通道位图（setNextXXV）
        const uint8_t dbBeat = pe(reg.s_read);
        const uint8_t dsBeat = pe(reg.s_save);
        const uint8_t chiBeat = pe(reg.s_send);
        const uint8_t tdv = task.bits.dataVec;
        if (allocFire) {
            next.s_read = 0;
            next.w_read = 0;
        } else if (taskHit && task.bits.dataOp.readToDB()) {
            next.s_read = tdv;
            next.w_read = tdv;
        } else {
            next.s_read = dbFire ? (dsWriDBHit ? reg.s_read & ~((1u << dbBeat) |
                                                               (1u << ds_wri_db.bits.beatNum))
                                               : reg.s_read & ~(1u << dbBeat))
                                 : (dsWriDBHit ? reg.s_read & ~(1u << ds_wri_db.bits.beatNum)
                                               : reg.s_read);
            if (dsWriDBHit) next.w_read = reg.w_read & ~(1u << ds_wri_db.bits.beatNum);
        }
        if (allocFire) {
            next.s_send = 0;
            next.w_send = 0;
        } else if (taskHit && task.bits.dataOp.readToCHI()) {
            next.s_send = tdv;
            next.w_send = tdv;
        } else {
            next.s_send = chiFire ? (txDatHit ? reg.s_send & ~((1u << chiBeat) |
                                                               (1u << tx_dat_fire.bits.beatNum))
                                              : reg.s_send & ~(1u << chiBeat))
                                  : (txDatHit ? reg.s_send & ~(1u << tx_dat_fire.bits.beatNum)
                                              : reg.s_send);
            if (txDatHit) next.w_send = reg.w_send & ~(1u << tx_dat_fire.bits.beatNum);
        }
        if (allocFire) {
            next.s_save = 0;
            next.w_save = 0;
        } else if (taskHit && task.bits.dataOp.readToDS()) {
            next.s_save = tdv;
            next.w_save = tdv;
        } else {
            next.s_save = dsFire ? (dbWriDSHit ? reg.s_save & ~((1u << dsBeat) |
                                                                (1u << db_wri_ds.bits.beatNum))
                                               : reg.s_save & ~(1u << dsBeat))
                                 : (dbWriDSHit ? reg.s_save & ~(1u << db_wri_ds.bits.beatNum)
                                               : reg.s_save);
            if (dbWriDSHit) next.w_save = reg.w_save & ~(1u << db_wri_ds.bits.beatNum);
        }

        // critical
        if (allocFire || taskHit) {
            next.critical = false;
        } else if (dbFire) {
            next.critical = popcount2(reg.s_read) > 1;
        } else if (chiFire) {
            next.critical = popcount2(reg.s_send) > 1;
        } else if (dsFire) {
            next.critical = popcount2(reg.s_save) > 1;
        }

        // hnTxnID
        if (allocFire) {
            next.task.hnTxnID = alloc.bits.hnTxnID;
        } else if (updHit) {
            next.task.hnTxnID = upd_hn_txn_id.bits.next;
        }

        // dbidVec
        if (allocFire) next.dbidVec = alloc.bits.dbidVec;

        // 状态机（用 next.* 位图判断）
        const bool nReadAll = next.w_read == 0;
        const bool nSendAll = next.w_send == 0;
        const bool nSaveAll = next.w_save == 0;
        switch (reg.state) {
            case ctrl::kFree:
                if (allocFire) next.state = ctrl::kAlloc;
                break;
            case ctrl::kAlloc:
                if (taskHit) {
                    next.state = task.bits.dataOp.repl  ? ctrl::kRepl
                                 : task.bits.dataOp.read ? ctrl::kRead
                                 : task.bits.dataOp.send ? ctrl::kSend
                                                         : ctrl::kSave;
                } else if (cleanHit) {
                    next.state = ctrl::kClean;
                }
                break;
            case ctrl::kRepl:
                if (nReadAll && nSaveAll) next.state = nSendAll ? ctrl::kResp : ctrl::kSend;
                break;
            case ctrl::kRead:
                if (nReadAll)
                    next.state = (reg.task.dataOp.send && !nSendAll)  ? ctrl::kSend
                                 : (reg.task.dataOp.save && !nSaveAll) ? ctrl::kSave
                                                                       : ctrl::kResp;
                break;
            case ctrl::kSend:
                if (nSendAll)
                    next.state = (reg.task.dataOp.save && !nSaveAll) ? ctrl::kSave : ctrl::kResp;
                break;
            case ctrl::kSave:
                if (nSaveAll) next.state = ctrl::kResp;
                break;
            case ctrl::kResp:
                if (respFire) next.state = ctrl::kAlloc;
                break;
            case ctrl::kClean:
                if (relFire) next.state = next.dataVec == 0 ? ctrl::kFree : ctrl::kAlloc;
                break;
            default: break;
        }
        return next;
    };
    w_set.assign().reads(reg, alloc) = [](auto src) {
        auto [reg, alloc] = src;
        return (alloc.valid && isFree(reg)) || !isFree(reg);
    };
    reg.update().on(posedge(clk)).reads(reg, w_next, w_set) = [](auto src) {
        auto [reg, w_next, w_set] = src;
        return w_set ? w_next : reg;
    };
}

// ---------------- DataCM ----------------

DataCM::DataCM() {
    resp_arb.clk = clk;
    rel_arb.clk = clk;
    db_hi_arb.clk = clk;
    db_lo_arb.clk = clk;
    ds_hi_arb.clk = clk;
    ds_lo_arb.clk = clk;
    chi_hi_arb.clk = clk;
    chi_lo_arb.clk = clk;

    for (uint32_t i = 0; i < kNrDataCM; ++i) {
        entries[i].clk = clk;
        entries[i].dcid = static_cast<uint8_t>(i);
        entries[i].upd_hn_txn_id = upd_hn_txn_id;
        entries[i].clean = clean;
        entries[i].ds_wri_db = ds_wri_db;
        entries[i].tx_dat_fire = tx_dat_fire;
        entries[i].db_wri_ds = db_wri_ds;
        entries[i].task.assign().reads(task_fire_reg, task_reg) = [](auto src) {
            auto [task_fire_reg, task_reg] = src;
            return Valid<DataTask>{task_fire_reg, task_reg};
        };
    }
    task_fire_reg.update().on(posedge(clk)).reads(task) = [](auto src) {
        auto [task] = src;
        return task.valid;
    };
    task_reg.update().on(posedge(clk)).reads(task, task_reg) = [](auto src) {
        auto [task, task_reg] = src;
        return task.valid ? task.bits : task_reg;
    };

    // ---- 条目端口汇集（combine：实体 → 值数组） ----
    combine(w_alloc_rdy_all, entries,
            [](DataCtrlEntry& e) -> wolvicmod::Out<bool>& { return e.alloc_rdy; });
    combine(w_read_for_repl_all, entries,
            [](DataCtrlEntry& e) -> wolvicmod::Out<bool>& { return e.read_for_repl; });
    combine(w_resp_in, entries,
            [](DataCtrlEntry& e) -> wolvicmod::Out<Valid<uint8_t>>& { return e.resp; });
    combine(w_rel_in, entries,
            [](DataCtrlEntry& e) -> wolvicmod::Out<Valid<DBIDVecC>>& { return e.release; });
    combine(w_db_in, entries,
            [](DataCtrlEntry& e) -> wolvicmod::Out<Valid<ReadDS>>& { return e.read_to_db; });
    combine(w_ds_in, entries,
            [](DataCtrlEntry& e) -> wolvicmod::Out<Valid<ReadDB>>& { return e.read_to_ds; });
    combine(w_chi_in, entries,
            [](DataCtrlEntry& e) -> wolvicmod::Out<Valid<ReadDB>>& {
                return e.read_to_chi;
            });
    combine(w_tx_dat_bits, entries,
            [](DataCtrlEntry& e) -> wolvicmod::Out<DataFlit>& { return e.tx_dat_bits; });
    combine(w_states, entries,
            [](DataCtrlEntry& e) -> wolvicmod::Out<Valid<EntryState>>& { return e.state; });

    // ---- reqDB 分配 ----
    w_has_free_dc.assign().reads(w_alloc_rdy_all) = [](auto src) {
        auto [rdys] = src;
        for (bool r : rdys)
            if (r) return true;
        return false;
    };
    w_free_dcid.assign().reads(w_alloc_rdy_all) = [](auto src) -> uint8_t {
        auto [rdys] = src;
        for (uint32_t i = 0; i < kNrDataCM; ++i)
            if (rdys[i]) return static_cast<uint8_t>(i);
        return 0;
    };
    req_db_out.assign().reads(req_db_in, w_has_free_dc) = [](auto src) {
        auto [req_db_in, w_has_free_dc] = src;
        return Valid<uint8_t>{req_db_in.valid && w_has_free_dc, req_db_in.bits.dataVec};
    };
    req_db_in_rdy.assign().reads(req_db_out_rdy, w_has_free_dc) = [](auto src) {
        auto [req_db_out_rdy, w_has_free_dc] = src;
        return req_db_out_rdy && w_has_free_dc;
    };
    for (uint32_t i = 0; i < kNrDataCM; ++i) {
        entries[i].alloc.assign().reads(req_db_in, req_db_in_rdy, w_free_dcid, dbid_resp) =
            [i](auto src) {
                auto [req_db_in, req_db_in_rdy, w_free_dcid, dbid_resp] = src;
                AllocBits b;
                b.hnTxnID = req_db_in.bits.hnTxnID;
                b.dataVec = req_db_in.bits.dataVec;
                b.dbidVec = dbid_resp;
                const bool fire = req_db_in.valid && req_db_in_rdy;
                return Valid<AllocBits>{fire && w_free_dcid == i, b};
            };
        entries[i].resp_rdy.assign().reads(resp_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].release_rdy.assign().reads(rel_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
    }

    // ---- resp / release（VipArb RR，validOut） ----
    resp_arb.in = w_resp_in;
    resp_arb.out_rdy = true;
    resp = resp_arb.out;
    rel_arb.in = w_rel_in;
    rel_arb.out_rdy = true;
    release = rel_arb.out;

    // ---- getChiDat / getDBID ----
    get_chi_dat_bits.assign().reads(get_chi_dat_dcid, w_tx_dat_bits) = [](auto src) {
        auto [dcid, tx_bits] = src;
        return tx_bits[dcid];
    };
    get_dbid_dbid.assign().reads(get_dbid_txn_id, get_dbid_data_id, w_states) = [](auto src) {
        auto [txn_id, data_id, states] = src;
        uint8_t hitId = 0;
        for (uint32_t i = 0; i < kNrDataCM; ++i)
            if (states[i].valid && states[i].bits.hnTxnID == txn_id) {
                hitId = static_cast<uint8_t>(i);
                break;  // PriorityEncoder 取首个匹配
            }
        const uint8_t beat = data_id == 2 ? 1 : 0;
        return states[hitId].bits.dbidVec[beat];
    };

    // ---- repl 选择：critical 优先，其次 dcid 序 ----
    w_has_repl.assign().reads(w_read_for_repl_all) = [](auto src) {
        auto [rfs] = src;
        for (bool r : rfs)
            if (r) return true;
        return false;
    };
    w_repl_dcid.assign().reads(w_read_for_repl_all, w_ds_in) = [](auto src) -> uint8_t {
        auto [rfs, ds_in] = src;
        for (uint32_t i = 0; i < kNrDataCM; ++i)
            if (rfs[i] && ds_in[i].bits.critical) return static_cast<uint8_t>(i);
        for (uint32_t i = 0; i < kNrDataCM; ++i)
            if (rfs[i]) return static_cast<uint8_t>(i);
        return 0;
    };

    // ---- QoS 两层仲裁输入（高优层 qos==0xf） ----
    db_hi_arb.in.assign().reads(w_db_in) = [](auto src) {
        auto [w_db_in] = src;
        DbInArr a;
        for (uint32_t i = 0; i < kNrDataCM; ++i) {
            a[i].valid = w_db_in[i].valid && w_db_in[i].bits.qos == 0xF;
            a[i].bits = w_db_in[i].bits;
        }
        return a;
    };
    db_lo_arb.in = w_db_in;
    ds_hi_arb.in.assign().reads(w_ds_in) = [](auto src) {
        auto [w_ds_in] = src;
        DsInArr a;
        for (uint32_t i = 0; i < kNrDataCM; ++i) {
            a[i].valid = w_ds_in[i].valid && w_ds_in[i].bits.qos == 0xF;
            a[i].bits = w_ds_in[i].bits;
        }
        return a;
    };
    ds_lo_arb.in = w_ds_in;
    chi_hi_arb.in.assign().reads(w_chi_in) = [](auto src) {
        auto [w_chi_in] = src;
        DsInArr a;
        for (uint32_t i = 0; i < kNrDataCM; ++i) {
            a[i].valid = w_chi_in[i].valid && w_chi_in[i].bits.qos == 0xF;
            a[i].bits = w_chi_in[i].bits;
        }
        return a;
    };
    chi_lo_arb.in = w_chi_in;

    // ---- 读通道输出：repl 捆绑 > critical > QoS 高优 > 普通 ----
    read_to_db.assign().reads(w_has_repl, w_repl_dcid, w_db_in, read_to_ds_rdy, db_hi_arb.out,
                              db_lo_arb.out) = [](auto src) {
        auto [w_has_repl, w_repl_dcid, w_db_in, read_to_ds_rdy, hi_out, lo_out] = src;
        if (w_has_repl) return Valid<ReadDS>{read_to_ds_rdy, w_db_in[w_repl_dcid].bits};
        for (uint32_t i = 0; i < kNrDataCM; ++i)
            if (w_db_in[i].valid && w_db_in[i].bits.critical)
                return Valid<ReadDS>{true, w_db_in[i].bits};
        return hi_out.valid ? hi_out : lo_out;
    };
    read_to_ds.assign().reads(w_has_repl, w_repl_dcid, w_ds_in, read_to_db_rdy, ds_hi_arb.out,
                              ds_lo_arb.out) = [](auto src) {
        auto [w_has_repl, w_repl_dcid, w_ds_in, read_to_db_rdy, hi_out, lo_out] = src;
        if (w_has_repl) return Valid<ReadDB>{read_to_db_rdy, w_ds_in[w_repl_dcid].bits};
        for (uint32_t i = 0; i < kNrDataCM; ++i)
            if (w_ds_in[i].valid && w_ds_in[i].bits.critical)
                return Valid<ReadDB>{true, w_ds_in[i].bits};
        return hi_out.valid ? hi_out : lo_out;
    };
    read_to_chi.assign().reads(w_chi_in, chi_hi_arb.out, chi_lo_arb.out) = [](auto src) {
        auto [w_chi_in, hi_out, lo_out] = src;
        for (uint32_t i = 0; i < kNrDataCM; ++i)
            if (w_chi_in[i].valid && w_chi_in[i].bits.critical)
                return Valid<ReadDB>{true, w_chi_in[i].bits};
        return hi_out.valid ? hi_out : lo_out;
    };

    // 仲裁 out_rdy：高优层有候选时低优层停摆（fastQosRRArb 的 when(hasHigh)）
    db_hi_arb.out_rdy.assign().reads(db_hi_arb.out, read_to_db_rdy) = [](auto src) {
        auto [hi_out, read_to_db_rdy] = src;
        return hi_out.valid && read_to_db_rdy;
    };
    db_lo_arb.out_rdy.assign().reads(db_hi_arb.out, read_to_db_rdy) = [](auto src) {
        auto [hi_out, read_to_db_rdy] = src;
        return !hi_out.valid && read_to_db_rdy;
    };
    ds_hi_arb.out_rdy.assign().reads(ds_hi_arb.out, read_to_ds_rdy) = [](auto src) {
        auto [hi_out, read_to_ds_rdy] = src;
        return hi_out.valid && read_to_ds_rdy;
    };
    ds_lo_arb.out_rdy.assign().reads(ds_hi_arb.out, read_to_ds_rdy) = [](auto src) {
        auto [hi_out, read_to_ds_rdy] = src;
        return !hi_out.valid && read_to_ds_rdy;
    };
    chi_hi_arb.out_rdy.assign().reads(chi_hi_arb.out, read_to_chi_rdy) = [](auto src) {
        auto [hi_out, read_to_chi_rdy] = src;
        return hi_out.valid && read_to_chi_rdy;
    };
    chi_lo_arb.out_rdy.assign().reads(chi_hi_arb.out, read_to_chi_rdy) = [](auto src) {
        auto [hi_out, read_to_chi_rdy] = src;
        return !hi_out.valid && read_to_chi_rdy;
    };

    // 条目 read rdy 回接
    for (uint32_t i = 0; i < kNrDataCM; ++i) {
        entries[i].read_to_db_rdy.assign().reads(
            w_has_repl, w_repl_dcid, w_db_in, read_to_db_rdy, read_to_ds_rdy, db_hi_arb.in_rdy,
            db_lo_arb.in_rdy) =
            [i](auto src) {
                auto [w_has_repl, w_repl_dcid, w_db_in, read_to_db_rdy, read_to_ds_rdy, hi_rdy,
                      lo_rdy] = src;
                if (w_has_repl) return i == w_repl_dcid && read_to_ds_rdy && read_to_db_rdy;
                for (uint32_t j = 0; j < kNrDataCM; ++j)
                    if (w_db_in[j].valid && w_db_in[j].bits.critical)
                        return j == i && read_to_db_rdy;
                return hi_rdy[i] || lo_rdy[i];
            };
        entries[i].read_to_ds_rdy.assign().reads(
            w_has_repl, w_repl_dcid, w_ds_in, read_to_db_rdy, read_to_ds_rdy, ds_hi_arb.in_rdy,
            ds_lo_arb.in_rdy) =
            [i](auto src) {
                auto [w_has_repl, w_repl_dcid, w_ds_in, read_to_db_rdy, read_to_ds_rdy, hi_rdy,
                      lo_rdy] = src;
                if (w_has_repl) return i == w_repl_dcid && read_to_ds_rdy && read_to_db_rdy;
                for (uint32_t j = 0; j < kNrDataCM; ++j)
                    if (w_ds_in[j].valid && w_ds_in[j].bits.critical)
                        return j == i && read_to_ds_rdy;
                return hi_rdy[i] || lo_rdy[i];
            };
        entries[i].read_to_chi_rdy.assign().reads(w_chi_in, read_to_chi_rdy, chi_hi_arb.in_rdy,
                                                  chi_lo_arb.in_rdy) = [i](auto src) {
            auto [w_chi_in, read_to_chi_rdy, hi_rdy, lo_rdy] = src;
            for (uint32_t j = 0; j < kNrDataCM; ++j)
                if (w_chi_in[j].valid && w_chi_in[j].bits.critical)
                    return j == i && read_to_chi_rdy;
            return hi_rdy[i] || lo_rdy[i];
        };
    }
}

// ---------------- DataBlock ----------------

DataBlock::DataBlock() {
    data_cm.clk = clk;
    dbid_ctrl.clk = clk;
    dat_buf.clk = clk;
    dbid_ctrl.clk_en = clk_en;
    dat_buf.clk_en = clk_en;
    ds_top_arb.clk = clk;
    for (uint32_t i = 0; i < kNrDSBank; ++i) {
        ds_bank_arbs[i].clk = clk;
        ds_resp_pipes[i].clk = clk;
    }
    for (uint32_t i = 0; i < kNrDSBank * kNrBeat; ++i) {
        beat_storages[i].clk = clk;
        beat_storages[i].clk_en = clk_en;
    }

    // ---- DS 读交叉分发 ----
    for (uint32_t i = 0; i < kNrDSBank * kNrBeat; ++i) {
        beat_storages[i].read.assign().reads(data_cm.read_to_db) = [i](auto src) {
            auto [read_to_db] = src;
            const bool sel = read_to_db.bits.ds.bank * kNrBeat + read_to_db.bits.beatNum == i;
            return Valid<ReadDS>{read_to_db.valid && sel, read_to_db.bits};
        };
        beat_storages[i].write.assign().reads(dat_buf.write_ds) = [i](auto src) {
            auto [write_ds] = src;
            const bool sel = write_ds.bits.ds.bank * kNrBeat + write_ds.bits.beatNum == i;
            return Valid<WriteDS>{write_ds.valid && sel, write_ds.bits};
        };
    }
    combine(w_bs_read_rdy, beat_storages,
            [](BeatStorage& b) -> wolvicmod::Out<bool>& { return b.read_rdy; });
    combine(w_bs_write_rdy, beat_storages,
            [](BeatStorage& b) -> wolvicmod::Out<bool>& { return b.write_rdy; });
    data_cm.read_to_db_rdy.assign().reads(data_cm.read_to_db, w_bs_read_rdy) = [](auto src) {
        auto [read_to_db, rdys] = src;
        return rdys[read_to_db.bits.ds.bank * kNrBeat + read_to_db.bits.beatNum];
    };
    dat_buf.write_ds_rdy.assign().reads(dat_buf.write_ds, w_bs_write_rdy) = [](auto src) {
        auto [write_ds, rdys] = src;
        return rdys[write_ds.bits.ds.bank * kNrBeat + write_ds.bits.beatNum];
    };

    // ---- dsResp 两级 fastArb ----
    for (uint32_t b = 0; b < kNrDSBank; ++b) {
        ds_bank_arbs[b].in.assign().reads(beat_storages[b * kNrBeat].resp,
                                          beat_storages[b * kNrBeat + 1].resp) = [](auto src) {
            auto [r0, r1] = src;
            std::array<Valid<DsResp>, kNrBeat> a;
            a[0] = r0;
            a[1] = r1;
            return a;
        };
        ds_bank_arbs[b].out_rdy = true;
        ds_resp_pipes[b].enq = ds_bank_arbs[b].out;
    }
    combine(w_ds_pipe_deq, ds_resp_pipes,
            [](DsRespPipeT& p) -> wolvicmod::Out<Valid<DsResp>>& { return p.deq; });
    ds_top_arb.in = w_ds_pipe_deq;
    ds_top_arb.out_rdy = true;
    w_ds_resp_arbed = ds_top_arb.out;
    dat_buf.ds_resp = w_ds_resp_arbed;

    // ---- dataCM ↔ datBuf / dbidCtrl ----
    dat_buf.read_to_chi = data_cm.read_to_chi;
    data_cm.read_to_chi_rdy = dat_buf.read_to_chi_rdy;
    dat_buf.read_to_ds = data_cm.read_to_ds;
    data_cm.read_to_ds_rdy = dat_buf.read_to_ds_rdy;

    data_cm.upd_hn_txn_id = upd_hn_txn_id;
    data_cm.task = task;
    data_cm.clean = clean_db;
    resp = data_cm.resp;

    data_cm.req_db_in = req_db;
    req_db_rdy = data_cm.req_db_in_rdy;
    dbid_ctrl.req = data_cm.req_db_out;
    data_cm.req_db_out_rdy = dbid_ctrl.req_rdy;
    data_cm.dbid_resp = dbid_ctrl.resp;
    dbid_ctrl.release = data_cm.release;

    // datBuf.clean = dbidCtrl.req.fire + resp
    dat_buf.clean.assign().reads(dbid_ctrl.req, dbid_ctrl.req_rdy, dbid_ctrl.resp) =
        [](auto src) {
            auto [req, req_rdy, resp] = src;
            DBIDVecC c;
            c.dataVec = req.bits;
            c.dbidVec = resp;
            return Valid<DBIDVecC>{req.valid && req_rdy, c};
        };

    // dsWriDB / dbWriDS / txDatFire 完成通知
    data_cm.ds_wri_db.assign().reads(w_ds_resp_arbed) = [](auto src) {
        auto [ds_resp] = src;
        return Valid<DcidBeat>{ds_resp.valid, {ds_resp.bits.dcid, ds_resp.bits.beatNum}};
    };
    data_cm.db_wri_ds.assign().reads(dat_buf.write_ds, dat_buf.write_ds_rdy) = [](auto src) {
        auto [write_ds, write_ds_rdy] = src;
        return Valid<DcidBeat>{write_ds.valid && write_ds_rdy,
                               {write_ds.bits.dcid, write_ds.bits.beatNum}};
    };

    // ---- rxDat（fromCHI） ----
    w_db_to_chi.assign().reads(dat_buf.to_chi) = [](auto src) {
        auto [to_chi] = src;
        return to_chi.valid;
    };
    w_ds_to_chi.assign().reads(w_ds_resp_arbed) = [](auto src) {
        auto [ds_resp] = src;
        return ds_resp.valid && ds_resp.bits.toCHI;
    };
    rx_dat_rdy = dat_buf.from_chi_rdy;
    data_cm.get_dbid_valid.assign().reads(rx_dat) = [](auto src) {
        auto [rx_dat] = src;
        return rx_dat.valid;
    };
    data_cm.get_dbid_txn_id.assign().reads(rx_dat) = [](auto src) {
        auto [rx_dat] = src;
        return rx_dat.bits.txn_id;
    };
    data_cm.get_dbid_data_id.assign().reads(rx_dat) = [](auto src) {
        auto [rx_dat] = src;
        return rx_dat.bits.data_id;
    };
    dat_buf.from_chi.assign().reads(rx_dat, data_cm.get_dbid_dbid) = [](auto src) {
        auto [rx_dat, dbid] = src;
        FromCHI f;
        f.dat = rx_dat.bits;
        f.dbid = dbid;
        return Valid<FromCHI>{rx_dat.valid, f};
    };

    // ---- txDat（dbToCHI 优先，dsToCHI 次之） ----
    data_cm.get_chi_dat_valid.assign().reads(w_db_to_chi, w_ds_to_chi) = [](auto src) {
        auto [db_to_chi, ds_to_chi] = src;
        return db_to_chi || ds_to_chi;
    };
    data_cm.get_chi_dat_dcid.assign().reads(w_db_to_chi, dat_buf.to_chi, w_ds_resp_arbed) =
        [](auto src) {
            auto [db_to_chi, to_chi, ds_resp] = src;
            return db_to_chi ? to_chi.bits.dcid : ds_resp.bits.dcid;
        };
    tx_dat.assign().reads(w_db_to_chi, w_ds_to_chi, dat_buf.to_chi, w_ds_resp_arbed,
                          data_cm.get_chi_dat_bits) = [](auto src) {
        auto [db_to_chi, ds_to_chi, to_chi, ds_resp, chi_bits] = src;
        DataFlit f = chi_bits;
        if (db_to_chi) {
            f.data_id = to_chi.bits.dat.data_id;
            f.data = to_chi.bits.dat.data;
            f.be = to_chi.bits.dat.be;
        } else {
            f.data_id = static_cast<uint8_t>(ds_resp.bits.beatNum << 1);
            f.data = ds_resp.bits.beat;
            f.be = 0xFFFFFFFFull;
        }
        return Valid<DataFlit>{db_to_chi || ds_to_chi, f};
    };
    dat_buf.to_chi_rdy = tx_dat_rdy;
    data_cm.tx_dat_fire.assign().reads(tx_dat, tx_dat_rdy, w_db_to_chi, dat_buf.to_chi,
                                       w_ds_resp_arbed) = [](auto src) {
        auto [tx_dat, tx_dat_rdy, db_to_chi, to_chi, ds_resp] = src;
        const bool fire = tx_dat.valid && tx_dat_rdy;
        DcidBeat b;
        b.dcid = db_to_chi ? to_chi.bits.dcid : ds_resp.bits.dcid;
        b.beatNum = db_to_chi ? to_chi.bits.beatNum : ds_resp.bits.beatNum;
        return Valid<DcidBeat>{fire, b};
    };
}

}  // namespace zj::dj
