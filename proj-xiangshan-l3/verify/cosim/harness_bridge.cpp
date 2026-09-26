// P4a 桥对拍 harness：SNodeAxiBridge vs AxiBridge.sv、HiNodeAxiLiteBridge vs
// AxiLiteBridge.sv（免 refgen，直接用 XiangShan emu 构建产物 build/rtl）。
//
// 激励（两侧严格同一序列；协议合法但时序/参数自由）：
//   请求器：S 侧 ReadNoSnp/WriteNoSnp{Ptl,Full,FullCleanInv}（size<=6）、
//     HI 侧 ReadNoSnp/WriteNoSnpPtl（size<=3）；小地址池制造同 tag 冲突；
//     order/mem_attr/qos/ExpCompAck/DoDWT/ReturnID 随机；valid 保持到 fire。
//   写数据引擎：DBIDResp/CompDBIDResp 观察后按 size 发 NCBWrData（DataID =
//     offset+2k，dw=256）或 WriteDataCancel（~8%）；HI 单拍数据；
//     HI 且 ExpCompAck 的事务：写见 DBIDResp/CompDBIDResp 后、读见 CompData
//     后回 CompAck（TxnID=CM idx=DBID）。
//   AXI 从机：aw/ar 随机 rdy 接受；B 在对应 W 突发（awQueue 序）收齐 + 随机
//     延迟后回；R 按 len+1 拍随机间隔回（数据随机，两侧同一数据）。
//   反压：tx_resp/tx_data/aw/w/ar 的 rdy 按密度分段随机翻转。
// 比对：每拍比对边界全部 valid/rdy/bits（bits 仅在 valid=1 时；HI 侧被
//   firtool 裁掉的常量端口不比）。

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <random>
#include <string>

#include "VAxiBridge.h"
#include "VAxiLiteBridge.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include <model/bridge/hinode_axilite_bridge.h>
#include <model/bridge/snode_axi_bridge.h>

using namespace wolvicmod;
using namespace zj::bridge;
using namespace zj::chi;
using namespace zj::axi;

