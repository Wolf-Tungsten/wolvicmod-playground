#include "model/ring/ring.h"

#include <wolvicmod/wolvicmod.h>

namespace zj::ring {

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

void Ring::buildReqChan(int i, In<Valid<RReqFlit>>* rx, Out<bool>* rxRdy,
                        Out<Valid<RReqFlit>>* tx, In<bool>* txRdy,
                        LaneEnds<RReqFlit>& lane) {
    const StopSpec&                 sp = kStopTable[i];
    ChannelTap<RReqFlit, 5, false>* tap;
    Queue<RReqFlit, 2>*             injq;
    buildChannel(i, lane, sp.injReq, sp.ejReq, tap, injq, "req");
    if (injq != nullptr) {
        if (sp.dec.enabled) {  // CC/RI：RnRouter 地址译码改 TgtID
            const RnDec dec = sp.dec;
            injq->enq.assign().reads(*rx, ci) = [dec](auto src) {
                auto [rx, ci] = src;
                return rnDecode(rx, ci, dec);
            };
        } else {
            injq->enq = *rx;
        }
        *rxRdy = injq->enq_rdy;
    }
    if (tap != nullptr && sp.ejReq) {
        *tx = tap->eject;
        tap->eject_rdy = *txRdy;
    }
}

void Ring::buildRspChan(int i, In<Valid<RespFlit>>* rx, Out<bool>* rxRdy,
                        Out<Valid<RespFlit>>* tx, In<bool>* txRdy,
                        LaneEnds<RespFlit>& lane) {
    const StopSpec&                sp = kStopTable[i];
    ChannelTap<RespFlit, 3, false>* tap;
    Queue<RespFlit, 2>*            injq;
    buildChannel(i, lane, sp.injRsp, sp.ejRsp, tap, injq, "rsp");
    if (injq != nullptr) {
        injq->enq = *rx;
        *rxRdy = injq->enq_rdy;
    }
    if (tap != nullptr && sp.ejRsp) {
        *tx = tap->eject;
        tap->eject_rdy = *txRdy;
    }
}

void Ring::buildDatChan(int i, In<Valid<DataFlit>>* rx, Out<bool>* rxRdy,
                        Out<Valid<DataFlit>>* tx, In<bool>* txRdy,
                        LaneEnds<DataFlit>& lane) {
    const StopSpec&               sp = kStopTable[i];
    ChannelTap<DataFlit, 3, true>* tap;
    Queue<DataFlit, 2>*           injq;
    buildChannel(i, lane, sp.injDat, sp.ejDat, tap, injq, "dat");
    if (injq != nullptr) {
        injq->enq = *rx;
        *rxRdy = injq->enq_rdy;
    }
    if (tap != nullptr && sp.ejDat) {
        *tx = tap->eject;
        tap->eject_rdy = *txRdy;
    }
}

void Ring::buildHrqChan(int i, LaneEnds<HrqFlit>& lane, In<Valid<HReqFlit>>* erqIn,
                        Out<bool>* erqInRdy, In<Valid<SnoopFlit>>* snpIn,
                        Out<bool>* snpInRdy, Out<Valid<HReqFlit>>* erqOut,
                        In<bool>* erqOutRdy, Out<Valid<SnoopFlit>>* snpOut,
                        In<bool>* snpOutRdy) {
    const StopSpec&               sp = kStopTable[i];
    const std::string             pfx = "n" + std::to_string(i) + "_";
    ChannelTap<HrqFlit, 5, false>* tap;
    Queue<HrqFlit, 2>*            injq;
    buildChannel(i, lane, sp.injHrq, sp.ejHrq, tap, injq, "hrq");
    if (injq != nullptr) {
        if (sp.hrqInjArb) {
            // HF：ERQ+SNP 经 ResetRRArbiter 合并（BaseRouter.scala:144-148）
            auto& arb = createChildModule<RRArb<HrqFlit, 2>>(pfx + "hrq_arb");
            arb.clk = clk;
            arb.in.assign().reads(*erqIn, *snpIn) = [](auto src) {
                auto [erq, snp] = src;
                std::array<Valid<HrqFlit>, 2> arr;
                arr[0].valid = erq.valid;
                arr[0].bits  = HrqFlit::fromHreq(erq.bits);
                arr[1].valid = snp.valid;
                arr[1].bits  = HrqFlit::fromSnp(snp.bits);
                return arr;
            };
            erqInRdy->assign().reads(arb.in_rdy) = [](auto src) { return std::get<0>(src)[0]; };
            snpInRdy->assign().reads(arb.in_rdy) = [](auto src) { return std::get<0>(src)[1]; };
            injq->enq = arb.out;
            arb.out_rdy.assign().reads(injq->enq_rdy) = [](auto src) { return std::get<0>(src); };
        } else {
            // HI：仅 ERQ 直连（BaseRouter.scala:149-150）
            injq->enq.assign().reads(*erqIn) = [](auto src) {
                auto [erq] = src;
                Valid<HrqFlit> d;
                d.valid = erq.valid;
                d.bits  = HrqFlit::fromHreq(erq.bits);
                return d;
            };
            *erqInRdy = injq->enq_rdy;
        }
    }
    if (tap != nullptr && sp.ejHrq) {
        if (snpOut != nullptr) {
            // CC：HRQ 车道 → SnoopFlit（RingFlit(128) 截断为 113b，字段 LSB 对齐）
            snpOut->assign().reads(tap->eject) = [](auto src) {
                auto [tap_eject] = src;
                Valid<SnoopFlit> d;
                d.valid = tap_eject.valid;
                d.bits  = tap_eject.bits.toSnp();
                return d;
            };
            tap->eject_rdy = *snpOutRdy;
        } else {
            // S：HRQ 车道 → HReqFlit（ERQ）
            erqOut->assign().reads(tap->eject) = [](auto src) {
                auto [tap_eject] = src;
                Valid<HReqFlit> d;
                d.valid = tap_eject.valid;
                d.bits  = tap_eject.bits.toHreq();
                return d;
            };
            tap->eject_rdy = *erqOutRdy;
        }
    }
}

Ring::Ring() {
    LaneEnds<RReqFlit> reqLane;
    LaneEnds<RespFlit> rspLane;
    LaneEnds<DataFlit> datLane;
    LaneEnds<HrqFlit>  hrqLane;

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

    // 建站
    for (int i = 0; i < 10; ++i) {
        const StopIO& io = stops[i];
        buildReqChan(i, io.rx_req, io.rx_req_rdy, io.tx_req, io.tx_req_rdy, reqLane);
        buildRspChan(i, io.rx_resp, io.rx_resp_rdy, io.tx_resp, io.tx_resp_rdy, rspLane);
        buildDatChan(i, io.rx_data, io.rx_data_rdy, io.tx_data, io.tx_data_rdy, datLane);
        buildHrqChan(i, hrqLane, io.rx_erq, io.rx_erq_rdy, io.rx_snoop, io.rx_snoop_rdy,
                     io.tx_erq, io.tx_erq_rdy, io.tx_snoop, io.tx_snoop_rdy);
    }

    // 链路（Ring.scala:23-26）：rings(0).rx ← 左邻 rings(0).tx；rings(1).rx ← 右邻
    for (int i = 0; i < 10; ++i) {
        const int l = (i + 9) % 10, r = (i + 1) % 10;
        *reqLane.rx0[i] = *reqLane.tx0[l];
        *reqLane.rx1[i] = *reqLane.tx1[r];
        *rspLane.rx0[i] = *rspLane.tx0[l];
        *rspLane.rx1[i] = *rspLane.tx1[r];
        *datLane.rx0[i] = *datLane.tx0[l];
        *datLane.rx1[i] = *datLane.tx1[r];
        *hrqLane.rx0[i] = *hrqLane.tx0[l];
        *hrqLane.rx1[i] = *hrqLane.tx1[r];
    }
}

}  // namespace zj::ring
