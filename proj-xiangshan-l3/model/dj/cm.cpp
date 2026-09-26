#include <wolvicmod/wolvicmod.h>

#include "model/dj/cm.h"

namespace zj::dj {

namespace dc = dectab;

namespace {
constexpr uint8_t kChReq = 0, kChDat = 1, kChRsp = 2, kChSnp = 3;
constexpr uint8_t kFullVec = 0x3;
// RspOpcode
constexpr uint8_t kSnpResp = 0x1, kCompAck = 0x2, kComp = 0x4, kCompDBIDResp = 0x5,
                  kDBIDResp = 0x6, kSnpRespFwded = 0x9;
// DatOpcode
constexpr uint8_t kSnpRespData = 0x1, kCopyBackWriteData = 0x2, kNonCopyBackWriteData = 0x3,
                  kCompData = 0x4, kSnpRespDataFwded = 0x6;
// SnpOpcode
constexpr uint8_t kSnpUniqueFwd = 0x17, kSnpMakeInvalid = 0x0a;
// ChiResp
constexpr uint8_t kRespI = 0, kRespSC = 1, kRespUC = 2, kRespUD = 3, kRespSdPd = 7;
// RespErr
constexpr uint8_t kErrOk = 0, kErrDerr = 2, kErrNderr = 3;
}  // namespace

// ---------------- SnoopEntry ----------------

SnoopEntry::SnoopEntry() {
    alloc_rdy.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        return reg.state == kFree;
    };
    w_rsp_hit.assign().reads(reg, rx_rsp) = [](auto src) {
        auto [reg, rx_rsp] = src;
        return reg.state != kFree && rx_rsp.valid && reg.task.hnTxnID == rx_rsp.bits.txn_id &&
               (rx_rsp.bits.opcode == kSnpResp || rx_rsp.bits.opcode == kSnpRespFwded);
    };
    w_dat_hit.assign().reads(reg, rx_dat) = [](auto src) {
        auto [reg, rx_dat] = src;
        return reg.state != kFree && rx_dat.valid && reg.task.hnTxnID == rx_dat.bits.txn_id &&
               (rx_dat.bits.opcode == kSnpRespData || rx_dat.bits.opcode == kSnpRespDataFwded);
    };