namespace {

// ---------------- 宽信号助手（256b ↔ VlWide<8>） ----------------

template <class W>
void setWide(W& w, const std::array<uint64_t, 4>& d) {
    for (int i = 0; i < 4; ++i) {
        w[2 * i]     = uint32_t(d[i]);
        w[2 * i + 1] = uint32_t(d[i] >> 32);
    }
}

template <class W>
uint64_t wideW(const W& w, int i) {
    return uint64_t(uint32_t(w[2 * i])) | (uint64_t(uint32_t(w[2 * i + 1])) << 32);
}

inline uint32_t beatCnt(uint8_t size) { return size > 5 ? (1u << (size - 5)) : 1; }

// ---------------- 事务跟踪（协议合法性保证） ----------------

struct TxnTrack {
    bool    active     = false;
    bool    isRead     = false;
    bool    eca        = false;
    bool    snpAttr    = false;  // DoDWT（S 写）
    uint8_t size       = 0;
    uint8_t dataIdOff  = 0;      // addr(5)<<1（S）
    uint64_t addr      = 0;
    uint16_t txnId     = 0;      // 活跃期内唯一
    uint16_t returnTxn = 0;      // 活跃期内唯一（S 读完成匹配用）
    int     cmIdx      = -1;     // DBIDResp 给出（写数据 TxnID）
    bool    decided    = false;  // 数据/取消已抉择
    bool    cancel     = false;
    uint32_t beatsSent = 0;
    bool    compSeen   = false;  // Comp/CompDBIDResp 已 fire
    bool    compAckSent = false;  // HI
};

bool txnDone(const TxnTrack& t) {
    if (t.isRead) return false;  // 读由 CompData 末拍直接回收
    // compSeen + 数据/取消 flit 全部 fire 才算完成（取消以 fire 时补齐 beats 计）
    return t.compSeen && t.beatsSent >= beatCnt(t.size);
}

// AXI 从机条目
struct AwPend {
    uint8_t  id     = 0;
    uint8_t  len    = 0;
    uint32_t wBeats = 0;
    bool     wDone  = false;
    int      bDelay = 0;
};

struct ArPend {
    uint8_t                 id    = 0;
    uint32_t                total = 0;  // len+1
    uint32_t                sent  = 0;
    int                     gap   = 0;
    std::array<uint64_t, 4> data[8];
};

// =================================================================
// S 桥
// =================================================================

uint64_t cosimSBridge(uint32_t seed, uint64_t cycles) {
    VAxiBridge ref;
    SNodeAxiBridge dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;

    auto zeroInputs = [&] {
        ref.icn_rx_req_valid = 0;
        ref.icn_rx_data_valid = 0;
        ref.icn_tx_resp_ready = 0;
        ref.icn_tx_data_ready = 0;
        ref.axi_aw_ready = 0;
        ref.axi_ar_ready = 0;
        ref.axi_w_ready = 0;
        ref.axi_b_valid = 0;
        ref.axi_r_valid = 0;
    };
    cosim::resetRef(ref, zeroInputs);
    dut.rx_req.set(Dec<HReqFlit>{});
    dut.rx_data.set(Dec<DataFlit>{});
    dut.tx_resp_rdy.set(false);
    dut.tx_data_rdy.set(false);
    dut.axi_aw_rdy.set(false);
    dut.axi_ar_rdy.set(false);
    dut.axi_w_rdy.set(false);
    dut.axi_b.set(Dec<BFlit>{});
    dut.axi_r.set(Dec<RFlit>{});

    // 地址池：4 条 64B 行 + 行内偏移（同块 tag 冲突）
    const uint64_t lineBase[4] = {0x8000'0000, 0x8000'0040, 0x8001'0000, 0x8001'0040};
    const uint64_t lineOffs[4] = {0, 8, 16, 24};

    std::array<TxnTrack, 64> txns{};
    std::array<uint64_t, 64> cmValidCyc{};
    uint32_t txnCursor = 0;
    bool     reqPend   = false;
    HReqFlit reqBits;
    TxnTrack* reqTxn   = nullptr;
    // 写数据/取消挂起（valid 保持到 fire）
    bool     datPend   = false;
    DataFlit datBits;
    TxnTrack* datTxn   = nullptr;
    // AXI 从机
    std::deque<AwPend> awQ;
    std::deque<ArPend> arQ;
    bool  rPend = false;
    RFlit rBits;
    // 事件计数（诊断用）
    uint64_t cReq = 0, cRsp = 0, cDat = 0, cAw = 0, cW = 0, cAr = 0, cDatIn = 0;
    uint64_t cArRdy = 0, cRspRdy = 0;
    const bool trace = getenv("BRIDGE_TRACE") != nullptr;

    auto idInUse = [&](uint16_t v) {
        for (const auto& t : txns)
            if (t.active && (t.txnId == v || t.returnTxn == v)) return true;
        return false;
    };

    auto newRequest = [&](uint64_t cur) -> bool {
        for (uint32_t k = 0; k < 64; ++k) {
            const uint32_t i = (txnCursor + k) % 64;
            if (txns[i].active) continue;
            txnCursor           = i + 1;
            TxnTrack& t         = txns[i];
            t                   = TxnTrack{};
            t.active            = true;
            t.isRead            = (rng() % 100) < 50;
            t.eca               = (rng() % 100) < 30;
            t.size              = uint8_t(rng() % 7);
            t.addr              = lineBase[rng() % 4] + lineOffs[rng() % 4];
            t.dataIdOff         = uint8_t(((t.addr >> 5) & 1) << 1);
            uint16_t v          = uint16_t(rng() % 0x1000);
            while (idInUse(v)) v = uint16_t(rng() % 0x1000);
            t.txnId             = v;
            uint16_t r2         = uint16_t(rng() % 0x1000);
            while (idInUse(r2)) r2 = uint16_t(rng() % 0x1000);
            t.returnTxn         = r2;
            t.snpAttr           = (rng() % 100) < 30;
            reqBits             = HReqFlit{};
            reqBits.opcode      = t.isRead ? req_op::kReadNoSnp
                                           : (rng() % 100) < 50 ? req_op::kWriteNoSnpPtl
                                                                : req_op::kWriteNoSnpFull;
            // （不注入 WriteNoSnpFullCleanInv：它触发上游 opvec 的 RTL 断言
            //   ——bridge/package.scala:38 的 legalCode 不含该码，行为与
            //   WriteNoSnpFull 完全相同，只徒增断言噪音）
            reqBits.addr        = t.addr;
            reqBits.size        = t.size;
            reqBits.txn_id      = t.txnId;
            reqBits.src_id      = 0x10;
            reqBits.tgt_id      = 0x30;
            reqBits.return_nid  = uint16_t(rng() % 0x800);
            reqBits.return_txn_id = t.returnTxn;
            reqBits.order       = uint8_t(rng() % 4);
            reqBits.mem_attr    = uint8_t(rng() % 16);
            reqBits.snp_attr    = t.snpAttr;
            reqBits.exp_comp_ack = t.eca;
            reqBits.qos         = uint8_t(rng() % 16);
            reqPend             = true;
            reqTxn              = &t;
            if (trace)
                std::printf("  cyc=%llu SLOT-NEW [%u] txnId=0x%x read=%d\n",
                            (unsigned long long)cur, i, t.txnId, (int)t.isRead);
            return true;
        }
        return false;
    };

    uint64_t cyc     = 0;
    bool     drained = false;
    // 主循环 = 激励（cycles 拍）+ 排空（停止新请求、反压全撤，直至事务全清）
    for (; cyc < cycles + 100000 && !drained; ++cyc) {
        const bool     draining = cyc >= cycles;
        const uint32_t density  = cosim::densityAt(cyc, cycles);
        char buf[160];

        if (!draining && !reqPend && cosim::roll(rng, density / 2)) newRequest(cyc);

        // ---- 写数据/取消抉择与组装（valid 保持到 fire）----
        if (!datPend) {
            for (auto& t : txns) {
                if (t.active && !t.isRead && t.cmIdx >= 0 && !t.decided) {
                    t.decided = true;
                    t.cancel  = (rng() % 100) < 8;
                }
            }
            // 取消优先，其次数据
            TxnTrack* sel = nullptr;
            bool      selCancel = false;
            for (auto& t : txns)
                if (t.active && !t.isRead && t.decided && t.cancel && t.beatsSent == 0) {
                    sel       = &t;
                    selCancel = true;
                    break;
                }
            if (sel == nullptr)
                for (auto& t : txns)
                    if (t.active && !t.isRead && t.decided && !t.cancel &&
                        t.beatsSent < beatCnt(t.size)) {
                        sel = &t;
                        break;
                    }
            if (sel != nullptr && cosim::roll(rng, density)) {
                datTxn        = sel;
                datBits       = DataFlit{};
                datBits.txn_id = uint16_t(sel->cmIdx);
                if (selCancel) {
                    datBits.opcode = dat_op::kWriteDataCancel;
                    // beats 在 fire 时补齐（见 fire 块）；此处保持 0，防止
                    // 未 fire 就被 txnDone 回收
                } else {
                    datBits.opcode  = (rng() % 100) < 10 ? dat_op::kNCBWrDataCompAck
                                                         : dat_op::kNonCopyBackWriteData;
                    datBits.data_id = uint8_t(sel->dataIdOff + 2 * sel->beatsSent);
                    for (auto& w : datBits.data) w = rng() | (uint64_t(rng()) << 32);
                    datBits.be      = sel->size == 6 ? 0xFFFFFFFF : rng();
                }
                datPend = true;
            }
        }

        // ---- 驱动 rx_req / rx_data ----
        if (reqPend) {
            ref.icn_rx_req_valid           = 1;
            ref.icn_rx_req_bits_ExpCompAck = reqBits.exp_comp_ack;
            ref.icn_rx_req_bits_Excl       = reqBits.excl;
            ref.icn_rx_req_bits_SnpAttr    = reqBits.snp_attr;
            ref.icn_rx_req_bits_MemAttr    = reqBits.mem_attr;
            ref.icn_rx_req_bits_Order      = reqBits.order;
            ref.icn_rx_req_bits_Addr       = reqBits.addr;
            ref.icn_rx_req_bits_Size       = reqBits.size;
            ref.icn_rx_req_bits_Opcode     = reqBits.opcode;
            ref.icn_rx_req_bits_ReturnTxnID = reqBits.return_txn_id;
            ref.icn_rx_req_bits_ReturnNID  = reqBits.return_nid;
            ref.icn_rx_req_bits_TxnID      = reqBits.txn_id;
            ref.icn_rx_req_bits_SrcID      = reqBits.src_id;
            ref.icn_rx_req_bits_TgtID      = reqBits.tgt_id;
            ref.icn_rx_req_bits_QoS        = reqBits.qos;
            Dec<HReqFlit> dq;
            dq.valid = true;
            dq.bits  = reqBits;
            dut.rx_req.set(dq);
        } else {
            ref.icn_rx_req_valid = 0;
            dut.rx_req.set(Dec<HReqFlit>{});
        }
        if (datPend) {
            ref.icn_rx_data_valid = 1;
            setWide(ref.icn_rx_data_bits_Data, datBits.data);
            ref.icn_rx_data_bits_BE         = datBits.be;
            ref.icn_rx_data_bits_DataID     = datBits.data_id;
            ref.icn_rx_data_bits_DBID       = datBits.dbid;
            ref.icn_rx_data_bits_CBusy      = datBits.c_busy;
            ref.icn_rx_data_bits_DataSource = datBits.data_source;
            ref.icn_rx_data_bits_Resp       = datBits.resp;
            ref.icn_rx_data_bits_RespErr    = datBits.resp_err;
            ref.icn_rx_data_bits_Opcode     = datBits.opcode;
            ref.icn_rx_data_bits_HomeNID    = datBits.home_nid;
            ref.icn_rx_data_bits_TxnID      = datBits.txn_id;
            ref.icn_rx_data_bits_SrcID      = datBits.src_id;
            ref.icn_rx_data_bits_TgtID      = datBits.tgt_id;
            ref.icn_rx_data_bits_QoS        = datBits.qos;
            Dec<DataFlit> dd;
            dd.valid = true;
            dd.bits  = datBits;
            dut.rx_data.set(dd);
        } else {
            ref.icn_rx_data_valid = 0;
            dut.rx_data.set(Dec<DataFlit>{});
        }

        // ---- rdy 反压（排空期全撤）----
        const bool rspRdy = draining || cosim::roll(rng, density);
        const bool datRdy = draining || cosim::roll(rng, density);
        const bool awRdy  = draining || cosim::roll(rng, density);
        const bool arRdy  = draining || cosim::roll(rng, density);
        const bool wRdy   = draining || cosim::roll(rng, density);
        if (arRdy) ++cArRdy;
        if (rspRdy) ++cRspRdy;
        if (trace && cyc % 10000 == 9999)
            std::printf("  [ckpt cyc=%llu] arRdy1=%llu rspRdy1=%llu | aw=%llu ar=%llu rsp=%llu dat=%llu\n",
                        (unsigned long long)cyc, cArRdy, cRspRdy, cAw, cAr, cRsp, cDat);
        ref.icn_tx_resp_ready = rspRdy;
        ref.icn_tx_data_ready = datRdy;
        ref.axi_aw_ready      = awRdy;
        ref.axi_ar_ready      = arRdy;
        ref.axi_w_ready       = wRdy;
        dut.tx_resp_rdy.set(rspRdy);
        dut.tx_data_rdy.set(datRdy);
        dut.axi_aw_rdy.set(awRdy);
        dut.axi_ar_rdy.set(arRdy);
        dut.axi_w_rdy.set(wRdy);

        // ---- AXI 从机：B ----
        for (auto& a : awQ)
            if (draining)
                a.bDelay = 0;
            else if (a.wDone && a.bDelay > 0)
                --a.bDelay;
        const bool bDrive = !awQ.empty() && awQ.front().wDone && awQ.front().bDelay == 0;
        ref.axi_b_valid   = bDrive;
        ref.axi_b_bits_id = bDrive ? awQ.front().id : 0;
        ref.axi_b_bits_resp = 0;
        {
            Dec<BFlit> db;
            db.valid      = bDrive;
            db.bits.id    = bDrive ? awQ.front().id : 0;
            db.bits.resp  = 0;
            dut.axi_b.set(db);
        }

        // ---- AXI 从机：R ----
        if (!rPend) {
            for (auto& a : arQ)
                if (draining)
                    a.gap = 0;
                else if (a.gap > 0)
                    --a.gap;
            for (auto& a : arQ) {
                if (a.sent < a.total && a.gap == 0) {
                    rPend      = true;
                    rBits      = RFlit{};
                    rBits.id   = a.id;
                    rBits.data = a.data[a.sent];
                    rBits.last = a.sent + 1 == a.total;
                    break;
                }
            }
        }
        ref.axi_r_valid = rPend;
        ref.axi_r_bits_id = rBits.id;
        setWide(ref.axi_r_bits_data, rBits.data);
        ref.axi_r_bits_resp = rBits.resp;
        ref.axi_r_bits_last = rBits.last;
        {
            Dec<RFlit> dr;
            dr.valid = rPend;
            dr.bits  = rBits;
            dut.axi_r.set(dr);
        }

        // ---- 拍低相：eval + 采样比对 ----
        cosim::phaseLow(ref, dut);
        rp.push([&] {
            std::snprintf(buf, sizeof buf,
                          "cyc=%llu reqV=%d datV=%d rspRdy=%d datRdy=%d awRdy=%d arRdy=%d "
                          "wRdy=%d bV=%d rV=%d",
                          (unsigned long long)cyc, (int)ref.icn_rx_req_valid,
                          (int)ref.icn_rx_data_valid, (int)rspRdy, (int)datRdy, (int)awRdy,
                          (int)arRdy, (int)wRdy, (int)bDrive, (int)rPend);
            return std::string(buf);
        }());
        auto cmp = [&](const char* port, uint64_t r, uint64_t d) {
            cosim::check(st, "sbridge", "s", seed, cyc, port, r, d, rp);
        };
        cmp("rx_req_rdy", ref.icn_rx_req_ready, dut.rx_req_rdy.get());
        cmp("rx_data_rdy", ref.icn_rx_data_ready, dut.rx_data_rdy.get());
        cmp("tx_resp_vld", ref.icn_tx_resp_valid, dut.tx_resp.get().valid);
        if (ref.icn_tx_resp_valid) {
            const auto& d = dut.tx_resp.get().bits;
            cmp("tx_resp_dbid", ref.icn_tx_resp_bits_DBID, d.dbid);
            cmp("tx_resp_op", ref.icn_tx_resp_bits_Opcode, d.opcode);
            cmp("tx_resp_txn", ref.icn_tx_resp_bits_TxnID, d.txn_id);
            cmp("tx_resp_src", ref.icn_tx_resp_bits_SrcID, d.src_id);
            cmp("tx_resp_tgt", ref.icn_tx_resp_bits_TgtID, d.tgt_id);
            cmp("tx_resp_qos", ref.icn_tx_resp_bits_QoS, d.qos);
            cmp("tx_resp_resp", ref.icn_tx_resp_bits_Resp, d.resp);
            cmp("tx_resp_rerr", ref.icn_tx_resp_bits_RespErr, d.resp_err);
            cmp("tx_resp_cbusy", ref.icn_tx_resp_bits_CBusy, d.c_busy);
            cmp("tx_resp_fwd", ref.icn_tx_resp_bits_FwdState, d.fwd_state);
        }
        cmp("tx_data_vld", ref.icn_tx_data_valid, dut.tx_data.get().valid);
        if (ref.icn_tx_data_valid) {
            const auto& d = dut.tx_data.get().bits;
            for (int i = 0; i < 4; ++i)
                cmp(cosim::lanePort("tx_data_data", i).c_str(),
                    wideW(ref.icn_tx_data_bits_Data, i), d.data[i]);
            cmp("tx_data_be", ref.icn_tx_data_bits_BE, d.be);
            cmp("tx_data_did", ref.icn_tx_data_bits_DataID, d.data_id);
            cmp("tx_data_dbid", ref.icn_tx_data_bits_DBID, d.dbid);
            cmp("tx_data_op", ref.icn_tx_data_bits_Opcode, d.opcode);
            cmp("tx_data_txn", ref.icn_tx_data_bits_TxnID, d.txn_id);
            cmp("tx_data_src", ref.icn_tx_data_bits_SrcID, d.src_id);
            cmp("tx_data_tgt", ref.icn_tx_data_bits_TgtID, d.tgt_id);
            cmp("tx_data_home", ref.icn_tx_data_bits_HomeNID, d.home_nid);
            cmp("tx_data_qos", ref.icn_tx_data_bits_QoS, d.qos);
            cmp("tx_data_resp", ref.icn_tx_data_bits_Resp, d.resp);
            cmp("tx_data_rerr", ref.icn_tx_data_bits_RespErr, d.resp_err);
            cmp("tx_data_ds", ref.icn_tx_data_bits_DataSource, d.data_source);
            cmp("tx_data_cbusy", ref.icn_tx_data_bits_CBusy, d.c_busy);
        }
        for (int ch = 0; ch < 2; ++ch) {
            const bool rv = ch == 0 ? ref.axi_aw_valid : ref.axi_ar_valid;
            const bool dv = ch == 0 ? dut.axi_aw.get().valid : dut.axi_ar.get().valid;
            cmp(ch == 0 ? "aw_vld" : "ar_vld", rv, dv);
            if (rv) {
                const auto& d = ch == 0 ? dut.axi_aw.get().bits : dut.axi_ar.get().bits;
                cmp("ax_id", ch == 0 ? ref.axi_aw_bits_id : ref.axi_ar_bits_id, d.id);
                cmp("ax_addr", ch == 0 ? ref.axi_aw_bits_addr : ref.axi_ar_bits_addr, d.addr);
                cmp("ax_len", ch == 0 ? ref.axi_aw_bits_len : ref.axi_ar_bits_len, d.len);
                cmp("ax_size", ch == 0 ? ref.axi_aw_bits_size : ref.axi_ar_bits_size, d.size);
                cmp("ax_burst", ch == 0 ? ref.axi_aw_bits_burst : ref.axi_ar_bits_burst,
                    d.burst);
                cmp("ax_lock", ch == 0 ? ref.axi_aw_bits_lock : ref.axi_ar_bits_lock, d.lock);
                cmp("ax_cache", ch == 0 ? ref.axi_aw_bits_cache : ref.axi_ar_bits_cache,
                    d.cache);
                cmp("ax_prot", ch == 0 ? ref.axi_aw_bits_prot : ref.axi_ar_bits_prot, d.prot);
                cmp("ax_qos", ch == 0 ? ref.axi_aw_bits_qos : ref.axi_ar_bits_qos, d.qos);
                cmp("ax_region", ch == 0 ? ref.axi_aw_bits_region : ref.axi_ar_bits_region,
                    d.region);
            }
        }
        cmp("w_vld", ref.axi_w_valid, dut.axi_w.get().valid);
        if (ref.axi_w_valid) {
            const auto& d = dut.axi_w.get().bits;
            for (int i = 0; i < 4; ++i)
                cmp(cosim::lanePort("w_data", i).c_str(), wideW(ref.axi_w_bits_data, i),
                    d.data[i]);
            cmp("w_strb", ref.axi_w_bits_strb, d.strb);
            cmp("w_last", ref.axi_w_bits_last, d.last);
        }
        cmp("b_rdy", ref.axi_b_ready, dut.axi_b_rdy.get());
        cmp("r_rdy", ref.axi_r_ready, dut.axi_r_rdy.get());

        // ---- 拍内采样值捕获（提交前；事后记账必须用拍内值，提交后的
        // ref.* 已是下一拍组合态）----
        const bool     s_reqRdy  = ref.icn_rx_req_ready;
        const bool     s_datInRdy = ref.icn_rx_data_ready;
        const bool     s_rspV    = ref.icn_tx_resp_valid;
        const uint8_t  s_rspOp   = ref.icn_tx_resp_bits_Opcode;
        const uint16_t s_rspTxn  = ref.icn_tx_resp_bits_TxnID;
        const uint16_t s_rspDbid = ref.icn_tx_resp_bits_DBID;
        const bool     s_datV    = ref.icn_tx_data_valid;
        const uint16_t s_datTxn  = ref.icn_tx_data_bits_TxnID;
        const uint8_t  s_datDid  = ref.icn_tx_data_bits_DataID;
        const bool     s_awV     = ref.axi_aw_valid;
        const uint8_t  s_awId    = ref.axi_aw_bits_id;
        const uint8_t  s_awLen   = ref.axi_aw_bits_len;
        const bool     s_arV     = ref.axi_ar_valid;
        const uint8_t  s_arId    = ref.axi_ar_bits_id;
        const uint8_t  s_arLen   = ref.axi_ar_bits_len;
        const bool     s_wV      = ref.axi_w_valid;
        const bool     s_rRdy    = ref.axi_r_ready;

        // ---- 拍高相：提交 ----
        cosim::phaseHigh(ref, dut);

        // 看门狗：任一 CM 连续 valid 超 5 万拍即打印内部态（镜像 RTL 的
        // "bridge CM time out" 调试断言；挂死回归时直接定位现场）
        for (uint32_t i = 0; i < 64; ++i) {
            const auto& s = dut.cms[i]->st.get();
            if (s.valid) {
                if (++cmValidCyc[i] == 50001) {
                    std::printf(
                        "cyc=%llu cm_%u 50k: wait=%d u(r=%d,d=%d,w=%d,rd=%d,c=%d) "
                        "d(wa=%d,ra=%d,wd=%d,wr=%d,rd=%d) bufAlloc=%d allocIss=%d "
                        "addr=0x%llx size=%d\n",
                        (unsigned long long)cyc, i, s.waiting, (int)s.u.receiptResp,
                        (int)s.u.dbidResp, (int)s.u.wdata, (int)s.u.rdata, (int)s.u.comp,
                        (int)s.d.waddr, (int)s.d.raddr, (int)s.d.wdata, (int)s.d.wresp,
                        (int)s.d.rdata, (int)s.buffer_allocated, (int)s.alloc_issued,
                        (unsigned long long)s.info.addr, s.info.size);
                }
            } else {
                cmValidCyc[i] = 0;
            }
        }

        // ---- 事后状态更新（按本拍 fire 结果，全部用拍内采样值）----
        if (reqPend && s_reqRdy) {
            if (trace && reqTxn)
                std::printf("  cyc=%llu REQ-FIRE txnId=0x%x read=%d size=%d addr=0x%llx\n",
                            (unsigned long long)cyc, reqTxn->txnId, (int)reqTxn->isRead,
                            reqTxn->size, (unsigned long long)reqTxn->addr);
            reqPend = false;
            ++cReq;
        }
        if (datPend && s_datInRdy) {
            if (trace)
                std::printf("  cyc=%llu DATIN-FIRE cmIdx=%d did=%d op=%d\n",
                            (unsigned long long)cyc, datTxn->cmIdx, datBits.data_id,
                            datBits.opcode);
            // 取消 flit 在 fire 时才算"数据侧完成"（勿在组装时提前记，否则
            // 提前回收会把取消丢弃在桥外）
            datTxn->beatsSent = datTxn->cancel ? beatCnt(datTxn->size)
                                               : datTxn->beatsSent + 1;
            datPend = false;
            ++cDatIn;
        }
        // DBIDResp / CompDBIDResp → 写 txn 记录 cmIdx；Comp 记 compSeen
        if (s_rspV && rspRdy) {
            if (trace)
                std::printf("  cyc=%llu RSP-FIRE op=%d txn=0x%x dbid=%d\n",
                            (unsigned long long)cyc, s_rspOp, s_rspTxn, s_rspDbid);
            for (auto& t : txns) {
                if (!t.active || t.isRead) continue;
                if (s_rspOp == rsp_op::kDBIDResp || s_rspOp == rsp_op::kCompDBIDResp) {
                    // TxnID 路由：icnDBID && dwt 时 returnTxn，否则 txnId
                    const uint16_t want = t.snpAttr ? t.returnTxn : t.txnId;
                    if (t.cmIdx < 0 && s_rspTxn == want) t.cmIdx = s_rspDbid;
                    if (s_rspOp == rsp_op::kCompDBIDResp && s_rspTxn == want)
                        t.compSeen = true;
                } else if (s_rspOp == rsp_op::kComp && s_rspTxn == t.txnId) {
                    // 纯 Comp 不触发 dwt 路由（Mux 条件 icnDBID && dwt）
                    t.compSeen = true;
                }
            }
        }
        // 读完成：CompData 末拍 fire（txn_id=returnTxn、data_id=末拍）
        if (s_datV && datRdy) {
            if (trace)
                std::printf("  cyc=%llu COMPDAT-FIRE txn=0x%x did=%d\n",
                            (unsigned long long)cyc, s_datTxn, s_datDid);
            for (auto& t : txns)
                if (t.active && t.isRead && s_datTxn == t.returnTxn &&
                    s_datDid == t.dataIdOff + 2 * (beatCnt(t.size) - 1))
                    t.active = false;
        }
        for (uint32_t si = 0; si < 64; ++si) {
            auto& t = txns[si];
            if (t.active && !t.isRead && txnDone(t)) {
                if (trace)
                    std::printf("  cyc=%llu SLOT-RET [%u] txnId=0x%x cmIdx=%d\n",
                                (unsigned long long)cyc, si, t.txnId, t.cmIdx);
                t.active = false;
            }
        }
        for (auto& t : txns)
            if (t.active && t.isRead) (void)t;
        if (s_rspV && rspRdy) ++cRsp;
        if (s_datV && datRdy) ++cDat;
        if (s_awV && awRdy) ++cAw;
        if (s_arV && arRdy) ++cAr;
        if (s_wV && wRdy) ++cW;
        if (reqPend && s_reqRdy) ++cReq;
        if (datPend && s_datInRdy) ++cDatIn;
        // AXI 从机推进
        if (s_awV && awRdy) {
            if (trace)
                std::printf("  cyc=%llu AW-FIRE id=%d\n", (unsigned long long)cyc, s_awId);
            AwPend a;
            a.id     = s_awId;
            a.len    = s_awLen;
            a.bDelay = rng() % 8;
            awQ.push_back(a);
        }
        if (s_wV && wRdy) {
            // W 按 awQueue 序落在最老的未完成突发上（不同 AW 的 W 不交织）
            for (auto& a : awQ) {
                if (!a.wDone) {
                    if (++a.wBeats > a.len) a.wDone = true;
                    break;
                }
            }
        }
        if (bDrive) {
            if (trace)
                std::printf("  cyc=%llu B-FIRE id=%d\n", (unsigned long long)cyc,
                            awQ.front().id);
            awQ.pop_front();
        }
        if (s_arV && arRdy) {
            if (trace)
                std::printf("  cyc=%llu AR-FIRE id=%d len=%d\n", (unsigned long long)cyc,
                            s_arId, s_arLen);
            ArPend a;
            a.id    = s_arId;
            a.total = uint32_t(s_arLen) + 1;
            a.gap   = rng() % 6;
            for (uint32_t k = 0; k < a.total; ++k)
                for (auto& w : a.data[k]) w = rng() | (uint64_t(rng()) << 32);
            arQ.push_back(a);
        }
        if (rPend && s_rRdy) {
            if (trace)
                std::printf("  cyc=%llu R-FIRE id=%d last=%d\n", (unsigned long long)cyc,
                            rBits.id, (int)rBits.last);
            for (auto& a : arQ) {
                if (a.id == rBits.id && a.sent < a.total) {
                    ++a.sent;
                    a.gap = rng() % 6;
                    break;
                }
            }
            rPend = false;
            for (auto it = arQ.begin(); it != arQ.end();)
                it = (it->sent >= it->total) ? arQ.erase(it) : it + 1;
        }

        if (draining) {
            bool any = reqPend || datPend;
            for (auto& t : txns) any = any || t.active;
            if (!any) drained = true;
        }
        if (st.mismatches > 50) break;
    }

    std::printf("%s sbridge seed=%u cycles=%llu(+%llu drain%s) checks=%llu mismatches=%llu | req=%llu rsp=%llu dat=%llu aw=%llu w=%llu ar=%llu datIn=%llu\n",
                st.mismatches == 0 ? "PASS" : "FAIL", seed, (unsigned long long)cycles,
                cyc > cycles ? (unsigned long long)(cyc - cycles) : 0ULL,
                drained ? "" : " 未排干",
                (unsigned long long)st.checks, (unsigned long long)st.mismatches,
                (unsigned long long)cReq, (unsigned long long)cRsp,
                (unsigned long long)cDat, (unsigned long long)cAw, (unsigned long long)cW,
                (unsigned long long)cAr, (unsigned long long)cDatIn);
    for (uint32_t i = 0; i < 64; ++i) {
        const auto& t = txns[i];
        if (t.active)
            std::printf(
                "  STUCK txn[%u] read=%d eca=%d size=%d addr=0x%llx txnId=0x%x retTxn=0x%x "
                "cmIdx=%d decided=%d cancel=%d beats=%d/%d comp=%d\n",
                i, (int)t.isRead, (int)t.eca, t.size, (unsigned long long)t.addr, t.txnId,
                t.returnTxn, t.cmIdx, (int)t.decided, (int)t.cancel, t.beatsSent,
                beatCnt(t.size), (int)t.compSeen);
    }
    if (trace) {
        // 末尾内部状态 dump：找死锁环
        std::printf("--- 内部状态（模型侧）---\n");
        const auto& dbs = dut.data_buf.st.get();
        std::printf("data_buf: avail=%d tx_req_vld=%d tx_cnt=%d tx_recv_max=%d rel_cnt=%d "
                    "awq_count=%d\n",
                    dbs.avail, (int)dbs.tx_req_vld, dbs.tx_cnt, dbs.tx_ctrl.recv_max,
                    dbs.rel_cnt, (int)dut.aw_q.count.get());
        std::printf("ar_arb: out.v=%d out.id=%d sel_reg[0]=%d sel_reg[1]=%d | "
                    "cm0.ar.v=%d cm1.ar.v=%d | ar_arb.in_rdy[0]=%d\n",
                    (int)dut.ar_arb.out.get().valid, dut.ar_arb.out.get().bits.id,
                    (int)dut.ar_arb.sel_reg.get()[0], (int)dut.ar_arb.sel_reg.get()[1],
                    (int)dut.cms[0]->axi_ar.get().valid, (int)dut.cms[1]->axi_ar.get().valid,
                    (int)dut.ar_arb.in_rdy.get()[0]);
        std::printf("aw_arb: out.v=%d sel_reg非零位数=", (int)dut.aw_arb.out.get().valid);
        int nz = 0;
        for (auto b : dut.aw_arb.sel_reg.get()) nz += b ? 1 : 0;
        std::printf("%d | rsp_arb: out.v=%d sel=%d\n", nz, (int)dut.rsp_arb.out.get().valid,
                    (int)dut.rsp_arb.out.get().bits.opcode);
        for (uint32_t i = 0; i < 64; ++i) {
            const auto& s = dut.cms[i]->st.get();
            if (!s.valid) continue;
            std::printf(
                "  cm_%d: wait=%d u(r=%d,d=%d,w=%d,rd=%d,c=%d) d(wa=%d,ra=%d,wd=%d,wr=%d,"
                "rd=%d) bufAlloc=%d allocIss=%d addr=0x%llx size=%d\n",
                i, s.waiting, (int)s.u.receiptResp, (int)s.u.dbidResp, (int)s.u.wdata,
                (int)s.u.rdata, (int)s.u.comp, (int)s.d.waddr, (int)s.d.raddr,
                (int)s.d.wdata, (int)s.d.wresp, (int)s.d.rdata, (int)s.buffer_allocated,
                (int)s.alloc_issued, (unsigned long long)s.info.addr, s.info.size);
        }
    }
    return st.mismatches;
}

// =================================================================
// HI 桥
// =================================================================

uint64_t cosimHiBridge(uint32_t seed, uint64_t cycles) {
    VAxiLiteBridge ref;
    HiNodeAxiLiteBridge dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;

    auto zeroInputs = [&] {
        ref.icn_rx_req_valid = 0;
        ref.icn_rx_resp_valid = 0;
        ref.icn_rx_data_valid = 0;
        ref.icn_tx_resp_ready = 0;
        ref.icn_tx_data_ready = 0;
        ref.axi_aw_ready = 0;
        ref.axi_ar_ready = 0;
        ref.axi_w_ready = 0;
        ref.axi_b_valid = 0;
        ref.axi_r_valid = 0;
    };
    cosim::resetRef(ref, zeroInputs);
    dut.node_id.set(0x20);
    dut.rx_req.set(Dec<RReqFlit>{});
    dut.rx_resp.set(Dec<RespFlit>{});
    dut.rx_data.set(Dec<DataFlit>{});
    dut.tx_resp_rdy.set(false);
    dut.tx_data_rdy.set(false);
    dut.axi_aw_rdy.set(false);
    dut.axi_ar_rdy.set(false);
    dut.axi_w_rdy.set(false);
    dut.axi_b.set(Dec<BFlit>{});
    dut.axi_r.set(Dec<RFlit>{});

    // 地址池：两个 tag（addr[18:3]）各 4 项——同 tag 冲突与并行并存（全池同
    // tag 时保序链过长会触发 RTL 的 5 万拍 timer 调试断言）
    const uint64_t pool[8] = {0x3800'0000, 0x3800'0008, 0x3800'0010, 0x3800'0018,
                              0x3800'4000, 0x3800'4008, 0x3800'4010, 0x3800'4018};

    std::array<TxnTrack, 8> txns{};
    std::array<uint64_t, 8> cmValidCyc{};
    uint32_t txnCursor = 0;
    bool     reqPend   = false;
    RReqFlit reqBits;
    // 写数据挂起
    bool     datPend = false;
    DataFlit datBits;
    TxnTrack* datTxn = nullptr;
    // CompAck 挂起
    bool     ackPend = false;
    RespFlit ackBits;
    // AXI 从机
    std::deque<AwPend> awQ;
    std::deque<ArPend> arQ;
    bool  rPend = false;
    RFlit rBits;

    auto idInUse = [&](uint16_t v) {
        for (const auto& t : txns)
            if (t.active && t.txnId == v) return true;
        return false;
    };

    auto newRequest = [&](uint64_t) -> bool {
        for (uint32_t k = 0; k < 8; ++k) {
            const uint32_t i = (txnCursor + k) % 8;
            if (txns[i].active) continue;
            txnCursor    = i + 1;
            TxnTrack& t  = txns[i];
            t            = TxnTrack{};
            t.active     = true;
            t.isRead     = (rng() % 100) < 50;
            t.eca        = (rng() % 100) < 40;
            t.size       = uint8_t(rng() % 4);  // <=3
            t.addr       = pool[rng() % 8];
            uint16_t v   = uint16_t(rng() % 0x1000);
            while (idInUse(v)) v = uint16_t(rng() % 0x1000);
            t.txnId      = v;
            reqBits      = RReqFlit{};
            reqBits.opcode      = t.isRead ? req_op::kReadNoSnp : req_op::kWriteNoSnpPtl;
            reqBits.addr        = t.addr;
            reqBits.size        = t.size;
            reqBits.txn_id      = t.txnId;
            reqBits.src_id      = 0x08;
            reqBits.order       = uint8_t(rng() % 4);
            reqBits.mem_attr    = uint8_t(rng() % 16);
            reqBits.exp_comp_ack = t.eca;
            reqBits.qos         = uint8_t(rng() % 16);
            reqPend             = true;
            return true;
        }
        return false;
    };

    uint64_t cyc     = 0;
    bool     drained = false;
    // 主循环 = 激励（cycles 拍）+ 排空（停止新请求、反压全撤，直至事务全清）
    for (; cyc < cycles + 100000 && !drained; ++cyc) {
        const bool     draining = cyc >= cycles;
        const uint32_t density  = cosim::densityAt(cyc, cycles);
        char buf[160];

        if (!draining && !reqPend && cosim::roll(rng, density / 2)) newRequest(cyc);

        // ---- 写数据组装（单拍；TxnID=cmIdx）----
        if (!datPend) {
            for (auto& t : txns)
                if (t.active && !t.isRead && t.cmIdx >= 0 && t.beatsSent == 0 &&
                    cosim::roll(rng, density)) {
                    datTxn              = &t;
                    datBits             = DataFlit{};
                    datBits.opcode      = dat_op::kNonCopyBackWriteData;
                    datBits.txn_id      = uint16_t(t.cmIdx);
                    datBits.data_id     = uint8_t((t.addr >> 3) & 3);
                    for (auto& w : datBits.data) w = rng() | (uint64_t(rng()) << 32);
                    datBits.be          = rng();
                    datPend             = true;
                    break;
                }
        }

        // ---- 驱动 ----
        if (reqPend) {
            ref.icn_rx_req_valid           = 1;
            ref.icn_rx_req_bits_ExpCompAck = reqBits.exp_comp_ack;
            ref.icn_rx_req_bits_MemAttr    = reqBits.mem_attr;
            ref.icn_rx_req_bits_Order      = reqBits.order;
            ref.icn_rx_req_bits_Addr       = reqBits.addr;
            ref.icn_rx_req_bits_Size       = reqBits.size;
            ref.icn_rx_req_bits_Opcode     = reqBits.opcode;
            ref.icn_rx_req_bits_TxnID      = reqBits.txn_id;
            ref.icn_rx_req_bits_SrcID      = reqBits.src_id;
            ref.icn_rx_req_bits_QoS        = reqBits.qos;
            Dec<RReqFlit> dq;
            dq.valid = true;
            dq.bits  = reqBits;
            dut.rx_req.set(dq);
        } else {
            ref.icn_rx_req_valid = 0;
            dut.rx_req.set(Dec<RReqFlit>{});
        }
        if (ackPend) {
            ref.icn_rx_resp_valid        = 1;
            ref.icn_rx_resp_bits_Opcode  = ackBits.opcode;
            ref.icn_rx_resp_bits_TxnID   = ackBits.txn_id;
            Dec<RespFlit> da;
            da.valid = true;
            da.bits  = ackBits;
            dut.rx_resp.set(da);
        } else {
            ref.icn_rx_resp_valid = 0;
            dut.rx_resp.set(Dec<RespFlit>{});
        }
        if (datPend) {
            ref.icn_rx_data_valid = 1;
            setWide(ref.icn_rx_data_bits_Data, datBits.data);
            ref.icn_rx_data_bits_Opcode = datBits.opcode;
            ref.icn_rx_data_bits_TxnID  = datBits.txn_id;
            Dec<DataFlit> dd;
            dd.valid = true;
            dd.bits  = datBits;
            dut.rx_data.set(dd);
        } else {
            ref.icn_rx_data_valid = 0;
            dut.rx_data.set(Dec<DataFlit>{});
        }

        // rdy 反压（排空期全撤）
        const bool rspRdy = draining || cosim::roll(rng, density);
        const bool datRdy = draining || cosim::roll(rng, density);
        const bool awRdy  = draining || cosim::roll(rng, density);
        const bool arRdy  = draining || cosim::roll(rng, density);
        const bool wRdy   = draining || cosim::roll(rng, density);
        ref.icn_tx_resp_ready = rspRdy;
        ref.icn_tx_data_ready = datRdy;
        ref.axi_aw_ready      = awRdy;
        ref.axi_ar_ready      = arRdy;
        ref.axi_w_ready       = wRdy;
        dut.tx_resp_rdy.set(rspRdy);
        dut.tx_data_rdy.set(datRdy);
        dut.axi_aw_rdy.set(awRdy);
        dut.axi_ar_rdy.set(arRdy);
        dut.axi_w_rdy.set(wRdy);

        for (auto& a : awQ)
            if (draining)
                a.bDelay = 0;
            else if (a.wDone && a.bDelay > 0)
                --a.bDelay;
        const bool bDrive = !awQ.empty() && awQ.front().wDone && awQ.front().bDelay == 0;
        ref.axi_b_valid   = bDrive;
        ref.axi_b_bits_id = bDrive ? awQ.front().id : 0;
        {
            Dec<BFlit> db;
            db.valid     = bDrive;
            db.bits.id   = bDrive ? awQ.front().id : 0;
            db.bits.resp = 0;
            dut.axi_b.set(db);
        }

        if (!rPend) {
            for (auto& a : arQ)
                if (draining)
                    a.gap = 0;
                else if (a.gap > 0)
                    --a.gap;
            for (auto& a : arQ) {
                if (a.sent < a.total && a.gap == 0) {
                    rPend      = true;
                    rBits      = RFlit{};
                    rBits.id   = a.id;
                    rBits.data = a.data[a.sent];
                    rBits.last = a.sent + 1 == a.total;
                    break;
                }
            }
        }
        ref.axi_r_valid = rPend;
        ref.axi_r_bits_id = rBits.id;
        setWide(ref.axi_r_bits_data, rBits.data);
        ref.axi_r_bits_resp = rBits.resp;
        ref.axi_r_bits_last = rBits.last;
        {
            Dec<RFlit> dr;
            dr.valid = rPend;
            dr.bits  = rBits;
            dut.axi_r.set(dr);
        }

        // ---- 拍低相：比对 ----
        cosim::phaseLow(ref, dut);
        rp.push([&] {
            std::snprintf(buf, sizeof buf,
                          "cyc=%llu reqV=%d rspV=%d datV=%d rspRdy=%d datRdy=%d awRdy=%d "
                          "arRdy=%d wRdy=%d bV=%d rV=%d",
                          (unsigned long long)cyc, (int)ref.icn_rx_req_valid,
                          (int)ref.icn_rx_resp_valid, (int)ref.icn_rx_data_valid,
                          (int)rspRdy, (int)datRdy, (int)awRdy, (int)arRdy, (int)wRdy,
                          (int)bDrive, (int)rPend);
            return std::string(buf);
        }());
        auto cmp = [&](const char* port, uint64_t r, uint64_t d) {
            cosim::check(st, "hibridge", "hi", seed, cyc, port, r, d, rp);
        };
        cmp("rx_req_rdy", ref.icn_rx_req_ready, dut.rx_req_rdy.get());
        cmp("tx_resp_vld", ref.icn_tx_resp_valid, dut.tx_resp.get().valid);
        if (ref.icn_tx_resp_valid) {
            const auto& d = dut.tx_resp.get().bits;
            cmp("tx_resp_dbid", ref.icn_tx_resp_bits_DBID, d.dbid);
            cmp("tx_resp_op", ref.icn_tx_resp_bits_Opcode, d.opcode);
            cmp("tx_resp_txn", ref.icn_tx_resp_bits_TxnID, d.txn_id);
            cmp("tx_resp_tgt", ref.icn_tx_resp_bits_TgtID, d.tgt_id);
            cmp("tx_resp_qos", ref.icn_tx_resp_bits_QoS, d.qos);
        }
        cmp("tx_data_vld", ref.icn_tx_data_valid, dut.tx_data.get().valid);
        if (ref.icn_tx_data_valid) {
            const auto& d = dut.tx_data.get().bits;
            for (int i = 0; i < 4; ++i)
                cmp(cosim::lanePort("tx_data_data", i).c_str(),
                    wideW(ref.icn_tx_data_bits_Data, i), d.data[i]);
            cmp("tx_data_be", ref.icn_tx_data_bits_BE, d.be);
            cmp("tx_data_did", ref.icn_tx_data_bits_DataID, d.data_id);
            cmp("tx_data_dbid", ref.icn_tx_data_bits_DBID, d.dbid);
            cmp("tx_data_op", ref.icn_tx_data_bits_Opcode, d.opcode);
            cmp("tx_data_txn", ref.icn_tx_data_bits_TxnID, d.txn_id);
            cmp("tx_data_src", ref.icn_tx_data_bits_SrcID, d.src_id);
            cmp("tx_data_tgt", ref.icn_tx_data_bits_TgtID, d.tgt_id);
            cmp("tx_data_home", ref.icn_tx_data_bits_HomeNID, d.home_nid);
            cmp("tx_data_qos", ref.icn_tx_data_bits_QoS, d.qos);
            cmp("tx_data_resp", ref.icn_tx_data_bits_Resp, d.resp);
            cmp("tx_data_rerr", ref.icn_tx_data_bits_RespErr, d.resp_err);
            cmp("tx_data_ds", ref.icn_tx_data_bits_DataSource, d.data_source);
            cmp("tx_data_cbusy", ref.icn_tx_data_bits_CBusy, d.c_busy);
        }
        for (int ch = 0; ch < 2; ++ch) {
            const bool rv = ch == 0 ? ref.axi_aw_valid : ref.axi_ar_valid;
            const bool dv = ch == 0 ? dut.axi_aw.get().valid : dut.axi_ar.get().valid;
            cmp(ch == 0 ? "aw_vld" : "ar_vld", rv, dv);
            if (rv) {
                const auto& d = ch == 0 ? dut.axi_aw.get().bits : dut.axi_ar.get().bits;
                cmp("ax_id", ch == 0 ? ref.axi_aw_bits_id : ref.axi_ar_bits_id, d.id);
                cmp("ax_addr", ch == 0 ? ref.axi_aw_bits_addr : ref.axi_ar_bits_addr, d.addr);
                cmp("ax_size", ch == 0 ? ref.axi_aw_bits_size : ref.axi_ar_bits_size, d.size);
                cmp("ax_burst", ch == 0 ? ref.axi_aw_bits_burst : ref.axi_ar_bits_burst,
                    d.burst);
                cmp("ax_qos", ch == 0 ? ref.axi_aw_bits_qos : ref.axi_ar_bits_qos, d.qos);
            }
        }
        cmp("w_vld", ref.axi_w_valid, dut.axi_w.get().valid);
        if (ref.axi_w_valid) {
            const auto& d = dut.axi_w.get().bits;
            for (int i = 0; i < 4; ++i)
                cmp(cosim::lanePort("w_data", i).c_str(), wideW(ref.axi_w_bits_data, i),
                    d.data[i]);
            cmp("w_strb", ref.axi_w_bits_strb, d.strb);
            cmp("w_last", ref.axi_w_bits_last, d.last);
        }
        cmp("r_rdy", ref.axi_r_ready, dut.axi_r_rdy.get());

        // ---- 拍内采样值捕获 ----
        const bool     s_reqRdy  = ref.icn_rx_req_ready;
        const bool     s_rspV    = ref.icn_tx_resp_valid;
        const uint8_t  s_rspOp   = ref.icn_tx_resp_bits_Opcode;
        const uint16_t s_rspTxn  = ref.icn_tx_resp_bits_TxnID;
        const uint16_t s_rspDbid = ref.icn_tx_resp_bits_DBID;
        const bool     s_datV    = ref.icn_tx_data_valid;
        const uint16_t s_datTxn  = ref.icn_tx_data_bits_TxnID;
        const uint16_t s_datDbid = ref.icn_tx_data_bits_DBID;
        const bool     s_awV     = ref.axi_aw_valid;
        const uint8_t  s_awId    = ref.axi_aw_bits_id;
        const bool     s_arV     = ref.axi_ar_valid;
        const uint8_t  s_arId    = ref.axi_ar_bits_id;
        const bool     s_wV      = ref.axi_w_valid;
        const bool     s_rRdy    = ref.axi_r_ready;

        // ---- 拍高相 ----
        cosim::phaseHigh(ref, dut);

        // 看门狗：任一 CM 连续 valid 超 5 万拍即打印内部态（镜像 RTL 的
        // "bridge CM time out" 调试断言；挂死回归时直接定位现场）
        for (uint32_t i = 0; i < 8; ++i) {
            const auto& s = dut.cms[i]->st.get();
            if (s.valid) {
                if (++cmValidCyc[i] == 50001) {
                    std::printf(
                        "cyc=%llu cm_%u 50k: wait=%d u(r=%d,d=%d,w=%d,rd=%d,ca=%d,"
                        "c=%d) d(wa=%d,ra=%d,wd=%d,wr=%d,rd=%d) snoop=%d addr=0x%llx "
                        "txnId=0x%x\n",
                        (unsigned long long)cyc, i, s.waiting, (int)s.u.receiptResp,
                        (int)s.u.dbidResp, (int)s.u.wdata, (int)s.u.rdata,
                        (int)s.u.compAck, (int)s.u.comp, (int)s.d.waddr, (int)s.d.raddr,
                        (int)s.d.wdata, (int)s.d.wresp, (int)s.d.rdata,
                        (int)s.info.isSnooped, (unsigned long long)s.info.addr,
                        s.info.txn_id);
                }
            } else {
                cmValidCyc[i] = 0;
            }
        }

        // ---- 事后状态更新（全部用拍内采样值）----
        if (reqPend && s_reqRdy) reqPend = false;
        if (datPend) {  // rx.data 恒 ready（无 rdy 端口）
            ++datTxn->beatsSent;
            datPend = false;
        }
        if (ackPend) ackPend = false;  // rx.resp 恒 ready
        // tx_resp 观察：DBID(CompDBID)Resp → cmIdx + compSeen；Comp → compSeen
        if (s_rspV && rspRdy) {
            for (auto& t : txns) {
                if (!t.active || t.isRead || s_rspTxn != t.txnId) continue;
                if (s_rspOp == rsp_op::kDBIDResp || s_rspOp == rsp_op::kCompDBIDResp) {
                    if (t.cmIdx < 0) t.cmIdx = s_rspDbid;
                    if (s_rspOp == rsp_op::kCompDBIDResp) t.compSeen = true;
                } else if (s_rspOp == rsp_op::kComp) {
                    t.compSeen = true;
                }
            }
        }
        // CompData 观察：ECA 读 → 回 CompAck（TxnID=DBID=CM idx）
        if (s_datV && datRdy) {
            for (auto& t : txns) {
                if (!t.active || !t.isRead || s_datTxn != t.txnId) continue;
                if (t.eca && !t.compAckSent) {
                    ackPend          = true;
                    ackBits          = RespFlit{};
                    ackBits.opcode   = rsp_op::kCompAck;
                    ackBits.txn_id   = s_datDbid;  // = CM idx
                    t.compAckSent    = true;
                }
                t.active = false;  // HI 读单拍 CompData 即完成
            }
        }
        // ECA 写：DBIDResp/CompDBIDResp 且写数据发完后回 CompAck。
        // 必须等数据先行（beatsSent>=1）：RTL BaseCtrlMachine.scala:106-117 在同拍
        // rx.data+rx.resp 同到时以后者覆盖 compAck（香山假设协议保序：CompAck 不
        // 与写数据同拍到达），同拍到达会被双方同时丢弃 → CM 永久泄漏。
        if (!ackPend) {
            for (auto& t : txns)
                if (t.active && !t.isRead && t.eca && t.cmIdx >= 0 &&
                    t.beatsSent >= 1 && !t.compAckSent) {
                    ackPend          = true;
                    ackBits          = RespFlit{};
                    ackBits.opcode   = rsp_op::kCompAck;
                    ackBits.txn_id   = uint16_t(t.cmIdx);
                    t.compAckSent    = true;
                    break;
                }
        }
        for (auto& t : txns)
            if (t.active && !t.isRead && t.compSeen && t.beatsSent >= 1 &&
                (!t.eca || t.compAckSent))  // ECA 须先回 CompAck 再回收
                t.active = false;
        if (s_awV && awRdy) {
            AwPend a;
            a.id     = s_awId;
            a.len    = 0;  // HI 恒单拍
            a.bDelay = rng() % 8;
            awQ.push_back(a);
        }
        if (s_wV && wRdy) {
            for (auto& a : awQ) {
                if (!a.wDone) {
                    if (++a.wBeats > a.len) a.wDone = true;
                    break;
                }
            }
        }
        if (bDrive) awQ.pop_front();
        if (s_arV && arRdy) {
            ArPend a;
            a.id    = s_arId;
            a.total = 1;  // HI len 恒 0
            a.gap   = rng() % 6;
            for (auto& w : a.data[0]) w = rng() | (uint64_t(rng()) << 32);
            arQ.push_back(a);
        }
        if (rPend && s_rRdy) {
            for (auto& a : arQ) {
                if (a.id == rBits.id && a.sent < a.total) {
                    ++a.sent;
                    a.gap = rng() % 6;
                    break;
                }
            }
            rPend = false;
            for (auto it = arQ.begin(); it != arQ.end();)
                it = (it->sent >= it->total) ? arQ.erase(it) : it + 1;
        }

        if (draining) {
            bool any = reqPend || datPend || ackPend;
            for (auto& t : txns) any = any || t.active;
            if (!any) drained = true;
        }
        if (st.mismatches > 50) break;
    }

    std::printf("%s hibridge seed=%u cycles=%llu(+%llu drain%s) checks=%llu mismatches=%llu\n",
                st.mismatches == 0 ? "PASS" : "FAIL", seed, (unsigned long long)cycles,
                cyc > cycles ? (unsigned long long)(cyc - cycles) : 0ULL,
                drained ? "" : " 未排干", (unsigned long long)st.checks,
                (unsigned long long)st.mismatches);
    uint32_t nz = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        const auto& t = txns[i];
        if (t.active) {
            ++nz;
            std::printf(
                "  STUCK txn[%u] read=%d eca=%d txnId=0x%x cmIdx=%d beats=%d comp=%d "
                "ackSent=%d\n",
                i, (int)t.isRead, (int)t.eca, t.txnId, t.cmIdx, t.beatsSent,
                (int)t.compSeen, (int)t.compAckSent);
        }
    }
    if (getenv("BRIDGE_TRACE") || nz > 0) {
        for (uint32_t i = 0; i < 8; ++i) {
            const auto& s = dut.cms[i]->st.get();
            if (!s.valid) continue;
            std::printf(
                "  cm_%u: wait=%d u(r=%d,d=%d,w=%d,rd=%d,ca=%d,c=%d) d(wa=%d,ra=%d,"
                "wd=%d,wr=%d,rd=%d) snoop=%d addr=0x%llx size=%d\n",
                i, s.waiting, (int)s.u.receiptResp, (int)s.u.dbidResp, (int)s.u.wdata,
                (int)s.u.rdata, (int)s.u.compAck, (int)s.u.comp, (int)s.d.waddr,
                (int)s.d.raddr, (int)s.d.wdata, (int)s.d.wresp, (int)s.d.rdata,
                (int)s.info.isSnooped, (unsigned long long)s.info.addr, s.info.size);
        }
    }
    return st.mismatches;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    const char* cycEnv = getenv("BRIDGE_CYCLES");
    const uint64_t n = cycEnv != nullptr ? std::stoull(cycEnv) : 100000;
    const char* only = getenv("BRIDGE_ONLY");
    if (only == nullptr || std::string(only) == "s") {
        bad += cosimSBridge(99, n);
        bad += cosimSBridge(999, n);
    }
    if (only == nullptr || std::string(only) == "hi") {
        bad += cosimHiBridge(99, n);
        bad += cosimHiBridge(999, n);
    }
    std::printf("%s bridge\n", bad == 0 ? "ALL-PASS" : "SOME-FAIL");
    return bad == 0 ? 0 : 1;
}
