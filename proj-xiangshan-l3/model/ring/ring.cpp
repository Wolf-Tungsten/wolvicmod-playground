#include "model/ring/ring.h"

#include <wolvicmod/wolvicmod.h>

namespace zj::ring {

using wolvicmod::Reg;
using wolvicmod::Wire;

Valid<RReqFlit> Ring::rnDecode(Valid<RReqFlit> in, uint8_t ci, const RnDec& dec) {
    const bool     device = (in.bits.mem_attr >> 1) & 1;  // MemAttr.device
    const uint64_t a      = in.bits.addr;
    uint8_t        tgtGid = dec.defaultHniGid;
    if (device && ((a >> 44) & 0xF) == ci && (a & dec.ccMask) == dec.ccBase) {
        tgtGid = dec.ccGid;
    } else if (!device) {
        for (int k = 0; k < 2; ++k)
            if (((a >> 12) & 1) == dec.friendBank[k]) {  // checkBank：addr[12]
                tgtGid = dec.friendGid[k];
                break;
            }
    }
    in.bits.tgt_id = uint16_t(tgtGid) << 3;
    return in;
}

template <class F, uint32_t EjD, bool IsDat>
void Ring::createChanEnts(ChanEnts<F, EjD, IsDat>& E, const char* pfx) {
    for (int i = 0; i < 10; ++i) {
        const std::string p = std::string(pfx) + "_s" + std::to_string(i);
        E.st[i]             = &createReg<ChanStop<F, EjD, IsDat>>(p + "_st");
        E.cb[i]             = &createWire<ChanComb<F>>(p + "_c");
        E.gt[i]             = &createWire<bool>(p + "_g");
    }
}

// REQ/RSP/DAT 通道的单站注册：组合视图 + 静止门 + 次态 + 边界端口。
// rx0 = 左邻 dir[0] 打拍、rx1 = 右邻 dir[1] 打拍（Ring.scala:23-26）。
template <class F, uint32_t EjD, bool IsDat, size_t I>
void Ring::buildStop(ChanEnts<F, EjD, IsDat>& E, std::integral_constant<size_t, I>) {
    constexpr StopSpec sp     = kStopTable[I];
    constexpr bool     kIsReq = std::is_same_v<F, RReqFlit>;
    constexpr bool     kIsRsp = std::is_same_v<F, RespFlit>;
    constexpr bool     hasInj = kIsReq ? sp.injReq : kIsRsp ? sp.injRsp : sp.injDat;
    constexpr bool     hasEj  = kIsReq ? sp.ejReq : kIsRsp ? sp.ejRsp : sp.ejDat;

    auto& st  = *E.st[I];
    auto& cb  = *E.cb[I];
    auto& g   = *E.gt[I];
    auto& stL = *E.st[(I + 9) % 10];
    auto& stR = *E.st[(I + 1) % 10];

    In<Valid<F>>*  rxP    = nullptr;
    Out<bool>*     rxRdyP = nullptr;
    Out<Valid<F>>* txP    = nullptr;
    In<bool>*      txRdyP = nullptr;
    if constexpr (kIsReq) {
        rxP = stops[I].rx_req;  rxRdyP = stops[I].rx_req_rdy;
        txP = stops[I].tx_req;  txRdyP = stops[I].tx_req_rdy;
    } else if constexpr (kIsRsp) {
        rxP = stops[I].rx_resp; rxRdyP = stops[I].rx_resp_rdy;
        txP = stops[I].tx_resp; txRdyP = stops[I].tx_resp_rdy;
    } else {
        rxP = stops[I].rx_data; rxRdyP = stops[I].rx_data_rdy;
        txP = stops[I].tx_data; txRdyP = stops[I].tx_data_rdy;
    }

    constexpr uint16_t rm  = sp.rightMask;
    constexpr uint16_t lm  = sp.leftMask;
    constexpr uint8_t  gid = sp.gid;

    // 组合视图（注入源存在性/译码/弹出反压按站型别编译期分支）
    if constexpr (hasInj && kIsReq) {
        // CC/RI REQ 注入：RnRouter 地址译码改 TgtID（router/package.scala:11-35）
        const RnDec dec = sp.dec;
        if constexpr (hasEj) {
            cb.assign().reads(st, stL, stR, *rxP, *txRdyP, ci) = [=](auto src) {
                auto [s, l, r, rxv, txr, ciV] = src;
                return chanComb<F, EjD, IsDat>(s, l.dir[0].slot, r.dir[1].slot,
                                               rnDecode(rxv, ciV, dec), Valid<F>{}, false,
                                               rm, lm, gid, hasInj, hasEj, txr);
            };
        } else {
            cb.assign().reads(st, stL, stR, *rxP, ci) = [=](auto src) {
                auto [s, l, r, rxv, ciV] = src;
                return chanComb<F, EjD, IsDat>(s, l.dir[0].slot, r.dir[1].slot,
                                               rnDecode(rxv, ciV, dec), Valid<F>{}, false,
                                               rm, lm, gid, hasInj, hasEj, false);
            };
        }
    } else if constexpr (hasInj && hasEj) {
        cb.assign().reads(st, stL, stR, *rxP, *txRdyP) = [=](auto src) {
            auto [s, l, r, rxv, txr] = src;
            return chanComb<F, EjD, IsDat>(s, l.dir[0].slot, r.dir[1].slot, rxv, Valid<F>{},
                                           false, rm, lm, gid, hasInj, hasEj, txr);
        };
    } else if constexpr (hasInj) {
        cb.assign().reads(st, stL, stR, *rxP) = [=](auto src) {
            auto [s, l, r, rxv] = src;
            return chanComb<F, EjD, IsDat>(s, l.dir[0].slot, r.dir[1].slot, rxv, Valid<F>{},
                                           false, rm, lm, gid, hasInj, hasEj, false);
        };
    } else if constexpr (hasEj) {
        cb.assign().reads(st, stL, stR, *txRdyP) = [=](auto src) {
            auto [s, l, r, txr] = src;
            return chanComb<F, EjD, IsDat>(s, l.dir[0].slot, r.dir[1].slot, Valid<F>{},
                                           Valid<F>{}, false, rm, lm, gid, hasInj, hasEj, txr);
        };
    } else {
        // 纯打拍站（无注入无弹出 ⇒ RingPipe 直透）
        cb.assign().reads(st, stL, stR) = [=](auto src) {
            auto [s, l, r] = src;
            return chanComb<F, EjD, IsDat>(s, l.dir[0].slot, r.dir[1].slot, Valid<F>{},
                                           Valid<F>{}, false, rm, lm, gid, false, false, false);
        };
    }
    g.assign().reads(st, cb) = [](auto src) {
        auto [s, c] = src;
        return chanActive<F, EjD, IsDat>(s, c);
    };
    st.update().on(posedge(clk)).en(g).reads(st, cb) = [](auto src) {
        auto [s, c] = src;
        return chanNext<F, EjD, IsDat>(s, c, false, hasInj, hasEj);
    };
    if constexpr (hasInj)
        rxRdyP->assign().reads(cb) = [](auto src) { return std::get<0>(src).inj_src_rdy[0]; };
    if constexpr (hasEj) txP->assign().reads(cb) = [](auto src) { return std::get<0>(src).tx; };
}

// HRQ 通道：HF = ERQ+SNP 双源 RR 合并注入（无弹出）；HI = 仅 ERQ 注入；
// CC = 弹出转 SnoopFlit；S = 弹出转 HReqFlit；其余站纯打拍。
template <size_t I>
void Ring::buildHrqStop(std::integral_constant<size_t, I>) {
    constexpr StopSpec sp     = kStopTable[I];
    constexpr bool     hasInj = sp.injHrq;
    constexpr bool     hasEj  = sp.ejHrq;
    constexpr bool     dual   = sp.hrqInjArb;

    auto& st  = *hrqE.st[I];
    auto& cb  = *hrqE.cb[I];
    auto& g   = *hrqE.gt[I];
    auto& stL = *hrqE.st[(I + 9) % 10];
    auto& stR = *hrqE.st[(I + 1) % 10];

    constexpr uint16_t rm  = sp.rightMask;
    constexpr uint16_t lm  = sp.leftMask;
    constexpr uint8_t  gid = sp.gid;

    if constexpr (dual) {
        // HF：ERQ+SNP 经 ResetRRArbiter(2) 合并（BaseRouter.scala:144-148）
        cb.assign().reads(st, stL, stR, *stops[I].rx_erq, *stops[I].rx_snoop) =
            [=](auto src) {
                auto [s, l, r, erq, snp] = src;
                Valid<HrqFlit> a{erq.valid, HrqFlit::fromHreq(erq.bits)};
                Valid<HrqFlit> b{snp.valid, HrqFlit::fromSnp(snp.bits)};
                return chanComb<HrqFlit, 5, false>(s, l.dir[0].slot, r.dir[1].slot, a, b, true,
                                                   rm, lm, gid, true, false, false);
            };
        stops[I].rx_erq_rdy->assign().reads(cb) =
            [](auto src) { return std::get<0>(src).inj_src_rdy[0]; };
        stops[I].rx_snoop_rdy->assign().reads(cb) =
            [](auto src) { return std::get<0>(src).inj_src_rdy[1]; };
    } else if constexpr (hasInj) {
        // HI：仅 ERQ 直连（BaseRouter.scala:149-150）
        cb.assign().reads(st, stL, stR, *stops[I].rx_erq) = [=](auto src) {
            auto [s, l, r, erq] = src;
            Valid<HrqFlit> a{erq.valid, HrqFlit::fromHreq(erq.bits)};
            return chanComb<HrqFlit, 5, false>(s, l.dir[0].slot, r.dir[1].slot, a,
                                               Valid<HrqFlit>{}, false, rm, lm, gid, true,
                                               false, false);
        };
        stops[I].rx_erq_rdy->assign().reads(cb) =
            [](auto src) { return std::get<0>(src).inj_src_rdy[0]; };
    } else if constexpr (hasEj) {
        // CC（弹 SNP）/ S（弹 ERQ）
        constexpr bool toSnp   = sp.kind == kCC;
        In<bool>*      ejRdyP  = toSnp ? stops[I].tx_snoop_rdy : stops[I].tx_erq_rdy;
        cb.assign().reads(st, stL, stR, *ejRdyP) = [=](auto src) {
            auto [s, l, r, ejr] = src;
            return chanComb<HrqFlit, 5, false>(s, l.dir[0].slot, r.dir[1].slot,
                                               Valid<HrqFlit>{}, Valid<HrqFlit>{}, false, rm,
                                               lm, gid, false, true, ejr);
        };
        if constexpr (toSnp)
            stops[I].tx_snoop->assign().reads(cb) = [](auto src) {
                auto [c] = src;
                return Valid<SnoopFlit>{c.tx.valid, c.tx.bits.toSnp()};
            };
        else
            stops[I].tx_erq->assign().reads(cb) = [](auto src) {
                auto [c] = src;
                return Valid<HReqFlit>{c.tx.valid, c.tx.bits.toHreq()};
            };
    } else {
        cb.assign().reads(st, stL, stR) = [=](auto src) {
            auto [s, l, r] = src;
            return chanComb<HrqFlit, 5, false>(s, l.dir[0].slot, r.dir[1].slot,
                                               Valid<HrqFlit>{}, Valid<HrqFlit>{}, false, rm,
                                               lm, gid, false, false, false);
        };
    }
    g.assign().reads(st, cb) = [](auto src) {
        auto [s, c] = src;
        return chanActive<HrqFlit, 5, false>(s, c);
    };
    st.update().on(posedge(clk)).en(g).reads(st, cb) = [](auto src) {
        auto [s, c] = src;
        return chanNext<HrqFlit, 5, false>(s, c, dual, hasInj, hasEj);
    };
}

Ring::Ring() {
    // 站边界端口视图填充（按站索引；空缺字段保持 nullptr，与 kStopTable 一致）
    stops[1].rx_req = &n1_rx_req;  stops[1].rx_req_rdy = &n1_rx_req_rdy;
    stops[3].rx_req = &n3_rx_req;  stops[3].rx_req_rdy = &n3_rx_req_rdy;
    stops[0].tx_req = &n0_tx_req;  stops[0].tx_req_rdy = &n0_tx_req_rdy;
    stops[1].tx_req = &n1_tx_req;  stops[1].tx_req_rdy = &n1_tx_req_rdy;
    stops[2].tx_req = &n2_tx_req;  stops[2].tx_req_rdy = &n2_tx_req_rdy;
    stops[4].tx_req = &n4_tx_req;  stops[4].tx_req_rdy = &n4_tx_req_rdy;
    stops[5].tx_req = &n5_tx_req;  stops[5].tx_req_rdy = &n5_tx_req_rdy;
    stops[7].tx_req = &n7_tx_req;  stops[7].tx_req_rdy = &n7_tx_req_rdy;

    stops[0].rx_erq = &n0_rx_req;  stops[0].rx_erq_rdy = &n0_rx_req_rdy;
    stops[2].rx_erq = &n2_rx_req;  stops[2].rx_erq_rdy = &n2_rx_req_rdy;
    stops[4].rx_erq = &n4_rx_req;  stops[4].rx_erq_rdy = &n4_rx_req_rdy;
    stops[5].rx_erq = &n5_rx_req;  stops[5].rx_erq_rdy = &n5_rx_req_rdy;
    stops[7].rx_erq = &n7_rx_req;  stops[7].rx_erq_rdy = &n7_rx_req_rdy;
    stops[6].tx_erq = &n6_tx_req;  stops[6].tx_erq_rdy = &n6_tx_req_rdy;

    stops[0].rx_resp = &n0_rx_resp; stops[0].rx_resp_rdy = &n0_rx_resp_rdy;
    stops[1].rx_resp = &n1_rx_resp; stops[1].rx_resp_rdy = &n1_rx_resp_rdy;
    stops[2].rx_resp = &n2_rx_resp; stops[2].rx_resp_rdy = &n2_rx_resp_rdy;
    stops[3].rx_resp = &n3_rx_resp; stops[3].rx_resp_rdy = &n3_rx_resp_rdy;
    stops[4].rx_resp = &n4_rx_resp; stops[4].rx_resp_rdy = &n4_rx_resp_rdy;
    stops[5].rx_resp = &n5_rx_resp; stops[5].rx_resp_rdy = &n5_rx_resp_rdy;
    stops[6].rx_resp = &n6_rx_resp; stops[6].rx_resp_rdy = &n6_rx_resp_rdy;
    stops[7].rx_resp = &n7_rx_resp; stops[7].rx_resp_rdy = &n7_rx_resp_rdy;
    stops[0].tx_resp = &n0_tx_resp; stops[0].tx_resp_rdy = &n0_tx_resp_rdy;
    stops[1].tx_resp = &n1_tx_resp; stops[1].tx_resp_rdy = &n1_tx_resp_rdy;
    stops[2].tx_resp = &n2_tx_resp; stops[2].tx_resp_rdy = &n2_tx_resp_rdy;
    stops[3].tx_resp = &n3_tx_resp; stops[3].tx_resp_rdy = &n3_tx_resp_rdy;
    stops[4].tx_resp = &n4_tx_resp; stops[4].tx_resp_rdy = &n4_tx_resp_rdy;
    stops[5].tx_resp = &n5_tx_resp; stops[5].tx_resp_rdy = &n5_tx_resp_rdy;
    stops[7].tx_resp = &n7_tx_resp; stops[7].tx_resp_rdy = &n7_tx_resp_rdy;

    stops[0].rx_data = &n0_rx_data; stops[0].rx_data_rdy = &n0_rx_data_rdy;
    stops[1].rx_data = &n1_rx_data; stops[1].rx_data_rdy = &n1_rx_data_rdy;
    stops[2].rx_data = &n2_rx_data; stops[2].rx_data_rdy = &n2_rx_data_rdy;
    stops[3].rx_data = &n3_rx_data; stops[3].rx_data_rdy = &n3_rx_data_rdy;
    stops[4].rx_data = &n4_rx_data; stops[4].rx_data_rdy = &n4_rx_data_rdy;
    stops[5].rx_data = &n5_rx_data; stops[5].rx_data_rdy = &n5_rx_data_rdy;
    stops[6].rx_data = &n6_rx_data; stops[6].rx_data_rdy = &n6_rx_data_rdy;
    stops[7].rx_data = &n7_rx_data; stops[7].rx_data_rdy = &n7_rx_data_rdy;
    stops[0].tx_data = &n0_tx_data; stops[0].tx_data_rdy = &n0_tx_data_rdy;
    stops[1].tx_data = &n1_tx_data; stops[1].tx_data_rdy = &n1_tx_data_rdy;
    stops[2].tx_data = &n2_tx_data; stops[2].tx_data_rdy = &n2_tx_data_rdy;
    stops[3].tx_data = &n3_tx_data; stops[3].tx_data_rdy = &n3_tx_data_rdy;
    stops[4].tx_data = &n4_tx_data; stops[4].tx_data_rdy = &n4_tx_data_rdy;
    stops[5].tx_data = &n5_tx_data; stops[5].tx_data_rdy = &n5_tx_data_rdy;
    stops[6].tx_data = &n6_tx_data; stops[6].tx_data_rdy = &n6_tx_data_rdy;
    stops[7].tx_data = &n7_tx_data; stops[7].tx_data_rdy = &n7_tx_data_rdy;

    stops[0].rx_snoop = &n0_rx_snoop; stops[0].rx_snoop_rdy = &n0_rx_snoop_rdy;
    stops[2].rx_snoop = &n2_rx_snoop; stops[2].rx_snoop_rdy = &n2_rx_snoop_rdy;
    stops[5].rx_snoop = &n5_rx_snoop; stops[5].rx_snoop_rdy = &n5_rx_snoop_rdy;
    stops[7].rx_snoop = &n7_rx_snoop; stops[7].rx_snoop_rdy = &n7_rx_snoop_rdy;
    stops[1].tx_snoop = &n1_tx_snoop; stops[1].tx_snoop_rdy = &n1_tx_snoop_rdy;

    // 建站：先实体后逻辑（编译期展开 10 站）
    createChanEnts(reqE, "req");
    createChanEnts(rspE, "rsp");
    createChanEnts(datE, "dat");
    createChanEnts(hrqE, "hrq");
    [this]<size_t... Is>(std::index_sequence<Is...>) {
        (buildStop(reqE, std::integral_constant<size_t, Is>{}), ...);
        (buildStop(rspE, std::integral_constant<size_t, Is>{}), ...);
        (buildStop(datE, std::integral_constant<size_t, Is>{}), ...);
        (buildHrqStop(std::integral_constant<size_t, Is>{}), ...);
    }(std::make_index_sequence<10>{});
}

}  // namespace zj::ring
