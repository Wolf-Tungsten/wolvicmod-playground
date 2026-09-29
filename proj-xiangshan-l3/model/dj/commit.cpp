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

// ---------------- CommitEntry ----------------

CommitEntry::CommitEntry() {
    // ---- 基本派生 ----
    w_valid.assign().reads(v) = [](auto src) {
        auto [v] = src;
        return v.state != kFree;
    };
    w_rx_rsp_hit.assign().reads(rx_rsp, hn_txn_id) = [](auto src) {
        auto [rx_rsp, hn_txn_id] = src;
        return rx_rsp.valid && rx_rsp.bits.txn_id == hn_txn_id;
    };
    w_rx_dat_hit.assign().reads(rx_dat, hn_txn_id) = [](auto src) {
        auto [rx_dat, hn_txn_id] = src;
        return rx_dat.valid && rx_dat.bits.txn_id == hn_txn_id;
    };
    w_comp_ack_hit.assign().reads(w_rx_rsp_hit, w_rx_dat_hit, rx_rsp, rx_dat) = [](auto src) {
        auto [w_rx_rsp_hit, w_rx_dat_hit, rx_rsp, rx_dat] = src;
        return (w_rx_rsp_hit && rx_rsp.bits.opcode == kCompAck) ||
               (w_rx_dat_hit && rx_dat.bits.opcode == kNCBWrDataCompAck);
    };
    w_xcb_hit0.assign().reads(w_rx_dat_hit, rx_dat) = [](auto src) {
        auto [w_rx_dat_hit, rx_dat] = src;
        const uint8_t op = rx_dat.bits.opcode;
        return w_rx_dat_hit && rx_dat.bits.data_id == 0 &&
               (op == kNonCopyBackWriteData || op == kNCBWrDataCompAck ||
                op == kCopyBackWriteData);
    };
    w_xcb_hit1.assign().reads(w_rx_dat_hit, rx_dat) = [](auto src) {
        auto [w_rx_dat_hit, rx_dat] = src;
        const uint8_t op = rx_dat.bits.opcode;
        return w_rx_dat_hit && rx_dat.bits.data_id == 2 &&
               (op == kNonCopyBackWriteData || op == kNCBWrDataCompAck ||
                op == kCopyBackWriteData);
    };
    w_cm_resp_hit.assign().reads(w_valid, cm_resp, hn_txn_id) = [](auto src) {
        auto [w_valid, cm_resp, hn_txn_id] = src;
        return w_valid && cm_resp.valid && cm_resp.bits.hnTxnID == hn_txn_id;
    };
    w_alloc_hit.assign().reads(alloc, hn_txn_id) = [](auto src) {
        auto [alloc, hn_txn_id] = src;
        return alloc.valid && alloc.bits.hnTxnID == hn_txn_id;
    };
    state_out.assign().reads(v) = [](auto src) {
        auto [v] = src;
        return v.state;
    };

    // ---- alrGet / respErr 跟踪 ----
    w_alr_get_next.assign().reads(v, w_comp_ack_hit, w_xcb_hit0, w_xcb_hit1,
                                  clean_pos, clean_pos_rdy) = [](auto src) {
        auto [v, w_comp_ack_hit, w_xcb_hit0, w_xcb_hit1, clean_pos, clean_pos_rdy] =
            src;
        const AlrGet& alr_get_reg = v.alrGet;
        AlrGet n = alr_get_reg;
        if (clean_pos.valid && clean_pos_rdy) {
            n = AlrGet{};
        } else {
            n.compAck = w_comp_ack_hit || alr_get_reg.compAck;
            n.ncbWrD0 = w_xcb_hit0 || alr_get_reg.ncbWrD0;
            n.ncbWrD1 = w_xcb_hit1 || alr_get_reg.ncbWrD1;
        }
        return n;
    };
    w_resp_err_next.assign().reads(v, w_cm_resp_hit, cm_resp, w_xcb_hit0, w_xcb_hit1,
                                   rx_dat, clean_pos, clean_pos_rdy) = [](auto src) {
        auto [v, w_cm_resp_hit, cm_resp, w_xcb_hit0, w_xcb_hit1, rx_dat, clean_pos,
              clean_pos_rdy] = src;
        const uint8_t& resp_err_reg = v.respErr;
        uint8_t n = resp_err_reg;
        if (clean_pos.valid && clean_pos_rdy) {
            n = kErrOk;
        } else if (resp_err_reg != kErrDerr && resp_err_reg != kErrNderr) {
            const bool cmIsErr = cm_resp.bits.respErr == kErrDerr || cm_resp.bits.respErr == kErrNderr;
            if (w_cm_resp_hit && cmIsErr) {
                n = cm_resp.bits.respErr;
            } else if (w_xcb_hit0 || w_xcb_hit1) {
                n = rx_dat.bits.resp_err;
            }
        }
        return n;
    };
    // ---- 输出通道 ----
    req_db.assign().reads(w_valid, v, hn_txn_id) = [](auto src) {
        auto [w_valid, v, hn_txn_id] = src;
        const Flag& flag_reg = v.flag;
        const CommitTask& task_reg = v.task;
        ReqDBQos r;
        r.hnTxnID = hn_txn_id;
        r.dataVec = (flag_reg.sCmTask && dc::tcSnoop(task_reg.task)) ? kFullVec
                                                                     : task_reg.chi.dataVec;
        r.qos = task_reg.qos;
        return Valid<ReqDBQos>{w_valid && flag_reg.sReqDB, r};
    };
    data_task.assign().reads(w_valid, v, hn_txn_id) = [](auto src) {
        auto [w_valid, v, hn_txn_id] = src;
        const Flag& flag_reg = v.flag;
        const CommitTask& task_reg = v.task;
        const uint8_t& resp_err_reg = v.respErr;
        DataTask t;
        const bool replLLC = flag_reg.sWriDir && dc::ccWriLLC(task_reg.cmt) && !task_reg.dir.llc.hit;
        t.dataOp.repl = dc::ccOpRepl(task_reg.cmt);
        t.dataOp.read = dc::ccOpRead(task_reg.cmt);
        t.dataOp.send = dc::ccOpSend(task_reg.cmt);
        t.dataOp.save = dc::ccOpSave(task_reg.cmt) && !replLLC;
        t.dataOp.merge = dc::ccOpMerge(task_reg.cmt);
        t.hnTxnID = hn_txn_id;
        t.ds = task_reg.ds;
        t.dataVec = dc::ccFullSize(task_reg.cmt) ? kFullVec : task_reg.chi.dataVec;
        t.qos = task_reg.qos;
        t.txDat.dbid = hn_txn_id;
        t.txDat.resp = static_cast<uint8_t>(dc::ccResp(task_reg.cmt));
        t.txDat.opcode = static_cast<uint8_t>(dc::ccOpcode(task_reg.cmt));
        t.txDat.txn_id = task_reg.chi.txnID;
        t.txDat.src_id = task_reg.chi.getNoC();
        t.txDat.tgt_id = task_reg.chi.nodeId;
        t.txDat.resp_err = resp_err_reg;
        return Valid<DataTask>{w_valid && flag_reg.sDataTask && task_reg.alr.reqDB, t};
    };
    repl_task.assign().reads(w_valid, v, hn_txn_id) = [](auto src) {
        auto [w_valid, v, hn_txn_id] = src;
        const Flag& flag_reg = v.flag;
        const CommitTask& task_reg = v.task;
        ReplTask t;
        t.hnTxnID = hn_txn_id;
        t.qos = task_reg.qos;
        t.wriSF = dc::ccWriSRC(task_reg.cmt) || dc::ccWriSNP(task_reg.cmt);
        t.dir.sf.hit = task_reg.dir.sf.hit;
        t.dir.sf.wayOH = task_reg.dir.sf.wayOH;
        t.directAllocSF = t.wriSF && !task_reg.dir.sf.hit && task_reg.dir.sf.meta == 0;
        // sf metaVec 单 meta：srcVec/snpVec 只可能命中 meta0
        const bool srcVec0 = true;  // metaIdOH 恒 1（nrSfMetas=1）
        const uint32_t snpTgt = dc::tcSnpTgt(task_reg.task);
        const bool snpVec0 =
            snpTgt == 1 && task_reg.dir.sf.meta != 0;  // SnpTgt.ALL=b01；单 meta 时 ONE/OTH→0
        uint8_t meta0;
        if (srcVec0 && dc::ccWriSRC(task_reg.cmt)) {
            meta0 = dc::ccSrcValid(task_reg.cmt);
        } else if (snpVec0 && dc::ccWriSNP(task_reg.cmt)) {
            meta0 = dc::ccSnpValid(task_reg.cmt);
        } else {
            meta0 = task_reg.dir.sf.hit ? task_reg.dir.sf.meta : 0;
        }
        t.dir.sf.meta = meta0;
        t.wriLLC = dc::ccWriLLC(task_reg.cmt);
        t.dir.llc.hit = task_reg.dir.llc.hit;
        t.dir.llc.wayOH = task_reg.dir.llc.wayOH;
        t.dir.llc.meta = static_cast<uint8_t>(dc::ccLlcState(task_reg.cmt));
        return Valid<ReplTask>{w_valid && flag_reg.sWriDir && !flag_reg.wDataResp, t};
    };

    // cmTask 三路（bits 共享，valid 分流）
    cm_task_snp.assign().reads(w_valid, v, hn_txn_id) = [](auto src) {
        auto [w_valid, v, hn_txn_id] = src;
        const Flag& flag_reg = v.flag;
        const CommitTask& task_reg = v.task;
        CMTask t;
        t.chi = task_reg.chi;
        t.chi.channel = dc::tcSnoop(task_reg.task) ? kChSnp : kChReq;
        t.chi.dataVec = (dc::tcSnoop(task_reg.task) || dc::tcFullSize(task_reg.task))
                            ? kFullVec
                            : task_reg.chi.dataVec;
        t.chi.opcode = static_cast<uint8_t>(dc::tcOpcode(task_reg.task));
        t.chi.expCompAck = dc::tcExpCompAck(task_reg.task);
        t.chi.retToSrc = dc::tcRetToSrc(task_reg.task);
        t.chi.size = (dc::tcSnoop(task_reg.task) || dc::tcFullSize(task_reg.task))
                         ? 6
                         : task_reg.chi.size;
        t.hnTxnID = hn_txn_id;
        t.dataOp.repl = dc::tcOpRepl(task_reg.task);
        t.dataOp.read = dc::tcOpRead(task_reg.task);
        t.dataOp.send = dc::tcOpSend(task_reg.task);
        t.dataOp.save = dc::tcOpSave(task_reg.task);
        t.dataOp.merge = dc::tcOpMerge(task_reg.task);
        t.ds = task_reg.ds;
        const uint32_t snpTgt = dc::tcSnpTgt(task_reg.task);
        t.snpVec = snpTgt == 1 && task_reg.dir.sf.meta != 0 ? 1 : 0;
        t.fromRepl = false;
        // cbResp：llc meta 的 cbResp（I→I, SC→SC, UC→UC, UD→UD_PD）
        const uint8_t st = task_reg.dir.llc.meta;
        t.cbResp = st == 0 ? 0 : st == 1 ? 1 : st == 3 ? 2 : 6;  // I/SC/UC/UD→I/SC/UC/UD_PD
        t.doDMT = dc::tcDoDMT(task_reg.task);
        t.qos = task_reg.qos;
        const bool valid = w_valid && flag_reg.sCmTask && !flag_reg.sReqDB &&
                           dc::tcSnoop(task_reg.task);
        return Valid<CMTask>{valid, t};
    };
    cm_task_read.assign().reads(w_valid, v, cm_task_snp) = [](auto src) {
        auto [w_valid, v, snp_ch] = src;
        const Flag& flag_reg = v.flag;
        const CommitTask& task_reg = v.task;
        CMTask t = snp_ch.bits;
        return Valid<CMTask>{w_valid && flag_reg.sCmTask && !flag_reg.sReqDB &&
                                 dc::tcRead(task_reg.task),
                             t};
    };
    cm_task_wri.assign().reads(w_valid, v, cm_task_snp) = [](auto src) {
        auto [w_valid, v, snp_ch] = src;
        const Flag& flag_reg = v.flag;
        const CommitTask& task_reg = v.task;
        CMTask t = snp_ch.bits;
        return Valid<CMTask>{w_valid && flag_reg.sCmTask && !flag_reg.sReqDB &&
                                 dc::tcWrite(task_reg.task),
                             t};
    };

    tx_rsp.assign().reads(w_valid, v, hn_txn_id) = [](auto src) {
        auto [w_valid, v, hn_txn_id] = src;
        const Flag& flag_reg = v.flag;
        const CommitTask& task_reg = v.task;
        const uint8_t& resp_err_reg = v.respErr;
        RespFlit f{};
        f.src_id = task_reg.chi.getNoC();
        f.tgt_id = task_reg.chi.nodeId;
        f.txn_id = task_reg.chi.txnID;
        f.dbid = hn_txn_id;
        f.opcode = flag_reg.sResp ? static_cast<uint8_t>(dc::ccOpcode(task_reg.cmt))
                                  : (task_reg.chi.isCopyBackWrite() ? kCompDBIDResp : kDBIDResp);
        f.fwd_state = static_cast<uint8_t>(dc::ccFwdResp(task_reg.cmt));
        f.resp = static_cast<uint8_t>(dc::ccResp(task_reg.cmt));
        f.qos = task_reg.qos;
        f.resp_err = resp_err_reg;
        const bool valid = w_valid && (flag_reg.sDbid || flag_reg.sResp) && !flag_reg.sReqDB;
        return Valid<RespFlit>{valid, f};
    };

    // 译码请求
    trd_dec_out.assign().reads(v, hn_txn_id) = [](auto src) {
        auto [v, hn_txn_id] = src;
        const uint8_t& state_reg = v.state;
        const Flag& flag_reg = v.flag;
        const uint32_t& inst_reg = v.inst;
        const CommitTask& task_reg = v.task;
        const bool decValid = flag_reg.sDecode &&
                              !(flag_reg.wCmResp || flag_reg.wXCB0 || flag_reg.wXCB1);
        DecMes m;
        m.taskInst = inst_reg;
        m.decList = task_reg.decList;
        m.hnTxnID = hn_txn_id;
        return Valid<DecMes>{decValid && state_reg == kFstTask, m};
    };
    fth_dec_out.assign().reads(v, hn_txn_id) = [](auto src) {
        auto [v, hn_txn_id] = src;
        const uint8_t& state_reg = v.state;
        const Flag& flag_reg = v.flag;
        const uint32_t& inst_reg = v.inst;
        const CommitTask& task_reg = v.task;
        const bool decValid = flag_reg.sDecode &&
                              !(flag_reg.wCmResp || flag_reg.wXCB0 || flag_reg.wXCB1);
        DecMes m;
        m.taskInst = inst_reg;
        m.decList = task_reg.decList;
        m.hnTxnID = hn_txn_id;
        return Valid<DecMes>{decValid && state_reg == kSecTask, m};
    };

    clean_pos.assign().reads(w_valid, v, hn_idx) = [](auto src) {
        auto [w_valid, v, hn_idx] = src;
        const uint8_t& state_reg = v.state;
        const CommitTask& task_reg = v.task;
        PosClean p;
        p.hnIdx = hn_idx;
        p.channel = task_reg.chi.channel;
        p.qos = task_reg.qos;
        return Valid<PosClean>{w_valid && state_reg == kClean, p};
    };

    // ---- 状态机（先算 stateNext，供 flag/inst 用） ----
    w_state_next.assign().reads(v, w_alloc_hit, alloc, dec_list_in, task_code_in,
                                cmt_code_in, clean_pos, clean_pos_rdy) = [](auto src) {
        auto [v, w_alloc_hit, alloc, dec_list_in, task_code_in, cmt_code_in,
              clean_pos, clean_pos_rdy] = src;
        const uint8_t& state_reg = v.state;
        const Flag& flag_reg = v.flag;
        const bool allFlagDone = !flag_reg.sDecode && !flag_reg.sReqDB && !flag_reg.sCmTask &&
                                 !flag_reg.sDataTask && !flag_reg.sWriDir &&
                                 !flag_reg.wCmResp && !flag_reg.wReplResp &&
                                 !flag_reg.wDataResp && !flag_reg.sDbid && !flag_reg.sResp &&
                                 !flag_reg.wXCB0 && !flag_reg.wXCB1 && !flag_reg.wCompAck;
        uint8_t n = state_reg;
        switch (state_reg) {
            case kFree:
                if (w_alloc_hit)
                    n = dc::tcIsValid(alloc.bits.task) ? kFstTask : kCommit;
                break;
            case kFstTask:
                if (dec_list_in.valid)
                    n = (dc::tcIsValid(task_code_in) && dc::ccWaitSecDone(cmt_code_in))
                            ? kSecTask
                            : kCommit;
                break;
            case kSecTask:
                if (dec_list_in.valid) n = kCommit;
                break;
            case kCommit:
                if (allFlagDone) n = kClean;
                break;
            case kClean:
                if (clean_pos.valid && clean_pos_rdy) n = kFree;
                break;
            default: break;
        }
        return n;
    };

    // ---- taskNext（decListIn 换入 / alr.reqDB） ----
    w_task_next.assign().reads(v, dec_list_in, task_code_in, cmt_code_in,
                               req_db, req_db_rdy) = [](auto src) {
        auto [v, dec_list_in, task_code_in, cmt_code_in, req_db, req_db_rdy] =
            src;
        const CommitTask& task_reg = v.task;
        const uint8_t& state_reg = v.state;
        CommitTask n = task_reg;
        if (dec_list_in.valid) {
            n.decList = dec_list_in.bits;
            n.task = task_code_in;
            n.task = (n.task & ~(3u << 1)) |
                     (dc::tcSnpTgt(task_reg.task) << 1);  // snpTgt 保留原值
            n.cmt = (state_reg == kFstTask && dc::ccWaitSecDone(cmt_code_in)) ? 0 : cmt_code_in;
        }
        if (req_db.valid && req_db_rdy) n.alr.reqDB = true;
        return n;
    };

    // ---- flag 次态 ----
    w_flag_next.assign().reads(v, w_state_next, w_alloc_hit, alloc,
                               w_task_next, dec_list_in, trd_dec_out, trd_dec_out_rdy,
                               fth_dec_out, fth_dec_out_rdy, req_db, req_db_rdy, cm_task_snp,
                               cm_task_snp_rdy, cm_task_read, cm_task_read_rdy, cm_task_wri,
                               cm_task_wri_rdy, data_task, data_task_rdy, repl_task,
                               repl_task_rdy, w_cm_resp_hit, repl_resp, data_resp, hn_txn_id,
                               tx_rsp, tx_rsp_rdy, w_comp_ack_hit, w_xcb_hit0,
                               w_xcb_hit1) = [](auto src) {
        auto [v, w_state_next, w_alloc_hit, alloc, w_task_next,
              dec_list_in, trd_dec_out, trd_dec_out_rdy, fth_dec_out, fth_dec_out_rdy, req_db,
              req_db_rdy, cm_task_snp, cm_task_snp_rdy, cm_task_read, cm_task_read_rdy,
              cm_task_wri, cm_task_wri_rdy, data_task, data_task_rdy, repl_task, repl_task_rdy,
              w_cm_resp_hit, repl_resp, data_resp, hn_txn_id, tx_rsp, tx_rsp_rdy,
              w_comp_ack_hit, w_xcb_hit0, w_xcb_hit1] = src;
        const Flag& flag_reg = v.flag;
        const uint8_t& state_reg = v.state;
        const CommitTask& task_reg = v.task;
        const AlrGet& alr_get_reg = v.alrGet;
        Flag n = flag_reg;
        if (w_alloc_hit || dec_list_in.valid) {
            const uint32_t task = dec_list_in.valid ? w_task_next.task : alloc.bits.task;
            const uint32_t cmt = dec_list_in.valid ? w_task_next.cmt : alloc.bits.cmt;
            const bool alrReqDB = dec_list_in.valid ? task_reg.alr.reqDB : alloc.bits.alr.reqDB;
            const bool alrSendData =
                dec_list_in.valid ? task_reg.alr.sData : alloc.bits.alr.sData;
            const bool needWaitAck =
                dec_list_in.valid ? flag_reg.wCompAck : alloc.bits.chi.expCompAck;
            const bool needWaitData =
                w_alloc_hit && (dc::tcReturnDBID(alloc.bits.task) || alloc.bits.alr.sDBID);
            const bool copyBackNeedData = alloc.bits.chi.isCopyBackWrite() && needWaitData;
            const bool replLLC = dc::ccWriLLC(cmt) && !task_reg.dir.llc.hit;
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

            n.sDecode = w_state_next == kFstTask || w_state_next == kSecTask;
            n.sReqDB = (dc::tcNeedDB(task) || cmtDataOpValid) && !alrReqDB;
            n.sCmTask = opIsValid;
            n.sDataTask = (cmtOnlySave ? !replLLC : cmtDataOpValid) && !alrSendData;
            n.sWriDir = cmtIsWriDir;

            n.wCmResp = n.sCmTask;
            n.wReplResp = n.sWriDir;
            n.wDataResp = n.sDataTask || alrSendData;

            n.sDbid = w_alloc_hit && dc::tcReturnDBID(alloc.bits.task) && !alloc.bits.alr.sDBID;
            n.sResp = dc::ccSendResp(cmt) && dc::ccChannel(cmt) == kChRsp;

            n.wXCB0 = needWaitData && (alloc.bits.chi.dataVec & 1) && !w_xcb_hit0 &&
                      !alr_get_reg.ncbWrD0;
            n.wXCB1 = needWaitData && ((alloc.bits.chi.dataVec >> 1) & 1) && !w_xcb_hit1 &&
                      !alr_get_reg.ncbWrD1;
            n.wCompAck = needWaitAck && !copyBackNeedData && !w_comp_ack_hit &&
                         !alr_get_reg.compAck;
        } else {
            const bool decodeFire =
                (trd_dec_out.valid && trd_dec_out_rdy) || (fth_dec_out.valid && fth_dec_out_rdy);
            const bool cmTaskHit = (cm_task_snp.valid && cm_task_snp_rdy) ||
                                   (cm_task_read.valid && cm_task_read_rdy) ||
                                   (cm_task_wri.valid && cm_task_wri_rdy);
            if (decodeFire) n.sDecode = false;
            if (req_db.valid && req_db_rdy) n.sReqDB = false;
            if (cmTaskHit) n.sCmTask = false;
            if (data_task.valid && data_task_rdy) n.sDataTask = false;
            if (repl_task.valid && repl_task_rdy) n.sWriDir = false;
            if (w_cm_resp_hit) n.wCmResp = false;
            if (repl_resp.valid && repl_resp.bits == hn_txn_id) n.wReplResp = false;
            if (data_resp.valid && data_resp.bits == hn_txn_id) n.wDataResp = false;
            if (tx_rsp.valid && tx_rsp_rdy) {
                n.sDbid = false;
                n.sResp = false;
            }
            if (w_comp_ack_hit) n.wCompAck = false;
            if (w_xcb_hit0) n.wXCB0 = false;
            if (w_xcb_hit1) n.wXCB1 = false;
        }
        return n;
    };

    // ---- inst 次态 ----
    w_inst_next.assign().reads(v, w_state_next, w_alloc_hit, w_cm_resp_hit,
                               cm_resp, w_xcb_hit0, w_xcb_hit1, rx_dat) = [](auto src) {
        auto [v, w_state_next, w_alloc_hit, w_cm_resp_hit, cm_resp,
              w_xcb_hit0, w_xcb_hit1, rx_dat] = src;
        const uint32_t& inst_reg = v.inst;
        const uint8_t& state_reg = v.state;
        uint32_t n = inst_reg;
        const bool cleanInst =
            (state_reg == kFstTask && w_state_next == kSecTask) || w_state_next == kCommit;
        if (w_alloc_hit || cleanInst) {
            n = 0;  // TaskInst 清零
        } else if (w_cm_resp_hit) {
            n = state_reg == kFstTask ? (inst_reg | cm_resp.bits.taskInst) : cm_resp.bits.taskInst;
        }
        if (cleanInst) {
            n &= ~((1u << 3) | 0x7u);  // getXCBResp=0, xCBResp=I
        } else if (w_xcb_hit0 || w_xcb_hit1) {
            n |= (1u << 3);
            n = (n & ~0x7u) | (rx_dat.bits.resp & 0x7u);
        }
        if (cleanInst) {
            n &= ~(1u << 18);  // valid=0
        } else if (w_cm_resp_hit || w_xcb_hit0 || w_xcb_hit1) {
            n |= (1u << 18);
        }
        return n;
    };

    // ---- 寄存 ----
    w_set.assign().reads(w_alloc_hit, w_valid, w_xcb_hit0, w_xcb_hit1) = [](auto src) {
        auto [w_alloc_hit, w_valid, w_xcb_hit0, w_xcb_hit1] = src;
        return w_alloc_hit || w_valid || w_xcb_hit0 || w_xcb_hit1;
    };
    v.update().on(posedge(clk)).reads(v, w_set, w_alloc_hit, alloc, w_task_next, w_flag_next,
                                      w_inst_next, w_state_next, w_alr_get_next,
                                      w_resp_err_next) = [](auto src) {
        auto [v, w_set, w_alloc_hit, alloc, w_task_next, w_flag_next, w_inst_next, w_state_next,
              w_alr_get_next, w_resp_err_next] = src;
        V n = v;
        if (w_set) {
            n.task = w_alloc_hit ? alloc.bits : w_task_next;
            n.flag = w_flag_next;
            n.inst = w_inst_next;
            n.state = w_state_next;
        }
        n.alrGet = w_alr_get_next;
        n.respErr = w_resp_err_next;
        return n;
    };
}


