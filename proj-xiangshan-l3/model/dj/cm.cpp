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

// ---------------- SnoopCM ----------------
// 拍平：32 个 SnoopEntry 实例并入 REG(entries) 一条 update 循环；tx_snp/resp
// 的 per-entry 组合输出数组化直喂 QosRR，per-entry rdy 直读仲裁器 in_rdy。

SnoopCM::SnoopCM() {
    alloc_arb.clk = clk;
    tx_snp_arb.clk = clk;
    resp_arb.clk = clk;

    // alloc 分配器：out_rdy = 各项空闲位
    w_alloc_rdy_all.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        RdyArrN r{};
        for (uint32_t i = 0; i < kEntries; ++i) r[i] = entries[i].state == kFree;
        return r;
    };
    alloc_arb.in = alloc;
    alloc_rdy = alloc_arb.in_rdy;
    alloc_arb.out_rdy = w_alloc_rdy_all;

    // per-entry 出站组合输出（数组化）
    w_tx_snp_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        TxSnpInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const EntryV& e = entries[i];
            SnoopFlit f{};
            const uint8_t pend = e.task.snpVec ^ (e.alrSend ? 1 : 0);  // 1bit
            const bool snpIsLast = pend == 1;  // PopCount(pend)===1（pend 至多 1）
            f.ret_to_src = snpIsLast && e.task.chi.retToSrc;
            f.do_not_go_to_sd = true;
            f.opcode = e.state == kPreSnp ? kSnpMakeInvalid : e.task.chi.opcode;
            f.fwd_txn_id = e.task.chi.txnID;
            f.fwd_nid = e.task.chi.nodeId;
            f.txn_id = e.task.hnTxnID;
            f.src_id = 0;    // LAN
            f.tgt_id = 0x09; // setSnpNodeId（本配置单 CC：0x08|1）
            f.qos = e.task.qos;
            o[i] = Valid<SnoopFlit>{e.state == kPreSnp || e.state == kSendSnp, f};
        }
        return o;
    };
    w_resp_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        RespInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const EntryV& e = entries[i];
            CMResp r;
            r.hnTxnID = e.task.hnTxnID;
            r.taskInst = e.taskInst;
            r.toRepl = e.task.fromRepl;
            r.qos = e.task.qos;
            r.respErr = e.respErr;
            o[i] = Valid<CMResp>{e.state == kRespCmt, r};
        }
        return o;
    };
    tx_snp_arb.in = w_tx_snp_in;
    tx_snp_arb.out_rdy = tx_snp_rdy;
    tx_snp = tx_snp_arb.out;
    resp_arb.in = w_resp_in;
    resp_arb.out_rdy = resp_rdy;
    resp = resp_arb.out;

    // N 项状态（一条 update 循环算 next；一切判定读旧值，派生量先算再写 next）
    entries.update().on(posedge(clk)).reads(entries, alloc_arb.out, tx_snp_arb.in_rdy,
                                            resp_arb.in_rdy, rx_rsp, rx_dat) = [](auto src) {
        auto [entries, alloc_out, tx_snp_rdy, resp_rdy, rx_rsp, rx_dat] = src;
        EntryArr n = entries;
        for (uint32_t i = 0; i < kEntries; ++i) {
            const EntryV& cur = entries[i];
            EntryV& ne = n[i];
            const bool allocFire = alloc_out[i].valid && cur.state == kFree;
            if (!allocFire && cur.state == kFree) continue;  // w_set 门控
            const bool rspHit = cur.state != kFree && rx_rsp.valid &&
                                cur.task.hnTxnID == rx_rsp.bits.txn_id &&
                                (rx_rsp.bits.opcode == kSnpResp ||
                                 rx_rsp.bits.opcode == kSnpRespFwded);
            const bool datHit = cur.state != kFree && rx_dat.valid &&
                                cur.task.hnTxnID == rx_dat.bits.txn_id &&
                                (rx_dat.bits.opcode == kSnpRespData ||
                                 rx_dat.bits.opcode == kSnpRespDataFwded);
            const bool snpFire = (cur.state == kPreSnp || cur.state == kSendSnp) && tx_snp_rdy[i];
            const bool rspIsFwd = rspHit && rx_rsp.bits.opcode == kSnpRespFwded;
            const bool datIsFwd = datHit && rx_dat.bits.opcode == kSnpRespDataFwded;

            if (allocFire) {
                ne.task = alloc_out[i].bits;
                ne.taskInst = 0;
                ne.respErr = kErrOk;
            } else if (rspHit || datHit) {
                uint32_t ti = cur.taskInst | (1u << 18);
                // fwdValid
                const bool fwdV = dc::tiFwdValid(cur.taskInst) || rspIsFwd || datIsFwd;
                ti = (ti & ~(1u << 17)) | (static_cast<uint32_t>(fwdV) << 17);
                // fwdResp
                const uint32_t fwdR = rspIsFwd ? rx_rsp.bits.fwd_state
                                      : datIsFwd ? rx_dat.bits.data_source
                                                 : dc::tiFwdResp(cur.taskInst);
                ti = (ti & ~(7u << 4)) | ((fwdR & 7u) << 4);
                // channel：datHit→DAT；rspHit→保持 DAT 或 RSP
                const uint32_t ch = datHit ? kChDat
                                    : rspHit ? (dc::tiChannel(cur.taskInst) == kChDat ? kChDat
                                                                                      : kChRsp)
                                             : dc::tiChannel(cur.taskInst);
                ti = (ti & ~(3u << 15)) | ((ch & 3u) << 15);
                // opcode
                const uint32_t op =
                    datHit ? rx_dat.bits.opcode
                    : rspHit
                        ? ((dc::tiChannel(cur.taskInst) == kChDat || fwdV)
                               ? dc::tiOpcode(cur.taskInst)
                               : rx_rsp.bits.opcode)
                        : dc::tiOpcode(cur.taskInst);
                ti = (ti & ~(0x1Fu << 10)) | ((op & 0x1Fu) << 10);
                // resp（不劣化：resp 较大者保持）
                const uint32_t rp =
                    datHit ? rx_dat.bits.resp
                    : rspHit
                        ? ((dc::tiChannel(cur.taskInst) == kChDat ||
                            dc::tiResp(cur.taskInst) >= rx_rsp.bits.resp)
                               ? dc::tiResp(cur.taskInst)
                               : rx_rsp.bits.resp)
                        : dc::tiResp(cur.taskInst);
                ti = (ti & ~(7u << 7)) | ((rp & 7u) << 7);
                ne.taskInst = ti;
                // respErr（首个非 OK）
                if (cur.respErr != kErrDerr && cur.respErr != kErrNderr) {
                    ne.respErr = datHit ? rx_dat.bits.resp_err : rx_rsp.bits.resp_err;
                }
            }

            if (allocFire) {
                ne.alrSend = false;
                ne.getResp = false;
                ne.getData = 0;
            } else {
                ne.alrSend = cur.alrSend || snpFire;
                const uint8_t beatId = rx_dat.valid && rx_dat.bits.data_id == 2 ? 1 : 0;
                ne.getData = cur.getData | (datHit ? static_cast<uint8_t>(1u << beatId) : 0);
                ne.getResp = cur.getResp || rspHit || datHit;  // metaId 恒 0
            }

            // 状态机
            const uint8_t pend = cur.task.snpVec ^ (cur.alrSend ? 1 : 0);
            const bool alrSnpAll = pend == 1 && snpFire;             // 最后一发
            const bool snpLastNext = pend == 0 && snpFire && false;  // PRESNP 专用（见下）
            const bool alrGetRspAll = (cur.task.snpVec ^ (ne.getResp ? 1 : 0)) == 0;
            const bool dataOne = (ne.getData == 1) || (ne.getData == 2);
            const bool alrGetAll = alrGetRspAll && !dataOne;
            switch (cur.state) {
                case kFree:
                    if (allocFire) {
                        const bool snpUniqueFwdMoreThan1 =
                            alloc_out[i].bits.chi.opcode == kSnpUniqueFwd &&
                            __builtin_popcount(alloc_out[i].bits.snpVec) > 1;
                        ne.state = snpUniqueFwdMoreThan1 ? kPreSnp : kSendSnp;
                    }
                    break;
                case kPreSnp:
                    // 倒数第二发（pend==1... 待发 2 个时 fires 后转 SENDSNP）：
                    // 本配置 snpVec 单位，不会到达（保留结构）
                    if (snpLastNext) ne.state = kSendSnp;
                    break;
                case kSendSnp:
                    if (alrSnpAll) ne.state = kWaitResp;
                    break;
                case kWaitResp:
                    if (alrGetAll) ne.state = kRespCmt;
                    break;
                case kRespCmt:
                    if (resp_rdy[i]) ne.state = kFree;
                    break;
                default: break;
            }
        }
        return n;
    };
}

