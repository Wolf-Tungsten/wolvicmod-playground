#pragma once

// ZjL3：P4a 阶段的部分组装顶层（WolvicZjTop 雏形，后续步骤在同一文件演进）：
//   L2 CHI 边界 → XscChiAdapter → CcSocket → Ring n1(CC)
//   Ring n0/n7 → HomeShell(bank0) → HnfStub；n2/n5 → HomeShell(bank1) → HnfStub
//   Ring n4(HI) → HiNodeAxiLiteBridge → cfgAXI；n6(S) → SNodeAxiBridge → memAXI
// 未建模站点（RI n3）的环边界端口直通外露（由测试 tie-off）。M/P 无环边界通道。
//
// ZhuJiangBridge 的 tie-off 角色（ZhuJiangBridge.scala:116-127,147-149）：
//   - eject REQ（l2_tx_req）：rdy 恒 false，valid/bits 外露 cc_tx_req 供观察
//     （RTL 中该通道存在但本集成永不消费；断言"不应有 REQ 到 CC"由测试检查）
//   - rx.hpr/snoop/debug、tx.hpr/debug：CC 节点这些通道不存在，天然无对应端口

#include <cstdint>

#include "model/bridge/hinode_axilite_bridge.h"
#include "model/bridge/snode_axi_bridge.h"
#include "model/cc/cc_socket.h"
#include "model/cc/xsc_chi_adapter.h"
#include "model/home/hnf_stub.h"
#include "model/home/home_shell.h"
#include "model/ring/ring.h"
#include "model/flit/xs_flit.h"
#include "model/flit/zj_flit.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/dec.h"

namespace zj {

using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Dec;

class ZjL3 : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(uint8_t, ci);  // 单核恒 0

    // ---- L2 CHI 边界（xscache flit，DecoupledPortIO 六通道）----
    IN(Dec<xs::CHIREQ>, chi_tx_req);
    OUT(bool, chi_tx_req_rdy);
    IN(Dec<xs::CHIRSP>, chi_tx_rsp);
    OUT(bool, chi_tx_rsp_rdy);
    IN(Dec<xs::CHIDAT>, chi_tx_dat);
    OUT(bool, chi_tx_dat_rdy);
    OUT(Dec<xs::CHIRSP>, chi_rx_rsp);
    IN(bool, chi_rx_rsp_rdy);
    OUT(Dec<xs::CHIDAT>, chi_rx_dat);
    IN(bool, chi_rx_dat_rdy);
    OUT(Dec<xs::CHISNP>, chi_rx_snp);
    IN(bool, chi_rx_snp_rdy);

    // eject REQ 观察口（rdy 内部恒 false）
    OUT(Dec<chi::RReqFlit>, cc_tx_req);

    // ---- 未建模站点的环边界直通（RI n3 / HI n4 / S n6）----
    IN(Dec<chi::RReqFlit>, n3_rx_req);
    OUT(bool, n3_rx_req_rdy);
    IN(Dec<chi::RespFlit>, n3_rx_resp);
    OUT(bool, n3_rx_resp_rdy);
    IN(Dec<chi::DataFlit>, n3_rx_data);
    OUT(bool, n3_rx_data_rdy);
    OUT(Dec<chi::RespFlit>, n3_tx_resp);
    IN(bool, n3_tx_resp_rdy);
    OUT(Dec<chi::DataFlit>, n3_tx_data);
    IN(bool, n3_tx_data_rdy);

    // ---- memAXI（SNode 桥，id 6b/addr 48b/data 256b）----
    OUT(Dec<axi::AWFlit>, mem_aw);
    IN(bool, mem_aw_rdy);
    OUT(Dec<axi::WFlit>, mem_w);
    IN(bool, mem_w_rdy);
    OUT(Dec<axi::ARFlit>, mem_ar);
    IN(bool, mem_ar_rdy);
    IN(Dec<axi::BFlit>, mem_b);
    OUT(bool, mem_b_rdy);
    IN(Dec<axi::RFlit>, mem_r);
    OUT(bool, mem_r_rdy);

