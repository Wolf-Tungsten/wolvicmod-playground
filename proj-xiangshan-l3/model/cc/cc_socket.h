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
// eject REQ 通道完整建模但 L2 侧由上层（WolvicZjTop，相当于 ZhuJiangBridge 的
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
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::sock {

using namespace zj::chi;

class CcSocket : public wolvicmod::Module {
public:
    IN(bool, clk);

    // ---- L2 侧（SocketDevSide io.icn）----
    IN(Valid<RReqFlit>, l2_rx_req);
    OUT(bool, l2_rx_req_rdy);
    IN(Valid<RespFlit>, l2_rx_resp);
    OUT(bool, l2_rx_resp_rdy);
    IN(Valid<DataFlit>, l2_rx_data);
    OUT(bool, l2_rx_data_rdy);
    OUT(Valid<RReqFlit>, l2_tx_req);
    IN(bool, l2_tx_req_rdy);
    OUT(Valid<RespFlit>, l2_tx_resp);
    IN(bool, l2_tx_resp_rdy);
    OUT(Valid<DataFlit>, l2_tx_data);
    IN(bool, l2_tx_data_rdy);
    OUT(Valid<SnoopFlit>, l2_tx_snoop);
    IN(bool, l2_tx_snoop_rdy);

    // ---- 环侧（SocketIcnSide io.dev）----
    OUT(Valid<RReqFlit>, ring_rx_req);
    IN(bool, ring_rx_req_rdy);
    OUT(Valid<RespFlit>, ring_rx_resp);
    IN(bool, ring_rx_resp_rdy);
    OUT(Valid<DataFlit>, ring_rx_data);
    IN(bool, ring_rx_data_rdy);
    IN(Valid<RReqFlit>, ring_tx_req);
    OUT(bool, ring_tx_req_rdy);
    IN(Valid<RespFlit>, ring_tx_resp);
    OUT(bool, ring_tx_resp_rdy);
    IN(Valid<DataFlit>, ring_tx_data);
    OUT(bool, ring_tx_data_rdy);
    IN(Valid<SnoopFlit>, ring_tx_snoop);
    OUT(bool, ring_tx_snoop_rdy);

    CcSocket();

private:
    // inject 通道（L2→环）：tx 在 dev 侧、rx 在 icn 侧
    MOD(PdcTx<RReqFlit>, ij_req_tx);
    MOD(PdcRx<RReqFlit>, ij_req_rx);
    MOD(PdcTx<RespFlit>, ij_rsp_tx);
    MOD(PdcRx<RespFlit>, ij_rsp_rx);
    MOD(PdcTx<DataFlit>, ij_dat_tx);
    MOD(PdcRx<DataFlit>, ij_dat_rx);
    // eject 通道（环→L2）：tx 在 icn 侧、rx 在 dev 侧
    MOD(PdcTx<RReqFlit>, ej_req_tx);
    MOD(PdcRx<RReqFlit>, ej_req_rx);
    MOD(PdcTx<RespFlit>, ej_rsp_tx);
    MOD(PdcRx<RespFlit>, ej_rsp_rx);
    MOD(PdcTx<DataFlit>, ej_dat_tx);
    MOD(PdcRx<DataFlit>, ej_dat_rx);
    MOD(PdcTx<SnoopFlit>, ej_snp_tx);
    MOD(PdcRx<SnoopFlit>, ej_snp_rx);

    // 单通道（两个方向结构相同，形参序即数据流向）：
    // src(decoupled 输入) → PdcTx →（PDC 线）→ PdcRx → dst(decoupled 输出)
    template <class F>
    void buildChan(In<Valid<F>>& src, Out<bool>& srcRdy, Out<Valid<F>>& dst, In<bool>& dstRdy,
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
