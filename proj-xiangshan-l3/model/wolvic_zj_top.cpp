#include "model/wolvic_zj_top.h"

#include <wolvicmod/wolvicmod.h>

namespace zj {

WolvicZjTop::WolvicZjTop() {
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

    // HomeShell ↔ DongJiang（hnx 边界）
    connHnx(shell0, hnf0);
    connHnx(shell1, hnf1);

    // RI(n3) tie-off 桩：XiangShan 侧 s_axi_main slave 常 0 → 无注入；
    // eject rdy 常 1（ZRING 裁剪语义）；eject valid/bits 悬空（无流量到达）
    ring.n3_rx_req = Valid<chi::RReqFlit>{};
    ring.n3_rx_resp = Valid<chi::RespFlit>{};
    ring.n3_rx_data = Valid<chi::DataFlit>{};
    ring.n3_tx_resp_rdy = true;
    ring.n3_tx_data_rdy = true;

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
    *ring.stops[4].rx_erq = Valid<chi::HReqFlit>{};  // AxiLiteBridge.scala:33
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
    hnf0.ci = ci;
    hnf1.ci = ci;
    hnf0.bank_id = uint8_t{0};
    hnf1.bank_id = uint8_t{1};
    snode.clk = clk;
    hinode.clk = clk;
    hinode.node_id = uint16_t{0x20};  // HI gid4 nodeId
}

void WolvicZjTop::connLan(ring::StopIO& r, home::LanIO& l) {
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

}  // namespace zj