    // ---- cfgAXI（HiNode 桥，id 3b/addr 48b/data 256b）----
    OUT(Dec<axi::AWFlit>, cfg_aw);
    IN(bool, cfg_aw_rdy);
    OUT(Dec<axi::WFlit>, cfg_w);
    IN(bool, cfg_w_rdy);
    OUT(Dec<axi::ARFlit>, cfg_ar);
    IN(bool, cfg_ar_rdy);
    IN(Dec<axi::BFlit>, cfg_b);
    OUT(bool, cfg_b_rdy);
    IN(Dec<axi::RFlit>, cfg_r);
    OUT(bool, cfg_r_rdy);

    using HomeShellB0 = home::HomeShell<home::kHomeBank0>;
    using HomeShellB1 = home::HomeShell<home::kHomeBank1>;

    SUB(xs::XscChiAdapter, adapter);
    SUB(sock::CcSocket, cc_socket);
    SUB(ring::Ring, ring);
    SUB(HomeShellB0, shell0);  // bank0：lan0=n0(gid0)、lan1=n7(gid7)
    SUB(HomeShellB1, shell1);  // bank1：lan0=n2(gid2)、lan1=n5(gid5)
    SUB(home::HnfStub, hnf0);
    SUB(home::HnfStub, hnf1);
    SUB(bridge::SNodeAxiBridge, snode);    // n6 → memAXI
    SUB(bridge::HiNodeAxiLiteBridge, hinode);  // n4 → cfgAXI