    tx_snp.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        SnoopFlit f{};
        const uint8_t pend = reg.task.snpVec ^ (reg.alrSend ? 1 : 0);  // 1bit
        const bool snpIsLast = pend == 1;  // PopCount(pend)===1（pend 至多 1）
        f.ret_to_src = snpIsLast && reg.task.chi.retToSrc;
        f.do_not_go_to_sd = true;
        f.opcode = reg.state == kPreSnp ? kSnpMakeInvalid : reg.task.chi.opcode;
        f.fwd_txn_id = reg.task.chi.txnID;
        f.fwd_nid = reg.task.chi.nodeId;
        f.txn_id = reg.task.hnTxnID;
        f.src_id = 0;  // LAN
        f.tgt_id = 0x09;  // setSnpNodeId（本配置单 CC：0x08|1）
        f.qos = reg.task.qos;
        return Valid<SnoopFlit>{reg.state == kPreSnp || reg.state == kSendSnp, f};
    };
    resp.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        CMResp r;
        r.hnTxnID = reg.task.hnTxnID;
        r.taskInst = reg.taskInst;
        r.toRepl = reg.task.fromRepl;
        r.qos = reg.task.qos;
        r.respErr = reg.respErr;
        return Valid<CMResp>{reg.state == kRespCmt, r};
    };

    w_next.assign().reads(reg, alloc, alloc_rdy, tx_snp, tx_snp_rdy, w_rsp_hit, w_dat_hit,
                          rx_rsp, rx_dat, resp, resp_rdy) = [](auto src) {
        auto [reg, alloc, alloc_rdy, tx_snp, tx_snp_rdy, w_rsp_hit, w_dat_hit, rx_rsp, rx_dat,
              resp, resp_rdy] = src;
        SnpReg n = reg;
        const bool rspIsFwd = w_rsp_hit && rx_rsp.bits.opcode == kSnpRespFwded;
        const bool datIsFwd = w_dat_hit && rx_dat.bits.opcode == kSnpRespDataFwded;

        if (alloc.valid && alloc_rdy) {
            n.task = alloc.bits;
            n.taskInst = 0;
            n.respErr = kErrOk;
        } else if (w_rsp_hit || w_dat_hit) {
            uint32_t ti = reg.taskInst | (1u << 18);
            // fwdValid
            const bool fwdV = dc::tiFwdValid(reg.taskInst) || rspIsFwd || datIsFwd;
            ti = (ti & ~(1u << 17)) | (static_cast<uint32_t>(fwdV) << 17);
            // fwdResp
            const uint32_t fwdR = rspIsFwd ? rx_rsp.bits.fwd_state
                                  : datIsFwd ? rx_dat.bits.data_source
                                             : dc::tiFwdResp(reg.taskInst);
            ti = (ti & ~(7u << 4)) | ((fwdR & 7u) << 4);
            // channel：datHit→DAT；rspHit→保持 DAT 或 RSP
            const uint32_t ch = w_dat_hit ? kChDat
                                : w_rsp_hit ? (dc::tiChannel(reg.taskInst) == kChDat ? kChDat
                                                                                     : kChRsp)
                                            : dc::tiChannel(reg.taskInst);
            ti = (ti & ~(3u << 15)) | ((ch & 3u) << 15);
            // opcode
            const uint32_t op =
                w_dat_hit ? rx_dat.bits.opcode
                : w_rsp_hit
                    ? ((dc::tiChannel(reg.taskInst) == kChDat || fwdV) ? dc::tiOpcode(reg.taskInst)
                                                                       : rx_rsp.bits.opcode)
                    : dc::tiOpcode(reg.taskInst);
            ti = (ti & ~(0x1Fu << 10)) | ((op & 0x1Fu) << 10);
            // resp（不劣化：resp 较大者保持）
            const uint32_t rp =
                w_dat_hit ? rx_dat.bits.resp
                : w_rsp_hit
                    ? ((dc::tiChannel(reg.taskInst) == kChDat ||
                        dc::tiResp(reg.taskInst) >= rx_rsp.bits.resp)
                           ? dc::tiResp(reg.taskInst)
                           : rx_rsp.bits.resp)
                    : dc::tiResp(reg.taskInst);
            ti = (ti & ~(7u << 7)) | ((rp & 7u) << 7);
            n.taskInst = ti;
            // respErr（首个非 OK）
            if (reg.respErr != kErrDerr && reg.respErr != kErrNderr) {
                n.respErr = w_dat_hit ? rx_dat.bits.resp_err : rx_rsp.bits.resp_err;
            }
        }

        if (alloc.valid && alloc_rdy) {
            n.alrSend = false;
            n.getResp = false;
            n.getData = 0;
        } else {
            const bool snpFire = tx_snp.valid && tx_snp_rdy;
            n.alrSend = reg.alrSend || snpFire;
            const uint8_t beatId = rx_dat.valid && rx_dat.bits.data_id == 2 ? 1 : 0;
            n.getData = reg.getData | (w_dat_hit ? static_cast<uint8_t>(1u << beatId) : 0);
            n.getResp = reg.getResp || w_rsp_hit || w_dat_hit;  // metaId 恒 0
        }

        // 状态机
        const uint8_t pend = reg.task.snpVec ^ (reg.alrSend ? 1 : 0);
        const bool snpFire = tx_snp.valid && tx_snp_rdy;
        const bool alrSnpAll = pend == 1 && snpFire;       // 最后一发
        const bool snpLastNext = pend == 0 && snpFire && false;  // PRESNP 专用（见下）
        const bool alrGetRspAll = (reg.task.snpVec ^ (n.getResp ? 1 : 0)) == 0;
        const bool dataOne = (n.getData == 1) || (n.getData == 2);
        const bool alrGetAll = alrGetRspAll && !dataOne;
        switch (reg.state) {
            case kFree:
                if (alloc.valid && alloc_rdy) {
                    const bool snpUniqueFwdMoreThan1 =
                        alloc.bits.chi.opcode == kSnpUniqueFwd &&
                        __builtin_popcount(alloc.bits.snpVec) > 1;
                    n.state = snpUniqueFwdMoreThan1 ? kPreSnp : kSendSnp;
                }
                break;
            case kPreSnp:
                // 倒数第二发（pend==1... 待发 2 个时 fires 后转 SENDSNP）：
                // 本配置 snpVec 单位，不会到达（保留结构）
                if (snpLastNext) n.state = kSendSnp;
                break;
            case kSendSnp:
                if (alrSnpAll) n.state = kWaitResp;
                break;
            case kWaitResp:
                if (alrGetAll) n.state = kRespCmt;
                break;
            case kRespCmt:
                if (resp.valid && resp_rdy) n.state = kFree;
                break;
            default: break;
        }
        return n;
    };
    w_set.assign().reads(reg, alloc, alloc_rdy) = [](auto src) {
        auto [reg, alloc, alloc_rdy] = src;
        return (alloc.valid && alloc_rdy) || reg.state != kFree;
    };
    reg.update().on(posedge(clk)).reads(reg, w_next, w_set) = [](auto src) {
        auto [reg, w_next, w_set] = src;
        return w_set ? w_next : reg;
    };
}