// ---------------- ReadCM ----------------

ReadCM::ReadCM() {
    alloc_arb.clk = clk;
    tx_req_arb.clk = clk;
    resp_arb.clk = clk;

    w_alloc_rdy_all.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        RdyArrN r{};
        for (uint32_t i = 0; i < kEntries; ++i) r[i] = entries[i].state == kFree;
        return r;
    };
    alloc_arb.in = alloc;
    alloc_rdy = alloc_arb.in_rdy;
    alloc_arb.out_rdy = w_alloc_rdy_all;

    w_tx_req_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        TxReqInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const EntryV& e = entries[i];
            HReqFlit f{};
            f.exp_comp_ack = e.task.chi.expCompAck;
            f.mem_attr = e.task.chi.memAttr;
            f.order = 0;
            f.size = e.task.chi.size;
            f.opcode = e.task.chi.opcode;
            f.return_txn_id = e.task.doDMT ? e.task.chi.txnID : e.task.hnTxnID;
            f.return_nid = e.task.doDMT ? e.task.chi.nodeId : 0x7FF;
            f.txn_id = e.task.hnTxnID;
            f.src_id = e.task.chi.getNoC();
            f.qos = e.task.qos;
            o[i] = Valid<HReqFlit>{e.state == kSendReq, f};
        }
        return o;
    };
    w_resp_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        RespInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const EntryV& e = entries[i];
            CMResp r{};
            r.hnTxnID = e.task.hnTxnID;
            r.toRepl = false;
            r.qos = e.task.qos;
            if (!e.task.doDMT) {
                uint32_t ti = 0;
                ti |= (1u << 18);            // valid
                ti |= (kChDat << 15);        // channel = DAT
                ti |= (static_cast<uint32_t>(kCompData) << 10);
                ti |= (static_cast<uint32_t>(dc::tiResp(e.taskInst)) << 7);
                ti |= (static_cast<uint32_t>(kRespI) << 4);  // fwdResp = I
                r.taskInst = ti;
                r.respErr = e.respErr;
            } else {
                r.taskInst = (1u << 18);
                r.respErr = kErrOk;
            }
            o[i] = Valid<CMResp>{e.state == kRespCmt, r};
        }
        return o;
    };
    tx_req_arb.in = w_tx_req_in;
    tx_req_arb.out_rdy = tx_req_rdy;
    tx_req = tx_req_arb.out;
    resp_arb.in = w_resp_in;
    resp_arb.out_rdy = resp_rdy;
    resp = resp_arb.out;

    entries.update().on(posedge(clk)).reads(entries, alloc_arb.out, tx_req_arb.in_rdy,
                                            resp_arb.in_rdy, rx_dat) = [](auto src) {
        auto [entries, alloc_out, tx_req_rdy, resp_rdy, rx_dat] = src;
        EntryArr n = entries;
        for (uint32_t i = 0; i < kEntries; ++i) {
            const EntryV& cur = entries[i];
            EntryV& ne = n[i];
            const bool allocFire = alloc_out[i].valid && cur.state == kFree;
            if (!allocFire && cur.state == kFree) continue;  // w_set 门控
            const bool datHit = cur.state != kFree && rx_dat.valid &&
                                cur.task.hnTxnID == rx_dat.bits.txn_id &&
                                rx_dat.bits.opcode == kCompData;
            if (allocFire) {
                ne.task = alloc_out[i].bits;
                ne.taskInst = 0;
                ne.respErr = kErrOk;
            } else if (datHit) {
                ne.task.chi.nodeId = rx_dat.bits.home_nid;
                ne.task.chi.txnID = rx_dat.bits.dbid;
                ne.taskInst = (cur.taskInst & ~(7u << 7)) |
                              ((static_cast<uint32_t>(rx_dat.bits.resp) & 7u) << 7);
                if (cur.respErr != kErrDerr && cur.respErr != kErrNderr)
                    ne.respErr = rx_dat.bits.resp_err;
            }
            switch (cur.state) {
                case kFree:
                    if (allocFire) ne.state = kSendReq;  // 无 BBN：恒 SENDREQ
                    break;
                case kCanNest: break;  // 不到达
                case kSendReq:
                    if (tx_req_rdy[i]) ne.state = cur.task.doDMT ? kRespCmt : kWaitData0;
                    break;
                case kWaitData0:
                    if (datHit) {
                        const bool half =
                            cur.task.chi.dataVec == 1 || cur.task.chi.dataVec == 2;
                        ne.state = half ? kRespCmt : kWaitData1;
                    }
                    break;
                case kWaitData1:
                    if (datHit) ne.state = kRespCmt;
                    break;
                case kCantNest: break;  // 不到达
                case kSendAck: break;   // 不到达
                case kRespCmt:
                    if (resp_rdy[i]) ne.state = kFree;
                    break;
                default: break;
            }
        }
        return n;
    };
}