    ZjL3() {
        adapter.chi_tx_req = chi_tx_req;
        adapter.chi_tx_rsp = chi_tx_rsp;
        adapter.chi_tx_dat = chi_tx_dat;
        chi_tx_req_rdy = adapter.chi_tx_req_rdy;
        chi_tx_rsp_rdy = adapter.chi_tx_rsp_rdy;
        chi_tx_dat_rdy = adapter.chi_tx_dat_rdy;
        chi_rx_rsp = adapter.chi_rx_rsp;
        chi_rx_dat = adapter.chi_rx_dat;
        chi_rx_snp = adapter.chi_rx_snp;
        adapter.chi_rx_rsp_rdy = chi_rx_rsp_rdy;
        adapter.chi_rx_dat_rdy = chi_rx_dat_rdy;
        adapter.chi_rx_snp_rdy = chi_rx_snp_rdy;

        // adapter ↔ cc_socket（L2 侧）
        cc_socket.l2_rx_req = adapter.zj_rx_req;
        cc_socket.l2_rx_resp = adapter.zj_rx_rsp;
        cc_socket.l2_rx_data = adapter.zj_rx_dat;
        adapter.zj_rx_req_rdy = cc_socket.l2_rx_req_rdy;
        adapter.zj_rx_rsp_rdy = cc_socket.l2_rx_resp_rdy;
        adapter.zj_rx_dat_rdy = cc_socket.l2_rx_data_rdy;
        adapter.zj_tx_rsp = cc_socket.l2_tx_resp;
        adapter.zj_tx_dat = cc_socket.l2_tx_data;
        adapter.zj_tx_snp = cc_socket.l2_tx_snoop;
        cc_socket.l2_tx_resp_rdy = adapter.zj_tx_rsp_rdy;
        cc_socket.l2_tx_data_rdy = adapter.zj_tx_dat_rdy;
        cc_socket.l2_tx_snoop_rdy = adapter.zj_tx_snp_rdy;
        // eject REQ：观察口外露 + rdy 恒 false
        cc_tx_req = cc_socket.l2_tx_req;
        cc_socket.l2_tx_req_rdy = false;

        // cc_socket ↔ ring n1(CC)
        ring.n1_rx_req = cc_socket.ring_rx_req;
        ring.n1_rx_resp = cc_socket.ring_rx_resp;
        ring.n1_rx_data = cc_socket.ring_rx_data;
        cc_socket.ring_rx_req_rdy = ring.n1_rx_req_rdy;
        cc_socket.ring_rx_resp_rdy = ring.n1_rx_resp_rdy;
        cc_socket.ring_rx_data_rdy = ring.n1_rx_data_rdy;
        cc_socket.ring_tx_req = ring.n1_tx_req;
        cc_socket.ring_tx_resp = ring.n1_tx_resp;
        cc_socket.ring_tx_data = ring.n1_tx_data;
        cc_socket.ring_tx_snoop = ring.n1_tx_snoop;
        ring.n1_tx_req_rdy = cc_socket.ring_tx_req_rdy;
        ring.n1_tx_resp_rdy = cc_socket.ring_tx_resp_rdy;
        ring.n1_tx_data_rdy = cc_socket.ring_tx_data_rdy;
        ring.n1_tx_snoop_rdy = cc_socket.ring_tx_snoop_rdy;

        // ring HF 站 ↔ HomeShell lan（经端口索引视图）
        connLan(ring.stops[0], shell0.lan[0]);
        connLan(ring.stops[7], shell0.lan[1]);
        connLan(ring.stops[2], shell1.lan[0]);
        connLan(ring.stops[5], shell1.lan[1]);

        // HomeShell ↔ HnfStub（hnx 边界）
        connHnx(shell0, hnf0);
        connHnx(shell1, hnf1);

        // 未建模站点直通
        ring.n3_rx_req = n3_rx_req;
        ring.n3_rx_resp = n3_rx_resp;
        ring.n3_rx_data = n3_rx_data;
        n3_rx_req_rdy = ring.n3_rx_req_rdy;
        n3_rx_resp_rdy = ring.n3_rx_resp_rdy;
        n3_rx_data_rdy = ring.n3_rx_data_rdy;
        n3_tx_resp = ring.n3_tx_resp;
        n3_tx_data = ring.n3_tx_data;
        ring.n3_tx_resp_rdy = n3_tx_resp_rdy;
        ring.n3_tx_data_rdy = n3_tx_data_rdy;

        // ring n4(HI) ↔ HiNodeAxiLiteBridge（经 stops 视图；ERQ 恒 invalid）
        hinode.rx_req = *ring.stops[4].tx_req;
        *ring.stops[4].tx_req_rdy = hinode.rx_req_rdy;
        hinode.rx_resp = *ring.stops[4].tx_resp;
        *ring.stops[4].tx_resp_rdy = hinode.rx_resp_rdy;
        hinode.rx_data = *ring.stops[4].tx_data;
        *ring.stops[4].tx_data_rdy = hinode.rx_data_rdy;
        *ring.stops[4].rx_resp = hinode.tx_resp;
        hinode.tx_resp_rdy = *ring.stops[4].rx_resp_rdy;
        *ring.stops[4].rx_data = hinode.tx_data;
        hinode.tx_data_rdy = *ring.stops[4].rx_data_rdy;
        *ring.stops[4].rx_erq = Dec<chi::HReqFlit>{};  // AxiLiteBridge.scala:33
        // cfgAXI 外露
        cfg_aw = hinode.axi_aw;
        hinode.axi_aw_rdy = cfg_aw_rdy;
        cfg_w = hinode.axi_w;
        hinode.axi_w_rdy = cfg_w_rdy;
        cfg_ar = hinode.axi_ar;
        hinode.axi_ar_rdy = cfg_ar_rdy;
        hinode.axi_b = cfg_b;
        cfg_b_rdy = hinode.axi_b_rdy;
        hinode.axi_r = cfg_r;
        cfg_r_rdy = hinode.axi_r_rdy;

        // ring n6(S) ↔ SNodeAxiBridge（经 stops 视图）
        snode.rx_req = *ring.stops[6].tx_erq;
        *ring.stops[6].tx_erq_rdy = snode.rx_req_rdy;
        snode.rx_data = *ring.stops[6].tx_data;
        *ring.stops[6].tx_data_rdy = snode.rx_data_rdy;
        *ring.stops[6].rx_resp = snode.tx_resp;
        snode.tx_resp_rdy = *ring.stops[6].rx_resp_rdy;
        *ring.stops[6].rx_data = snode.tx_data;
        snode.tx_data_rdy = *ring.stops[6].rx_data_rdy;
        // memAXI 外露
        mem_aw = snode.axi_aw;
        snode.axi_aw_rdy = mem_aw_rdy;
        mem_w = snode.axi_w;
        snode.axi_w_rdy = mem_w_rdy;
        mem_ar = snode.axi_ar;
        snode.axi_ar_rdy = mem_ar_rdy;
        snode.axi_b = mem_b;
        mem_b_rdy = snode.axi_b_rdy;
        snode.axi_r = mem_r;
        mem_r_rdy = snode.axi_r_rdy;

        ring.ci = ci;
        shell0.ci = ci;
        shell1.ci = ci;
        cc_socket.clk = clk;
        ring.clk = clk;
        shell0.clk = clk;
        shell1.clk = clk;
        hnf0.clk = clk;
        hnf1.clk = clk;
        snode.clk = clk;
        hinode.clk = clk;
        hinode.node_id = uint16_t{0x20};  // HI gid4 nodeId
    }

private:
    // HomeShell lan ↔ Ring HF 站（端口索引视图接线；HF 站上这些字段由
    // kStopTable 保证非空）
    static void connLan(ring::StopIO& r, home::LanIO& l) {
        // eject（ring → shell）：ring nX_tx_* → shell lan_rx_*
        *l.rx_req = *r.tx_req;
        *l.rx_resp = *r.tx_resp;
        *l.rx_data = *r.tx_data;
        *r.tx_req_rdy = *l.rx_req_rdy;
        *r.tx_resp_rdy = *l.rx_resp_rdy;
        *r.tx_data_rdy = *l.rx_data_rdy;
        // inject（shell → ring）：shell lan_tx_* → ring nX_rx_*
        *r.rx_resp = *l.tx_resp;
        *r.rx_data = *l.tx_data;
        *r.rx_snoop = *l.tx_snoop;
        *r.rx_erq = *l.tx_erq;
        *l.tx_resp_rdy = *r.rx_resp_rdy;
        *l.tx_data_rdy = *r.rx_data_rdy;
        *l.tx_snoop_rdy = *r.rx_snoop_rdy;
        *l.tx_erq_rdy = *r.rx_erq_rdy;
    }

    template <class ShellT>
    void connHnx(ShellT& shell, home::HnfStub& hnf) {
        hnf.hnx_rx_req = shell.hnx_rx_req;
        hnf.hnx_rx_resp = shell.hnx_rx_resp;
        hnf.hnx_rx_data = shell.hnx_rx_data;
        shell.hnx_rx_req_rdy = hnf.hnx_rx_req_rdy;
        shell.hnx_rx_resp_rdy = hnf.hnx_rx_resp_rdy;
        shell.hnx_rx_data_rdy = hnf.hnx_rx_data_rdy;
        shell.hnx_tx_resp = hnf.hnx_tx_resp;
        shell.hnx_tx_data = hnf.hnx_tx_data;
        shell.hnx_tx_snoop = hnf.hnx_tx_snoop;
        shell.hnx_tx_erq = hnf.hnx_tx_erq;
        hnf.hnx_tx_resp_rdy = shell.hnx_tx_resp_rdy;
        hnf.hnx_tx_data_rdy = shell.hnx_tx_data_rdy;
        hnf.hnx_tx_snoop_rdy = shell.hnx_tx_snoop_rdy;
        hnf.hnx_tx_erq_rdy = shell.hnx_tx_erq_rdy;
    }
};

}  // namespace zj