SnoopCM::SnoopCM() {
    alloc_arb.clk = clk;
    tx_snp_arb.clk = clk;
    resp_arb.clk = clk;
    for (uint32_t i = 0; i < kEntries; ++i) {
        entries[i].clk = clk;
        entries[i].rx_rsp = rx_rsp;
        entries[i].rx_dat = rx_dat;
        entries[i].tx_snp_rdy.assign().reads(tx_snp_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].resp_rdy.assign().reads(resp_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
    }
    alloc_arb.in = alloc;
    alloc_rdy = alloc_arb.in_rdy;
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
    combine(w_tx_snp_in, entries,
            [](SnoopEntry& e) -> wolvicmod::Out<Valid<SnoopFlit>>& { return e.tx_snp; });
    combine(w_resp_in, entries,
            [](SnoopEntry& e) -> wolvicmod::Out<Valid<CMResp>>& { return e.resp; });
    tx_snp_arb.in = w_tx_snp_in;
    tx_snp_arb.out_rdy = tx_snp_rdy;
    tx_snp = tx_snp_arb.out;
    resp_arb.in = w_resp_in;
    resp_arb.out_rdy = resp_rdy;
    resp = resp_arb.out;
}

// ---------------- ReadEntry ----------------

ReadEntry::ReadEntry() {
    alloc_rdy.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        return reg.state == kFree;
    };
    w_rec_data_hit.assign().reads(reg, rx_dat) = [](auto src) {
        auto [reg, rx_dat] = src;
        return reg.state != kFree && rx_dat.valid && reg.task.hnTxnID == rx_dat.bits.txn_id &&
               rx_dat.bits.opcode == kCompData;
    };

    tx_req.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        HReqFlit f{};
        f.exp_comp_ack = reg.task.chi.expCompAck;
        f.mem_attr = reg.task.chi.memAttr;
        f.order = 0;
        f.size = reg.task.chi.size;
        f.opcode = reg.task.chi.opcode;
        f.return_txn_id = reg.task.doDMT ? reg.task.chi.txnID : reg.task.hnTxnID;
        f.return_nid = reg.task.doDMT ? reg.task.chi.nodeId : 0x7FF;
        f.txn_id = reg.task.hnTxnID;
        f.src_id = reg.task.chi.getNoC();
        f.qos = reg.task.qos;
        return Valid<HReqFlit>{reg.state == kSendReq, f};
    };
    resp.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        CMResp r{};
        r.hnTxnID = reg.task.hnTxnID;
        r.toRepl = false;
        r.qos = reg.task.qos;
        if (!reg.task.doDMT) {
            uint32_t ti = 0;
            ti |= (1u << 18);            // valid
            ti |= (kChDat << 15);        // channel = DAT
            ti |= (static_cast<uint32_t>(kCompData) << 10);
            ti |= (static_cast<uint32_t>(dc::tiResp(reg.taskInst)) << 7);
            ti |= (static_cast<uint32_t>(kRespI) << 4);  // fwdResp = I
            r.taskInst = ti;
            r.respErr = reg.respErr;
        } else {
            r.taskInst = (1u << 18);
            r.respErr = kErrOk;
        }
        return Valid<CMResp>{reg.state == kRespCmt, r};
    };

    w_next.assign().reads(reg, alloc, alloc_rdy, tx_req, tx_req_rdy, w_rec_data_hit, rx_dat,
                          resp, resp_rdy) = [](auto src) {
        auto [reg, alloc, alloc_rdy, tx_req, tx_req_rdy, w_rec_data_hit, rx_dat, resp,
              resp_rdy] = src;
        RdReg n = reg;
        if (alloc.valid && alloc_rdy) {
            n.task = alloc.bits;
            n.taskInst = 0;
            n.respErr = kErrOk;
        } else if (w_rec_data_hit) {
            n.task.chi.nodeId = rx_dat.bits.home_nid;
            n.task.chi.txnID = rx_dat.bits.dbid;
            n.taskInst = (reg.taskInst & ~(7u << 7)) |
                         ((static_cast<uint32_t>(rx_dat.bits.resp) & 7u) << 7);
            if (reg.respErr != kErrDerr && reg.respErr != kErrNderr)
                n.respErr = rx_dat.bits.resp_err;
        }
        switch (reg.state) {
            case kFree:
                if (alloc.valid && alloc_rdy) n.state = kSendReq;  // 无 BBN：恒 SENDREQ
                break;
            case kCanNest: break;  // 不到达
            case kSendReq:
                if (tx_req.valid && tx_req_rdy)
                    n.state = reg.task.doDMT ? kRespCmt : kWaitData0;
                break;
            case kWaitData0:
                if (w_rec_data_hit) {
                    const bool half = reg.task.chi.dataVec == 1 || reg.task.chi.dataVec == 2;
                    n.state = half ? kRespCmt : kWaitData1;
                }
                break;
            case kWaitData1:
                if (w_rec_data_hit) n.state = kRespCmt;
                break;
            case kCantNest: break;  // 不到达
            case kSendAck: break;   // 不到达
            case kRespCmt:
                if (resp.valid && resp_rdy) n.state = kFree;
                break;
            default: break;
        }
        return n;
    };
    w_set.assign().reads(reg, alloc, alloc_rdy) = [](auto src) {
        auto [reg, alloc, alloc_rdy] = src;
        return (alloc.valid && alloc_rdy) || reg.state != kFree;
    };
    reg.update().on(posedge(clk)).reads(reg, w_next, w_set) = [](auto src) {
        auto [reg, w_next, w_set] = src;
        return w_set ? w_next : reg;
    };
}