// ---------------- WriteCM ----------------

WriteCM::WriteCM() {
    alloc_arb.clk = clk;
    tx_req_arb.clk = clk;
    resp_arb.clk = clk;
    data_task_arb.clk = clk;

    w_alloc_rdy_all.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        RdyArrN r{};
        for (uint32_t i = 0; i < kEntries; ++i) r[i] = entries[i].state == kFree;
        return r;
    };
    alloc_arb.in = alloc;
    alloc_rdy = alloc_arb.in_rdy;
    alloc_arb.out_rdy = w_alloc_rdy_all;

    w_tx_req_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        TxReqInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const EntryV& e = entries[i];
            HReqFlit f{};
            f.mem_attr = e.task.chi.memAttr;
            f.order = 0;
            f.size = e.task.chi.size;
            f.opcode = e.task.chi.opcode;
            f.txn_id = e.task.hnTxnID;
            f.src_id = e.task.chi.getNoC();
            f.qos = e.task.qos;
            o[i] = Valid<HReqFlit>{e.state == kSendReq, f};
        }
        return o;
    };
    w_data_task_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        DataTaskInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const EntryV& e = entries[i];
            DataTask t{};
            t.dataOp = e.task.dataOp;
            t.hnTxnID = e.task.hnTxnID;
            t.ds = e.task.ds;
            t.dataVec = e.task.chi.dataVec;
            t.qos = e.task.qos;
            t.txDat.resp = e.task.cbResp;
            t.txDat.opcode = e.task.chi.isImmediateWrite() ? kNonCopyBackWriteData
                                                           : kCopyBackWriteData;
            t.txDat.txn_id = e.task.chi.txnID;
            t.txDat.src_id = e.task.chi.getNoC();
            t.txDat.tgt_id = e.task.chi.nodeId;
            o[i] = Valid<DataTask>{e.state == kDataTask, t};
        }
        return o;
    };
    w_resp_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        RespInArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            const EntryV& e = entries[i];
            CMResp r{};
            r.hnTxnID = e.task.hnTxnID;
            r.toRepl = e.task.fromRepl;
            r.taskInst = (1u << 18);
            r.qos = e.task.qos;
            r.respErr = e.respErr;
            o[i] = Valid<CMResp>{e.state == kRespCmt && e.alrGetComp, r};
        }
        return o;
    };
    tx_req_arb.in = w_tx_req_in;
    tx_req_arb.out_rdy = tx_req_rdy;
    tx_req = tx_req_arb.out;
    resp_arb.in = w_resp_in;
    resp_arb.out_rdy = resp_rdy;
    resp = resp_arb.out;
    data_task_arb.in = w_data_task_in;
    data_task_arb.out_rdy = data_task_rdy;
    data_task = data_task_arb.out;

    entries.update().on(posedge(clk)).reads(entries, alloc_arb.out, tx_req_arb.in_rdy,
                                            resp_arb.in_rdy, data_task_arb.in_rdy, rx_rsp,
                                            data_resp) = [](auto src) {
        auto [entries, alloc_out, tx_req_rdy, resp_rdy, data_task_rdy, rx_rsp, data_resp] = src;
        EntryArr n = entries;
        for (uint32_t i = 0; i < kEntries; ++i) {
            const EntryV& cur = entries[i];
            EntryV& ne = n[i];
            const bool allocFire = alloc_out[i].valid && cur.state == kFree;
            if (!allocFire && cur.state == kFree) continue;  // w_set 门控
            const bool dbidHit = cur.state != kFree && rx_rsp.valid &&
                                 rx_rsp.bits.txn_id == cur.task.hnTxnID &&
                                 (rx_rsp.bits.opcode == kCompDBIDResp ||
                                  rx_rsp.bits.opcode == kDBIDResp);
            const bool compHit = cur.state != kFree && rx_rsp.valid &&
                                 rx_rsp.bits.txn_id == cur.task.hnTxnID &&
                                 (rx_rsp.bits.opcode == kCompDBIDResp ||
                                  rx_rsp.bits.opcode == kComp);
            const bool dataRespHit = cur.state != kFree && data_resp.valid &&
                                     data_resp.bits == cur.task.hnTxnID;
            if (allocFire) {
                ne.task = alloc_out[i].bits;
                ne.alrGetComp = false;
                ne.respErr = kErrOk;
            } else if (dbidHit) {
                ne.task.chi.txnID = rx_rsp.bits.dbid;
                ne.task.chi.nodeId = rx_rsp.bits.src_id;
                ne.alrGetComp = (rx_rsp.bits.opcode == kCompDBIDResp) || cur.alrGetComp;
                if (cur.respErr != kErrDerr && cur.respErr != kErrNderr)
                    ne.respErr = rx_rsp.bits.resp_err;
            } else if (compHit) {
                ne.alrGetComp = true;
                if (cur.respErr != kErrDerr && cur.respErr != kErrNderr)
                    ne.respErr = rx_rsp.bits.resp_err;
            }
            switch (cur.state) {
                case kFree:
                    if (allocFire) ne.state = kSendReq;
                    break;
                case kCanNest: break;
                case kSendReq:
                    if (tx_req_rdy[i]) ne.state = kWaitDbid;
                    break;
                case kWaitDbid:
                    if (dbidHit) ne.state = kDataTask;
                    break;
                case kDataTask:
                    if (data_task_rdy[i]) ne.state = kWaitData;
                    break;
                case kWaitData:
                    if (dataRespHit) ne.state = kRespCmt;
                    break;
                case kCantNest: break;
                case kRespCmt:
                    if (cur.alrGetComp && resp_rdy[i]) ne.state = kFree;
                    break;
                default: break;
            }
        }
        return n;
    };
}

}  // namespace zj::dj
