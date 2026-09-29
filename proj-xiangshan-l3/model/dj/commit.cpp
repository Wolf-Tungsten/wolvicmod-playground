#include <wolvicmod/wolvicmod.h>

#include "model/dj/commit.h"

namespace zj::dj {

namespace {
namespace dc = dectab;

// DataVec
constexpr uint8_t kFullVec = 0x3;
// ChiChannel
constexpr uint8_t kChReq = 0, kChDat = 1, kChRsp = 2, kChSnp = 3;
// RspOpcode
constexpr uint8_t kSnpResp = 0x1, kCompAck = 0x2, kComp = 0x4, kCompDBIDResp = 0x5,
                  kDBIDResp = 0x6, kSnpRespFwded = 0x9;
// DatOpcode
constexpr uint8_t kSnpRespData = 0x1, kCopyBackWriteData = 0x2, kNonCopyBackWriteData = 0x3,
                  kCompData = 0x4, kSnpRespDataFwded = 0x6, kNCBWrDataCompAck = 0xc;
// RespErr
constexpr uint8_t kErrOk = 0, kErrExOk = 1, kErrDerr = 2, kErrNderr = 3;
}  // namespace

// ---------------- BackendDecode ----------------

template <bool Third>
BackendDecode<Third>::BackendDecode() {
    dec_val_reg.update().on(posedge(clk)).reads(dec_mes_in) = [](auto src) {
        auto [dec_mes_in] = src;
        return dec_mes_in.valid;
    };
    dec_mes_reg.update().on(posedge(clk)).reads(dec_mes_in, dec_mes_reg) = [](auto src) {
        auto [dec_mes_in, dec_mes_reg] = src;
        return dec_mes_in.valid ? dec_mes_in.bits : dec_mes_reg;
    };

    // taskInst.valid := decValReg & 原 valid（其余字段透传）
    w_task_inst.assign().reads(dec_val_reg, dec_mes_reg) = [](auto src) {
        auto [dec_val_reg, dec_mes_reg] = src;
        uint32_t ti = dec_mes_reg.taskInst;
        const bool v = dec_val_reg && dc::tiValid(ti);
        ti = (ti & ~(1u << 18)) | (static_cast<uint32_t>(v) << 18);
        return ti;
    };
    // thirdDec：查 decList(2)；fourthDec：查 decList(3)
    w_dec_list.assign().reads(w_task_inst, dec_mes_reg) = [](auto src) {
        auto [w_task_inst, dec_mes_reg] = src;
        auto list = dec_mes_reg.decList;
        if constexpr (Third) {
            list[2] = static_cast<uint8_t>(dc::decTask(list[0], list[1], w_task_inst));
        } else {
            list[3] =
                static_cast<uint8_t>(dc::decSec(list[0], list[1], list[2], w_task_inst));
        }
        return list;
    };
    w_task_code.assign().reads(w_dec_list) = [](auto src) {
        auto [w_dec_list] = src;
        if constexpr (Third) {
            return dc::getSecTaskCode(w_dec_list[0], w_dec_list[1], w_dec_list[2]);
        } else {
            return 0u;
        }
    };
    w_cmt_code.assign().reads(w_dec_list) = [](auto src) {
        auto [w_dec_list] = src;
        return dc::getCommitCode(w_dec_list[0], w_dec_list[1], w_dec_list[2], w_dec_list[3]);
    };

    hn_id_val_reg.update().on(posedge(clk)).reads(dec_val_reg) = [](auto src) {
        auto [dec_val_reg] = src;
        return dec_val_reg;
    };
    hn_id_reg.update().on(posedge(clk)).reads(dec_val_reg, dec_mes_reg, hn_id_reg) =
        [](auto src) {
            auto [dec_val_reg, dec_mes_reg, hn_id_reg] = src;
            return dec_val_reg ? dec_mes_reg.hnTxnID : hn_id_reg;
        };
    // RTL backend/Decode.scala：io.hnTxnIdOut.valid := RegNext(decValReg)、
    // bits := RegEnable(decMesReg.hnTxnID, decValReg)——与 code 输出同拍（第二级）。
    hn_txn_id_out.assign().reads(hn_id_val_reg, hn_id_reg) = [](auto src) {
        auto [v, id] = src;
        return Valid<uint8_t>{v, id};
    };
    dec_list_out = dec_list_reg;
    task_code_out = task_code_reg;
    cmt_code_out = cmt_code_reg;
    dec_list_reg.update().on(posedge(clk)).reads(dec_val_reg, w_dec_list, dec_list_reg) =
        [](auto src) {
            auto [dec_val_reg, w_dec_list, dec_list_reg] = src;
            return dec_val_reg ? w_dec_list : dec_list_reg;
        };
    task_code_reg.update().on(posedge(clk)).reads(dec_val_reg, w_task_code, task_code_reg) =
        [](auto src) {
            auto [dec_val_reg, w_task_code, task_code_reg] = src;
            return dec_val_reg ? w_task_code : task_code_reg;
        };
    cmt_code_reg.update().on(posedge(clk)).reads(dec_val_reg, w_cmt_code, cmt_code_reg) =
        [](auto src) {
            auto [dec_val_reg, w_cmt_code, cmt_code_reg] = src;
            return dec_val_reg ? w_cmt_code : cmt_code_reg;
        };
}

// ---------------- Commit ----------------
// 拍平建模：112 个表项不再做子模块——状态并入 REG(entries) 一条 update 循环，
// per-entry 组合逻辑数组化为 Array wire（广播输入变化今天本来就唤醒全部
// 112 个实例的小 assign，数组合并后总计算量不变、框架动作数 -95%）。
// 仲裁器/译码器子模块不变。

Commit::Commit() {
    trd_dec.clk = clk;
    fth_dec.clk = clk;
    trd_arb.clk = clk;
    fth_arb.clk = clk;
    tx_rsp_arb.clk = clk;
    snp_arb.clk = clk;
    wri_arb.clk = clk;
    read_arb.clk = clk;
    req_db_arb.clk = clk;
    repl_arb.clk = clk;
    data_task_arb.clk = clk;
    clean_arb.clk = clk;

    // ---- 基本派生 ----
    w_valid.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        BoolArrN o{};
        for (uint32_t i = 0; i < kEntries; ++i) o[i] = entries[i].state != kFree;
        return o;
    };
    w_alloc_hit.assign().reads(cmt_task_0, cmt_task_1) = [](auto src) {
        auto [cmt_task_0, cmt_task_1] = src;
        BoolArrN o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const auto& al = (i / 56 == 0) ? cmt_task_0 : cmt_task_1;
            o[i] = al.valid && al.bits.hnTxnID == hnIdOf(i);
        }
        return o;
    };
    w_comp_ack_hit.assign().reads(rx_rsp, rx_dat) = [](auto src) {
        auto [rx_rsp, rx_dat] = src;
        BoolArrN o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const uint8_t hn = hnIdOf(i);
            const bool rspHit = rx_rsp.valid && rx_rsp.bits.txn_id == hn;
            const bool datHit = rx_dat.valid && rx_dat.bits.txn_id == hn;
            o[i] = (rspHit && rx_rsp.bits.opcode == kCompAck) ||
                   (datHit && rx_dat.bits.opcode == kNCBWrDataCompAck);
        }
        return o;
    };
    w_xcb_hit0.assign().reads(rx_dat) = [](auto src) {
        auto [rx_dat] = src;
        BoolArrN o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const uint8_t op = rx_dat.bits.opcode;
            o[i] = rx_dat.valid && rx_dat.bits.txn_id == hnIdOf(i) &&
                   rx_dat.bits.data_id == 0 &&
                   (op == kNonCopyBackWriteData || op == kNCBWrDataCompAck ||
                    op == kCopyBackWriteData);
        }
        return o;
    };
    w_xcb_hit1.assign().reads(rx_dat) = [](auto src) {
        auto [rx_dat] = src;
        BoolArrN o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const uint8_t op = rx_dat.bits.opcode;
            o[i] = rx_dat.valid && rx_dat.bits.txn_id == hnIdOf(i) &&
                   rx_dat.bits.data_id == 2 &&
                   (op == kNonCopyBackWriteData || op == kNCBWrDataCompAck ||
                    op == kCopyBackWriteData);
        }
        return o;
    };
    w_cm_resp_hit.assign().reads(w_valid, cm_resp) = [](auto src) {
        auto [w_valid, cm_resp] = src;
        BoolArrN o{};
        for (uint32_t i = 0; i < kEntries; ++i)
            o[i] = w_valid[i] && cm_resp.valid && cm_resp.bits.hnTxnID == hnIdOf(i);
        return o;
    };

    // ---- 译码结果回灌（按 hnTxnID 匹配；trd 优先） ----
    w_dec_list_in.assign().reads(trd_dec.hn_txn_id_out, fth_dec.hn_txn_id_out,
                                 trd_dec.dec_list_out, fth_dec.dec_list_out) = [](auto src) {
        auto [trd_id, fth_id, trd_list, fth_list] = src;
        DecListInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const bool trdHit = trd_id.valid && trd_id.bits == hnIdOf(i);
            const bool fthHit = fth_id.valid && fth_id.bits == hnIdOf(i);
            o[i] = Valid<DecListArr>{trdHit || fthHit, trdHit ? trd_list : fth_list};
        }
        return o;
    };
    w_task_code_in.assign().reads(trd_dec.task_code_out, fth_dec.task_code_out,
                                  trd_dec.hn_txn_id_out, fth_dec.hn_txn_id_out) = [](auto src) {
        auto [trd_code, fth_code, trd_id, fth_id] = src;
        U32ArrN o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const bool trdHit = trd_id.valid && trd_id.bits == hnIdOf(i);
            o[i] = trdHit ? trd_code : fth_code;
        }
        return o;
    };
    w_cmt_code_in.assign().reads(trd_dec.cmt_code_out, fth_dec.cmt_code_out,
                                 trd_dec.hn_txn_id_out, fth_dec.hn_txn_id_out) = [](auto src) {
        auto [trd_code, fth_code, trd_id, fth_id] = src;
        U32ArrN o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const bool trdHit = trd_id.valid && trd_id.bits == hnIdOf(i);
            o[i] = trdHit ? trd_code : fth_code;
        }
        return o;
    };

    // ---- 译码请求 ----
    w_trd_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        DecInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const V& e = entries[i];
            const bool decValid =
                e.flag.sDecode && !(e.flag.wCmResp || e.flag.wXCB0 || e.flag.wXCB1);
            DecMes m;
            m.taskInst = e.inst;
            m.decList = e.task.decList;
            m.hnTxnID = hnIdOf(i);
            o[i] = Valid<DecMes>{decValid && e.state == kFstTask, m};
        }
        return o;
    };
    w_fth_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        DecInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const V& e = entries[i];
            const bool decValid =
                e.flag.sDecode && !(e.flag.wCmResp || e.flag.wXCB0 || e.flag.wXCB1);
            DecMes m;
            m.taskInst = e.inst;
            m.decList = e.task.decList;
            m.hnTxnID = hnIdOf(i);
            o[i] = Valid<DecMes>{decValid && e.state == kSecTask, m};
        }
        return o;
    };

    // ---- 输出通道数组 ----
    w_req_db_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        ReqDbInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const V& e = entries[i];
            ReqDBQos r;
            r.hnTxnID = hnIdOf(i);
            r.dataVec = (e.flag.sCmTask && dc::tcSnoop(e.task.task)) ? kFullVec
                                                                     : e.task.chi.dataVec;
            r.qos = e.task.qos;
            o[i] = Valid<ReqDBQos>{e.state != kFree && e.flag.sReqDB, r};
        }
        return o;
    };
    w_data_task_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        DataTaskInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const V& e = entries[i];
            const uint8_t hn = hnIdOf(i);
            DataTask t;
            const bool replLLC = e.flag.sWriDir && dc::ccWriLLC(e.task.cmt) && !e.task.dir.llc.hit;
            t.dataOp.repl = dc::ccOpRepl(e.task.cmt);
            t.dataOp.read = dc::ccOpRead(e.task.cmt);
            t.dataOp.send = dc::ccOpSend(e.task.cmt);
            t.dataOp.save = dc::ccOpSave(e.task.cmt) && !replLLC;
            t.dataOp.merge = dc::ccOpMerge(e.task.cmt);
            t.hnTxnID = hn;
            t.ds = e.task.ds;
            t.dataVec = dc::ccFullSize(e.task.cmt) ? kFullVec : e.task.chi.dataVec;
            t.qos = e.task.qos;
            t.txDat.dbid = hn;
            t.txDat.resp = static_cast<uint8_t>(dc::ccResp(e.task.cmt));
            t.txDat.opcode = static_cast<uint8_t>(dc::ccOpcode(e.task.cmt));
            t.txDat.txn_id = e.task.chi.txnID;
            t.txDat.src_id = e.task.chi.getNoC();
            t.txDat.tgt_id = e.task.chi.nodeId;
            t.txDat.resp_err = e.respErr;
            o[i] = Valid<DataTask>{e.state != kFree && e.flag.sDataTask && e.task.alr.reqDB, t};
        }
        return o;
    };
    w_repl_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        ReplInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const V& e = entries[i];
            ReplTask t;
            t.hnTxnID = hnIdOf(i);
            t.qos = e.task.qos;
            t.wriSF = dc::ccWriSRC(e.task.cmt) || dc::ccWriSNP(e.task.cmt);
            t.dir.sf.hit = e.task.dir.sf.hit;
            t.dir.sf.wayOH = e.task.dir.sf.wayOH;
            t.directAllocSF = t.wriSF && !e.task.dir.sf.hit && e.task.dir.sf.meta == 0;
            // sf metaVec 单 meta：srcVec/snpVec 只可能命中 meta0
            const bool srcVec0 = true;  // metaIdOH 恒 1（nrSfMetas=1）
            const uint32_t snpTgt = dc::tcSnpTgt(e.task.task);
            const bool snpVec0 =
                snpTgt == 1 && e.task.dir.sf.meta != 0;  // SnpTgt.ALL=b01；单 meta 时 ONE/OTH→0
            uint8_t meta0;
            if (srcVec0 && dc::ccWriSRC(e.task.cmt)) {
                meta0 = dc::ccSrcValid(e.task.cmt);
            } else if (snpVec0 && dc::ccWriSNP(e.task.cmt)) {
                meta0 = dc::ccSnpValid(e.task.cmt);
            } else {
                meta0 = e.task.dir.sf.hit ? e.task.dir.sf.meta : 0;
            }
            t.dir.sf.meta = meta0;
            t.wriLLC = dc::ccWriLLC(e.task.cmt);
            t.dir.llc.hit = e.task.dir.llc.hit;
            t.dir.llc.wayOH = e.task.dir.llc.wayOH;
            t.dir.llc.meta = static_cast<uint8_t>(dc::ccLlcState(e.task.cmt));
            o[i] = Valid<ReplTask>{e.state != kFree && e.flag.sWriDir && !e.flag.wDataResp, t};
        }
        return o;
    };
    // cmTask 三路（bits 共享，valid 分流）
    w_snp_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        CmTaskInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const V& e = entries[i];
            CMTask t;
            t.chi = e.task.chi;
            t.chi.channel = dc::tcSnoop(e.task.task) ? kChSnp : kChReq;
            t.chi.dataVec = (dc::tcSnoop(e.task.task) || dc::tcFullSize(e.task.task))
                                ? kFullVec
                                : e.task.chi.dataVec;
            t.chi.opcode = static_cast<uint8_t>(dc::tcOpcode(e.task.task));
            t.chi.expCompAck = dc::tcExpCompAck(e.task.task);
            t.chi.retToSrc = dc::tcRetToSrc(e.task.task);
            t.chi.size = (dc::tcSnoop(e.task.task) || dc::tcFullSize(e.task.task))
                             ? 6
                             : e.task.chi.size;
            t.hnTxnID = hnIdOf(i);
            t.dataOp.repl = dc::tcOpRepl(e.task.task);
            t.dataOp.read = dc::tcOpRead(e.task.task);
            t.dataOp.send = dc::tcOpSend(e.task.task);
            t.dataOp.save = dc::tcOpSave(e.task.task);
            t.dataOp.merge = dc::tcOpMerge(e.task.task);
            t.ds = e.task.ds;
            const uint32_t snpTgt = dc::tcSnpTgt(e.task.task);
            t.snpVec = snpTgt == 1 && e.task.dir.sf.meta != 0 ? 1 : 0;
            t.fromRepl = false;
            // cbResp：llc meta 的 cbResp（I→I, SC→SC, UC→UC, UD→UD_PD）
            const uint8_t st = e.task.dir.llc.meta;
            t.cbResp = st == 0 ? 0 : st == 1 ? 1 : st == 3 ? 2 : 6;  // I/SC/UC/UD→I/SC/UC/UD_PD
            t.doDMT = dc::tcDoDMT(e.task.task);
            t.qos = e.task.qos;
            const bool v = e.state != kFree && e.flag.sCmTask && !e.flag.sReqDB &&
                           dc::tcSnoop(e.task.task);
            o[i] = Valid<CMTask>{v, t};
        }
        return o;
    };
    w_read_in.assign().reads(w_snp_in, entries) = [](auto src) {
        auto [snp_ch, entries] = src;
        CmTaskInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const V& e = entries[i];
            o[i] = Valid<CMTask>{e.state != kFree && e.flag.sCmTask && !e.flag.sReqDB &&
                                     dc::tcRead(e.task.task),
                                 snp_ch[i].bits};
        }
        return o;
    };
    w_wri_in.assign().reads(w_snp_in, entries) = [](auto src) {
        auto [snp_ch, entries] = src;
        CmTaskInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const V& e = entries[i];
            o[i] = Valid<CMTask>{e.state != kFree && e.flag.sCmTask && !e.flag.sReqDB &&
                                     dc::tcWrite(e.task.task),
                                 snp_ch[i].bits};
        }
        return o;
    };
    w_tx_rsp_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        TxRspInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const V& e = entries[i];
            RespFlit f{};
            f.src_id = e.task.chi.getNoC();
            f.tgt_id = e.task.chi.nodeId;
            f.txn_id = e.task.chi.txnID;
            f.dbid = hnIdOf(i);
            f.opcode = e.flag.sResp ? static_cast<uint8_t>(dc::ccOpcode(e.task.cmt))
                                    : (e.task.chi.isCopyBackWrite() ? kCompDBIDResp : kDBIDResp);
            f.fwd_state = static_cast<uint8_t>(dc::ccFwdResp(e.task.cmt));
            f.resp = static_cast<uint8_t>(dc::ccResp(e.task.cmt));
            f.qos = e.task.qos;
            f.resp_err = e.respErr;
            const bool v =
                e.state != kFree && (e.flag.sDbid || e.flag.sResp) && !e.flag.sReqDB;
            o[i] = Valid<RespFlit>{v, f};
        }
        return o;
    };
    w_clean_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        CleanInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const V& e = entries[i];
            PosClean p;
            p.hnIdx = hnIdOf(i);
            p.channel = e.task.chi.channel;
            p.qos = e.task.qos;
            o[i] = Valid<PosClean>{e.state != kFree && e.state == kClean, p};
        }
        return o;
    };

    // ---- 次态（state/task/flag/inst/alrGet/respErr） ----
    w_state_next.assign().reads(entries, w_alloc_hit, cmt_task_0, cmt_task_1, w_dec_list_in,
                                w_task_code_in, w_cmt_code_in, w_clean_in, clean_arb.in_rdy) =
        [](auto src) {
            auto [entries, w_alloc_hit, cmt_task_0, cmt_task_1, dec_list_in, task_code_in,
                  cmt_code_in, clean_in, clean_rdy] = src;
            U8ArrN o{};
            for (uint32_t i = 0; i < kEntries; ++i) {
                const V& e = entries[i];
                const auto& alloc = (i / 56 == 0) ? cmt_task_0 : cmt_task_1;
                const bool allFlagDone =
                    !e.flag.sDecode && !e.flag.sReqDB && !e.flag.sCmTask && !e.flag.sDataTask &&
                    !e.flag.sWriDir && !e.flag.wCmResp && !e.flag.wReplResp &&
                    !e.flag.wDataResp && !e.flag.sDbid && !e.flag.sResp && !e.flag.wXCB0 &&
                    !e.flag.wXCB1 && !e.flag.wCompAck;
                uint8_t n = e.state;
                switch (e.state) {
                    case kFree:
                        if (w_alloc_hit[i])
                            n = dc::tcIsValid(alloc.bits.task) ? kFstTask : kCommit;
                        break;
                    case kFstTask:
                        if (dec_list_in[i].valid)
                            n = (dc::tcIsValid(task_code_in[i]) &&
                                 dc::ccWaitSecDone(cmt_code_in[i]))
                                    ? kSecTask
                                    : kCommit;
                        break;
                    case kSecTask:
                        if (dec_list_in[i].valid) n = kCommit;
                        break;
                    case kCommit:
                        if (allFlagDone) n = kClean;
                        break;
                    case kClean:
                        if (clean_in[i].valid && clean_rdy[i]) n = kFree;
                        break;
                    default: break;
                }
                o[i] = n;
            }
            return o;
        };

    // taskNext（decListIn 换入 / alr.reqDB）
    w_task_next.assign().reads(entries, w_dec_list_in, w_task_code_in, w_cmt_code_in, w_req_db_in,
                               req_db_arb.in_rdy) = [](auto src) {
        auto [entries, dec_list_in, task_code_in, cmt_code_in, req_db_in, req_db_rdy] = src;
        TaskArrN o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const V& e = entries[i];
            CommitTask n = e.task;
            if (dec_list_in[i].valid) {
                n.decList = dec_list_in[i].bits;
                n.task = task_code_in[i];
                n.task = (n.task & ~(3u << 1)) |
                         (dc::tcSnpTgt(e.task.task) << 1);  // snpTgt 保留原值
                n.cmt = (e.state == kFstTask && dc::ccWaitSecDone(cmt_code_in[i]))
                            ? 0
                            : cmt_code_in[i];
            }
            if (req_db_in[i].valid && req_db_rdy[i]) n.alr.reqDB = true;
            o[i] = n;
        }
        return o;
    };

    // flag 次态
    w_flag_next.assign().reads(entries, w_state_next, w_alloc_hit, cmt_task_0, cmt_task_1,
                               w_task_next, w_dec_list_in, w_trd_in, trd_arb.in_rdy, w_fth_in,
                               fth_arb.in_rdy, w_req_db_in, req_db_arb.in_rdy, w_snp_in,
                               snp_arb.in_rdy, w_read_in, read_arb.in_rdy, w_wri_in,
                               wri_arb.in_rdy, w_data_task_in, data_task_arb.in_rdy, w_repl_in,
                               repl_arb.in_rdy, w_cm_resp_hit, repl_resp, data_resp, w_tx_rsp_in,
                               tx_rsp_arb.in_rdy, w_comp_ack_hit, w_xcb_hit0, w_xcb_hit1) =
        [](auto src) {
            auto [entries, state_next, alloc_hit, cmt_task_0, cmt_task_1, task_next, dec_list_in,
                  trd_in, trd_rdy, fth_in, fth_rdy, req_db_in, req_db_rdy, snp_in, snp_rdy,
                  read_in, read_rdy, wri_in, wri_rdy, data_task_in, data_task_rdy, repl_in,
                  repl_rdy, cm_resp_hit, repl_resp, data_resp, tx_rsp_in, tx_rsp_rdy,
                  comp_ack_hit, xcb_hit0, xcb_hit1] = src;
            FlagArrN o{};
            for (uint32_t i = 0; i < kEntries; ++i) {
                const V& e = entries[i];
                const auto& alloc = (i / 56 == 0) ? cmt_task_0 : cmt_task_1;
                const uint8_t hn = hnIdOf(i);
                Flag n = e.flag;
                if (alloc_hit[i] || dec_list_in[i].valid) {
                    const uint32_t task = dec_list_in[i].valid ? task_next[i].task : alloc.bits.task;
                    const uint32_t cmt = dec_list_in[i].valid ? task_next[i].cmt : alloc.bits.cmt;
                    const bool alrReqDB =
                        dec_list_in[i].valid ? e.task.alr.reqDB : alloc.bits.alr.reqDB;
                    const bool alrSendData =
                        dec_list_in[i].valid ? e.task.alr.sData : alloc.bits.alr.sData;
                    const bool needWaitAck =
                        dec_list_in[i].valid ? e.flag.wCompAck : alloc.bits.chi.expCompAck;
                    const bool needWaitData =
                        alloc_hit[i] && (dc::tcReturnDBID(alloc.bits.task) || alloc.bits.alr.sDBID);
                    const bool copyBackNeedData =
                        alloc.bits.chi.isCopyBackWrite() && needWaitData;
                    const bool replLLC = dc::ccWriLLC(cmt) && !e.task.dir.llc.hit;
                    const bool opIsValid = dc::tcSnoop(task) || dc::tcRead(task) ||
                                           dc::tcDataless(task) || dc::tcWrite(task);
                    const bool cmtDataOpValid = dc::ccOpRepl(cmt) || dc::ccOpRead(cmt) ||
                                                dc::ccOpSend(cmt) || dc::ccOpSave(cmt) ||
                                                dc::ccOpMerge(cmt);
                    const bool cmtOnlySave = !dc::ccOpRepl(cmt) && !dc::ccOpRead(cmt) &&
                                             !dc::ccOpSend(cmt) && dc::ccOpSave(cmt) &&
                                             !dc::ccOpMerge(cmt);
                    const bool cmtIsWriDir =
                        dc::ccWriSRC(cmt) || dc::ccWriSNP(cmt) || dc::ccWriLLC(cmt);

                    n.sDecode = state_next[i] == kFstTask || state_next[i] == kSecTask;
                    n.sReqDB = (dc::tcNeedDB(task) || cmtDataOpValid) && !alrReqDB;
                    n.sCmTask = opIsValid;
                    n.sDataTask = (cmtOnlySave ? !replLLC : cmtDataOpValid) && !alrSendData;
                    n.sWriDir = cmtIsWriDir;

                    n.wCmResp = n.sCmTask;
                    n.wReplResp = n.sWriDir;
                    n.wDataResp = n.sDataTask || alrSendData;

                    n.sDbid = alloc_hit[i] && dc::tcReturnDBID(alloc.bits.task) &&
                              !alloc.bits.alr.sDBID;
                    n.sResp = dc::ccSendResp(cmt) && dc::ccChannel(cmt) == kChRsp;

                    n.wXCB0 = needWaitData && (alloc.bits.chi.dataVec & 1) && !xcb_hit0[i] &&
                              !e.alrGet.ncbWrD0;
                    n.wXCB1 = needWaitData && ((alloc.bits.chi.dataVec >> 1) & 1) &&
                              !xcb_hit1[i] && !e.alrGet.ncbWrD1;
                    n.wCompAck = needWaitAck && !copyBackNeedData && !comp_ack_hit[i] &&
                                 !e.alrGet.compAck;
                } else {
                    const bool decodeFire =
                        (trd_in[i].valid && trd_rdy[i]) || (fth_in[i].valid && fth_rdy[i]);
                    const bool cmTaskHit = (snp_in[i].valid && snp_rdy[i]) ||
                                           (read_in[i].valid && read_rdy[i]) ||
                                           (wri_in[i].valid && wri_rdy[i]);
                    if (decodeFire) n.sDecode = false;
                    if (req_db_in[i].valid && req_db_rdy[i]) n.sReqDB = false;
                    if (cmTaskHit) n.sCmTask = false;
                    if (data_task_in[i].valid && data_task_rdy[i]) n.sDataTask = false;
                    if (repl_in[i].valid && repl_rdy[i]) n.sWriDir = false;
                    if (cm_resp_hit[i]) n.wCmResp = false;
                    if (repl_resp.valid && repl_resp.bits == hn) n.wReplResp = false;
                    if (data_resp.valid && data_resp.bits == hn) n.wDataResp = false;
                    if (tx_rsp_in[i].valid && tx_rsp_rdy[i]) {
                        n.sDbid = false;
                        n.sResp = false;
                    }
                    if (comp_ack_hit[i]) n.wCompAck = false;
                    if (xcb_hit0[i]) n.wXCB0 = false;
                    if (xcb_hit1[i]) n.wXCB1 = false;
                }
                o[i] = n;
            }
            return o;
        };

    // inst 次态
    w_inst_next.assign().reads(entries, w_state_next, w_alloc_hit, w_cm_resp_hit, cm_resp,
                               w_xcb_hit0, w_xcb_hit1, rx_dat) = [](auto src) {
        auto [entries, state_next, alloc_hit, cm_resp_hit, cm_resp, xcb_hit0, xcb_hit1,
              rx_dat] = src;
        U32ArrN o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const V& e = entries[i];
            uint32_t n = e.inst;
            const bool cleanInst =
                (e.state == kFstTask && state_next[i] == kSecTask) || state_next[i] == kCommit;
            if (alloc_hit[i] || cleanInst) {
                n = 0;  // TaskInst 清零
            } else if (cm_resp_hit[i]) {
                n = e.state == kFstTask ? (e.inst | cm_resp.bits.taskInst)
                                        : cm_resp.bits.taskInst;
            }
            if (cleanInst) {
                n &= ~((1u << 3) | 0x7u);  // getXCBResp=0, xCBResp=I
            } else if (xcb_hit0[i] || xcb_hit1[i]) {
                n |= (1u << 3);
                n = (n & ~0x7u) | (rx_dat.bits.resp & 0x7u);
            }
            if (cleanInst) {
                n &= ~(1u << 18);  // valid=0
            } else if (cm_resp_hit[i] || xcb_hit0[i] || xcb_hit1[i]) {
                n |= (1u << 18);
            }
            o[i] = n;
        }
        return o;
    };

    // alrGet / respErr 跟踪
    w_alr_get_next.assign().reads(entries, w_comp_ack_hit, w_xcb_hit0, w_xcb_hit1, w_clean_in,
                                  clean_arb.in_rdy) = [](auto src) {
        auto [entries, comp_ack_hit, xcb_hit0, xcb_hit1, clean_in, clean_rdy] = src;
        AlrArrN o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            AlrGet n = entries[i].alrGet;
            if (clean_in[i].valid && clean_rdy[i]) {
                n = AlrGet{};
            } else {
                n.compAck = comp_ack_hit[i] || n.compAck;
                n.ncbWrD0 = xcb_hit0[i] || n.ncbWrD0;
                n.ncbWrD1 = xcb_hit1[i] || n.ncbWrD1;
            }
            o[i] = n;
        }
        return o;
    };
    w_resp_err_next.assign().reads(entries, w_cm_resp_hit, cm_resp, w_xcb_hit0, w_xcb_hit1,
                                   rx_dat, w_clean_in, clean_arb.in_rdy) = [](auto src) {
        auto [entries, cm_resp_hit, cm_resp, xcb_hit0, xcb_hit1, rx_dat, clean_in,
              clean_rdy] = src;
        U8ArrN o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const V& e = entries[i];
            uint8_t n = e.respErr;
            if (clean_in[i].valid && clean_rdy[i]) {
                n = kErrOk;
            } else if (e.respErr != kErrDerr && e.respErr != kErrNderr) {
                const bool cmIsErr =
                    cm_resp.bits.respErr == kErrDerr || cm_resp.bits.respErr == kErrNderr;
                if (cm_resp_hit[i] && cmIsErr) {
                    n = cm_resp.bits.respErr;
                } else if (xcb_hit0[i] || xcb_hit1[i]) {
                    n = rx_dat.bits.resp_err;
                }
            }
            o[i] = n;
        }
        return o;
    };

    w_set.assign().reads(w_alloc_hit, w_valid, w_xcb_hit0, w_xcb_hit1) = [](auto src) {
        auto [alloc_hit, valid, xcb_hit0, xcb_hit1] = src;
        BoolArrN o{};
        for (uint32_t i = 0; i < kEntries; ++i)
            o[i] = alloc_hit[i] || valid[i] || xcb_hit0[i] || xcb_hit1[i];
        return o;
    };

    // ---- 寄存（112 项一条 update；w_set 门控组 + 每拍直通组） ----
    entries.update().on(posedge(clk)).reads(entries, w_set, w_alloc_hit, cmt_task_0, cmt_task_1,
                                            w_task_next, w_flag_next, w_inst_next, w_state_next,
                                            w_alr_get_next, w_resp_err_next) = [](auto src) {
        auto [entries, w_set, alloc_hit, cmt_task_0, cmt_task_1, task_next, flag_next, inst_next,
              state_next, alr_get_next, resp_err_next] = src;
        EntryArr n = entries;
        for (uint32_t i = 0; i < kEntries; ++i) {
            V nv = entries[i];
            if (w_set[i]) {
                const auto& alloc = (i / 56 == 0) ? cmt_task_0 : cmt_task_1;
                nv.task = alloc_hit[i] ? alloc.bits : task_next[i];
                nv.flag = flag_next[i];
                nv.inst = inst_next[i];
                nv.state = state_next[i];
            }
            nv.alrGet = alr_get_next[i];
            nv.respErr = resp_err_next[i];
            n[i] = nv;
        }
        return n;
    };

    // ---- 译码请求 RR 合流（validOnly：out_rdy 恒真） ----
    trd_arb.in = w_trd_in;
    trd_arb.out_rdy = true;
    fth_arb.in = w_fth_in;
    fth_arb.out_rdy = true;
    trd_dec.dec_mes_in = trd_arb.out;
    fth_dec.dec_mes_in = fth_arb.out;

    // ---- 输出仲裁 ----
    tx_rsp_arb.in = w_tx_rsp_in;
    tx_rsp_arb.out_rdy = tx_rsp_rdy;
    tx_rsp = tx_rsp_arb.out;
    snp_arb.in = w_snp_in;
    snp_arb.out_rdy = cm_task_snp_rdy;
    cm_task_snp = snp_arb.out;
    wri_arb.in = w_wri_in;
    wri_arb.out_rdy = cm_task_wri_rdy;
    cm_task_wri = wri_arb.out;
    read_arb.in = w_read_in;
    read_arb.out_rdy = cm_task_read_rdy;
    cm_task_read = read_arb.out;
    req_db_arb.in = w_req_db_in;
    req_db_arb.out_rdy = req_db_rdy;
    req_db.assign().reads(req_db_arb.out) = [](auto src) {
        auto [o] = src;
        return Valid<ReqDB>{o.valid, {o.bits.hnTxnID, o.bits.dataVec}};
    };
    repl_arb.in = w_repl_in;
    repl_arb.out_rdy = repl_task_rdy;
    repl_task = repl_arb.out;
    data_task_arb.in = w_data_task_in;
    data_task_arb.out_rdy = data_task_rdy;
    data_task = data_task_arb.out;
    clean_arb.in = w_clean_in;
    clean_arb.out_rdy = clean_pos_rdy;
    clean_pos = clean_arb.out;
}
}  // namespace zj::dj
