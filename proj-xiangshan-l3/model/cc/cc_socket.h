#pragma once

// CcSocket：CC 节点的完整 sync socket（dev 侧 ChiPdcDevSide + icn 侧
// ChiPdcIcnSide + PDC 线内部直连），对齐
// zhujiang/device/socket/{Socket.scala,PowerDomaincCrossing.scala}。
// CC 节点（Node.scala:139）：injects = REQ/RSP/DAT，ejects = REQ/RSP/DAT/SNP。
//
// 每通道 = PdcTx →（PDC 线）→ PdcRx：
//   inject（L2→环）：l2_rx_* →（dev 侧 syncToPdc）→（icn 侧 pdcToSync）→ ring_rx_*
//   eject（环→L2）：ring_tx_* →（icn 侧 syncToPdc）→（dev 侧 pdcToSync）→ l2_tx_*
// PDC 线即 Zhujiang 顶层的 ccnIO（Socket.scala:118-132 的 IcnPdcBundle ↔
// DevPdcBundle）；本模型不跨时钟域，两侧同 clk。
//
// eject REQ 通道完整建模但 L2 侧由上层（ZjL3，相当于 ZhuJiangBridge 的
// `tx.req.ready := false.B`，ZhuJiangBridge.scala:147）恒置不消费：token 耗尽后
// 反压经 PDC 传回环侧（ring_tx_req_rdy=0）——行为与 RTL 一致。
//
// 端口命名：l2_* 对齐 SocketDevSide io.icn（IcnBundle 语义：rx=inject 输入、
// tx=eject 输出）；ring_* 对齐 SocketIcnSide io.dev（DeviceIcnBundle 语义：
// rx=eject 输入、tx=inject 输出）。PDC 载荷在 RTL 中是打包 UInt，包/解包为
// 恒等重映射（splitFlit=true），模型直接携带 flit struct。

#include <cstdint>

#include "model/cc/pdc.h"
#include "model/flit/zj_flit.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/dec.h"

namespace zj::sock {

using namespace zj::chi;

class CcSocket : public wolvicmod::Module {
public:
    IN(bool, clk);

    // ---- L2 侧（SocketDevSide io.icn）----
    IN(Dec<RReqFlit>, l2_rx_req);
    OUT(bool, l2_rx_req_rdy);
    IN(Dec<RespFlit>, l2_rx_resp);
    OUT(bool, l2_rx_resp_rdy);
    IN(Dec<DataFlit>, l2_rx_data);
    OUT(bool, l2_rx_data_rdy);
    OUT(Dec<RReqFlit>, l2_tx_req);
    IN(bool, l2_tx_req_rdy);
    OUT(Dec<RespFlit>, l2_tx_resp);
    IN(bool, l2_tx_resp_rdy);
    OUT(Dec<DataFlit>, l2_tx_data);
    IN(bool, l2_tx_data_rdy);
    OUT(Dec<SnoopFlit>, l2_tx_snoop);
    IN(bool, l2_tx_snoop_rdy);

    // ---- 环侧（SocketIcnSide io.dev）----
    OUT(Dec<RReqFlit>, ring_rx_req);
    IN(bool, ring_rx_req_rdy);
    OUT(Dec<RespFlit>, ring_rx_resp);
    IN(bool, ring_rx_resp_rdy);
    OUT(Dec<DataFlit>, ring_rx_data);
    IN(bool, ring_rx_data_rdy);
    IN(Dec<RReqFlit>, ring_tx_req);
    OUT(bool, ring_tx_req_rdy);
    IN(Dec<RespFlit>, ring_tx_resp);
    OUT(bool, ring_tx_resp_rdy);
    IN(Dec<DataFlit>, ring_tx_data);
    OUT(bool, ring_tx_data_rdy);
    IN(Dec<SnoopFlit>, ring_tx_snoop);
    OUT(bool, ring_tx_snoop_rdy);

    // inject 通道（L2→环）：tx 在 dev 侧、rx 在 icn 侧
    SUB(PdcTx<RReqFlit>, ij_req_tx);
    SUB(PdcRx<RReqFlit>, ij_req_rx);
    SUB(PdcTx<RespFlit>, ij_rsp_tx);
    SUB(PdcRx<RespFlit>, ij_rsp_rx);
    SUB(PdcTx<DataFlit>, ij_dat_tx);
    SUB(PdcRx<DataFlit>, ij_dat_rx);
    // eject 通道（环→L2）：tx 在 icn 侧、rx 在 dev 侧
    SUB(PdcTx<RReqFlit>, ej_req_tx);
    SUB(PdcRx<RReqFlit>, ej_req_rx);
    SUB(PdcTx<RespFlit>, ej_rsp_tx);
    SUB(PdcRx<RespFlit>, ej_rsp_rx);
    SUB(PdcTx<DataFlit>, ej_dat_tx);
    SUB(PdcRx<DataFlit>, ej_dat_rx);
    SUB(PdcTx<SnoopFlit>, ej_snp_tx);
    SUB(PdcRx<SnoopFlit>, ej_snp_rx);

    CcSocket() {
        buildChan(l2_rx_req, l2_rx_req_rdy, ring_rx_req, ring_rx_req_rdy, ij_req_tx, ij_req_rx);
        buildChan(l2_rx_resp, l2_rx_resp_rdy, ring_rx_resp, ring_rx_resp_rdy, ij_rsp_tx, ij_rsp_rx);
        buildChan(l2_rx_data, l2_rx_data_rdy, ring_rx_data, ring_rx_data_rdy, ij_dat_tx, ij_dat_rx);
        buildChan(ring_tx_req, ring_tx_req_rdy, l2_tx_req, l2_tx_req_rdy, ej_req_tx, ej_req_rx);
        buildChan(ring_tx_resp, ring_tx_resp_rdy, l2_tx_resp, l2_tx_resp_rdy, ej_rsp_tx, ej_rsp_rx);
        buildChan(ring_tx_data, ring_tx_data_rdy, l2_tx_data, l2_tx_data_rdy, ej_dat_tx, ej_dat_rx);
        buildChan(ring_tx_snoop, ring_tx_snoop_rdy, l2_tx_snoop, l2_tx_snoop_rdy, ej_snp_tx, ej_snp_rx);
    }

private:
    // 单通道（两个方向结构相同，形参序即数据流向）：
    // src(decoupled 输入) → PdcTx →（PDC 线）→ PdcRx → dst(decoupled 输出)
    template <class F>
    void buildChan(In<Dec<F>>& src, Out<bool>& srcRdy, Out<Dec<F>>& dst, In<bool>& dstRdy,
                   PdcTx<F>& tx, PdcRx<F>& rx) {
        tx.clk = clk;
        rx.clk = clk;
        tx.enq = src;
        srcRdy = tx.enq_rdy;
        rx.pdc = tx.pdc;
        tx.pdc_grant = rx.pdc_grant;
        dst = rx.deq;
        rx.deq_rdy = dstRdy;
    }
};

}  // namespace zj::sock