ReadCM::ReadCM() {
    alloc_arb.clk = clk;
    tx_req_arb.clk = clk;
    resp_arb.clk = clk;
    for (uint32_t i = 0; i < kEntries; ++i) {
        entries[i].clk = clk;
        entries[i].rx_dat = rx_dat;
        entries[i].tx_req_rdy.assign().reads(tx_req_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].resp_rdy.assign().reads(resp_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
    }
    alloc_arb.in = alloc;
    alloc_rdy = alloc_arb.in_rdy;
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
    combine(w_tx_req_in, entries,
            [](ReadEntry& e) -> wolvicmod::Out<Valid<HReqFlit>>& { return e.tx_req; });
    combine(w_resp_in, entries,
            [](ReadEntry& e) -> wolvicmod::Out<Valid<CMResp>>& { return e.resp; });
    tx_req_arb.in = w_tx_req_in;
    tx_req_arb.out_rdy = tx_req_rdy;
    tx_req = tx_req_arb.out;
    resp_arb.in = w_resp_in;
    resp_arb.out_rdy = resp_rdy;
    resp = resp_arb.out;
}

// ---------------- WriteEntry ----------------

WriteEntry::WriteEntry() {
    alloc_rdy.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        return reg.state == kFree;
    };
    w_dbid_hit.assign().reads(reg, rx_rsp) = [](auto src) {
        auto [reg, rx_rsp] = src;
        return reg.state != kFree && rx_rsp.valid && rx_rsp.bits.txn_id == reg.task.hnTxnID &&
               (rx_rsp.bits.opcode == kCompDBIDResp || rx_rsp.bits.opcode == kDBIDResp);
    };
    w_comp_hit.assign().reads(reg, rx_rsp) = [](auto src) {
        auto [reg, rx_rsp] = src;
        return reg.state != kFree && rx_rsp.valid && rx_rsp.bits.txn_id == reg.task.hnTxnID &&
               (rx_rsp.bits.opcode == kCompDBIDResp || rx_rsp.bits.opcode == kComp);
    };
    w_data_resp_hit.assign().reads(reg, data_resp) = [](auto src) {
        auto [reg, data_resp] = src;
        return reg.state != kFree && data_resp.valid && data_resp.bits == reg.task.hnTxnID;
    };

    tx_req.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        HReqFlit f{};
        f.mem_attr = reg.task.chi.memAttr;
        f.order = 0;
        f.size = reg.task.chi.size;
        f.opcode = reg.task.chi.opcode;
        f.txn_id = reg.task.hnTxnID;
        f.src_id = reg.task.chi.getNoC();
        f.qos = reg.task.qos;
        return Valid<HReqFlit>{reg.state == kSendReq, f};
    };
    data_task.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        DataTask t{};
        t.dataOp = reg.task.dataOp;
        t.hnTxnID = reg.task.hnTxnID;
        t.ds = reg.task.ds;
        t.dataVec = reg.task.chi.dataVec;
        t.qos = reg.task.qos;
        t.txDat.resp = reg.task.cbResp;
        t.txDat.opcode = reg.task.chi.isImmediateWrite() ? kNonCopyBackWriteData
                                                         : kCopyBackWriteData;
        t.txDat.txn_id = reg.task.chi.txnID;
        t.txDat.src_id = reg.task.chi.getNoC();
        t.txDat.tgt_id = reg.task.chi.nodeId;
        return Valid<DataTask>{reg.state == kDataTask, t};
    };
    resp.assign().reads(reg) = [](auto src) {
        auto [reg] = src;
        CMResp r{};
        r.hnTxnID = reg.task.hnTxnID;
        r.toRepl = reg.task.fromRepl;
        r.taskInst = (1u << 18);
        r.qos = reg.task.qos;
        r.respErr = reg.respErr;
        return Valid<CMResp>{reg.state == kRespCmt && reg.alrGetComp, r};
    };

    w_next.assign().reads(reg, alloc, alloc_rdy, tx_req, tx_req_rdy, w_dbid_hit, w_comp_hit,
                          rx_rsp, data_task, data_task_rdy, w_data_resp_hit, resp, resp_rdy) =
        [](auto src) {
            auto [reg, alloc, alloc_rdy, tx_req, tx_req_rdy, w_dbid_hit, w_comp_hit, rx_rsp,
                  data_task, data_task_rdy, w_data_resp_hit, resp, resp_rdy] = src;
            WrReg n = reg;
            if (alloc.valid && alloc_rdy) {
                n.task = alloc.bits;
                n.alrGetComp = false;
                n.respErr = kErrOk;
            } else if (w_dbid_hit) {
                n.task.chi.txnID = rx_rsp.bits.dbid;
                n.task.chi.nodeId = rx_rsp.bits.src_id;
                n.alrGetComp = (rx_rsp.bits.opcode == kCompDBIDResp) || reg.alrGetComp;
                if (reg.respErr != kErrDerr && reg.respErr != kErrNderr)
                    n.respErr = rx_rsp.bits.resp_err;
            } else if (w_comp_hit) {
                n.alrGetComp = true;
                if (reg.respErr != kErrDerr && reg.respErr != kErrNderr)
                    n.respErr = rx_rsp.bits.resp_err;
            }
            switch (reg.state) {
                case kFree:
                    if (alloc.valid && alloc_rdy) n.state = kSendReq;
                    break;
                case kCanNest: break;
                case kSendReq:
                    if (tx_req.valid && tx_req_rdy) n.state = kWaitDbid;
                    break;
                case kWaitDbid:
                    if (w_dbid_hit) n.state = kDataTask;
                    break;
                case kDataTask:
                    if (data_task.valid && data_task_rdy) n.state = kWaitData;
                    break;
                case kWaitData:
                    if (w_data_resp_hit) n.state = kRespCmt;
                    break;
                case kCantNest: break;
                case kRespCmt:
                    if (resp.valid && resp_rdy) n.state = kFree;
                    break;
                default: break;
            }
            return n;
        };
    w_set.assign().reads(reg, alloc, alloc_rdy) = [](auto src) {
        auto [reg, alloc, alloc_rdy] = src;
        return (alloc.valid && alloc_rdy) || reg.state != kFree;
    };
    reg.update().on(posedge(clk)).reads(reg, w_next, w_set) = [](auto src) {
        auto [reg, w_next, w_set] = src;
        return w_set ? w_next : reg;
    };
}