// ---------------- Commit ----------------

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

    for (uint32_t i = 0; i < kEntries; ++i) {
        const uint32_t bank = i / 56, set = (i % 56) / 14, way = i % 14;
        auto& e = entries[i];
        e.clk = clk;
        e.cfg_ci = cfg_ci;
        e.cfg_bank_id = cfg_bank_id;
        e.hn_txn_id = static_cast<uint8_t>(bank * 64 + set * 16 + way);
        e.hn_idx = static_cast<uint8_t>((bank << 6) | (set << 4) | way);
        e.alloc = bank == 0 ? cmt_task_0 : cmt_task_1;
        e.rx_rsp = rx_rsp;
        e.rx_dat = rx_dat;
        e.cm_resp = cm_resp;
        e.repl_resp = repl_resp;
        e.data_resp = data_resp;

        // 译码结果回灌（按 hnTxnID 匹配；trd 优先）
        e.dec_list_in.assign().reads(trd_dec.hn_txn_id_out, fth_dec.hn_txn_id_out,
                                     trd_dec.dec_list_out, fth_dec.dec_list_out,
                                     e.hn_txn_id) = [](auto src) {
            auto [trd_id, fth_id, trd_list, fth_list, hn_txn_id] = src;
            const bool trdHit = trd_id.valid && trd_id.bits == hn_txn_id;
            const bool fthHit = fth_id.valid && fth_id.bits == hn_txn_id;
            return Valid<CommitEntry::DecListArr>{trdHit || fthHit,
                                                  trdHit ? trd_list : fth_list};
        };
        e.task_code_in.assign().reads(trd_dec.task_code_out, fth_dec.task_code_out,
                                      trd_dec.hn_txn_id_out, fth_dec.hn_txn_id_out,
                                      e.hn_txn_id) = [](auto src) {
            auto [trd_code, fth_code, trd_id, fth_id, hn_txn_id] = src;
            const bool trdHit = trd_id.valid && trd_id.bits == hn_txn_id;
            return trdHit ? trd_code : fth_code;
        };
        e.cmt_code_in.assign().reads(trd_dec.cmt_code_out, fth_dec.cmt_code_out,
                                     trd_dec.hn_txn_id_out, fth_dec.hn_txn_id_out,
                                     e.hn_txn_id) = [](auto src) {
            auto [trd_code, fth_code, trd_id, fth_id, hn_txn_id] = src;
            const bool trdHit = trd_id.valid && trd_id.bits == hn_txn_id;
            return trdHit ? trd_code : fth_code;
        };
    }

    // 译码请求 RR 合流（validOnly：out_rdy 恒真）
    combine(w_trd_in, entries,
            [](CommitEntry& e) -> wolvicmod::Out<Valid<DecMes>>& { return e.trd_dec_out; });
    combine(w_fth_in, entries,
            [](CommitEntry& e) -> wolvicmod::Out<Valid<DecMes>>& { return e.fth_dec_out; });
    trd_arb.in = w_trd_in;
    trd_arb.out_rdy = true;
    fth_arb.in = w_fth_in;
    fth_arb.out_rdy = true;
    trd_dec.dec_mes_in = trd_arb.out;
    fth_dec.dec_mes_in = fth_arb.out;
    for (uint32_t i = 0; i < kEntries; ++i) {
        entries[i].trd_dec_out_rdy.assign().reads(trd_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].fth_dec_out_rdy.assign().reads(fth_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
    }

    // 输出汇集 + QosRR 仲裁
    combine(w_tx_rsp_in, entries,
            [](CommitEntry& e) -> wolvicmod::Out<Valid<RespFlit>>& { return e.tx_rsp; });
    combine(w_snp_in, entries,
            [](CommitEntry& e) -> wolvicmod::Out<Valid<CMTask>>& { return e.cm_task_snp; });
    combine(w_wri_in, entries,
            [](CommitEntry& e) -> wolvicmod::Out<Valid<CMTask>>& { return e.cm_task_wri; });
    combine(w_read_in, entries,
            [](CommitEntry& e) -> wolvicmod::Out<Valid<CMTask>>& { return e.cm_task_read; });
    combine(w_req_db_in, entries,
            [](CommitEntry& e) -> wolvicmod::Out<Valid<ReqDBQos>>& { return e.req_db; });
    combine(w_repl_in, entries,
            [](CommitEntry& e) -> wolvicmod::Out<Valid<ReplTask>>& { return e.repl_task; });
    combine(w_data_task_in, entries,
            [](CommitEntry& e) -> wolvicmod::Out<Valid<DataTask>>& { return e.data_task; });
    combine(w_clean_in, entries,
            [](CommitEntry& e) -> wolvicmod::Out<Valid<PosClean>>& { return e.clean_pos; });

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

    for (uint32_t i = 0; i < kEntries; ++i) {
        entries[i].tx_rsp_rdy.assign().reads(tx_rsp_arb.in_rdy) = [i](auto src) {
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
        entries[i].cm_task_read_rdy.assign().reads(read_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].req_db_rdy.assign().reads(req_db_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].repl_task_rdy.assign().reads(repl_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].data_task_rdy.assign().reads(data_task_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].clean_pos_rdy.assign().reads(clean_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
    }
}
}  // namespace zj::dj