WriteCM::WriteCM() {
    alloc_arb.clk = clk;
    tx_req_arb.clk = clk;
    resp_arb.clk = clk;
    data_task_arb.clk = clk;
    for (uint32_t i = 0; i < kEntries; ++i) {
        entries[i].clk = clk;
        entries[i].rx_rsp = rx_rsp;
        entries[i].data_resp = data_resp;
        entries[i].tx_req_rdy.assign().reads(tx_req_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].resp_rdy.assign().reads(resp_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
        entries[i].data_task_rdy.assign().reads(data_task_arb.in_rdy) = [i](auto src) {
            auto [rdy] = src;
            return rdy[i];
        };
    }
    alloc_arb.in = alloc;
    alloc_rdy = alloc_arb.in_rdy;
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
    combine(w_tx_req_in, entries,
            [](WriteEntry& e) -> wolvicmod::Out<Valid<HReqFlit>>& { return e.tx_req; });
    combine(w_resp_in, entries,
            [](WriteEntry& e) -> wolvicmod::Out<Valid<CMResp>>& { return e.resp; });
    combine(w_data_task_in, entries,
            [](WriteEntry& e) -> wolvicmod::Out<Valid<DataTask>>& { return e.data_task; });
    tx_req_arb.in = w_tx_req_in;
    tx_req_arb.out_rdy = tx_req_rdy;
    tx_req = tx_req_arb.out;
    resp_arb.in = w_resp_in;
    resp_arb.out_rdy = resp_rdy;
    resp = resp_arb.out;
    data_task_arb.in = w_data_task_in;
    data_task_arb.out_rdy = data_task_rdy;
    data_task = data_task_arb.out;
}

}  // namespace zj::dj
