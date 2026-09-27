#pragma once

// ZjL3：P4a 阶段的部分组装顶层（WolvicZjTop 雏形，后续步骤在同一文件演进）：
//   L2 CHI 边界 → XscChiAdapter → CcSocket → Ring n1(CC)
//   Ring n0/n7 → HomeShell(bank0) → DongJiang；n2/n5 → HomeShell(bank1) → DongJiang
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
#include "model/home/dongjiang.h"
#include "model/home/home_shell.h"
#include "model/ring/ring.h"
#include "model/flit/xs_flit.h"
#include "model/flit/zj_flit.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"

namespace zj {

using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Valid;

class ZjL3 : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(uint8_t, ci);  // 单核恒 0

    // ---- L2 CHI 边界（xscache flit，DecoupledPortIO 六通道）----
    IN(Valid<xs::CHIREQ>, chi_tx_req);
    OUT(bool, chi_tx_req_rdy);
    IN(Valid<xs::CHIRSP>, chi_tx_rsp);
    OUT(bool, chi_tx_rsp_rdy);
    IN(Valid<xs::CHIDAT>, chi_tx_dat);
    OUT(bool, chi_tx_dat_rdy);
    OUT(Valid<xs::CHIRSP>, chi_rx_rsp);
    IN(bool, chi_rx_rsp_rdy);
    OUT(Valid<xs::CHIDAT>, chi_rx_dat);
    IN(bool, chi_rx_dat_rdy);
    OUT(Valid<xs::CHISNP>, chi_rx_snp);
    IN(bool, chi_rx_snp_rdy);

    // eject REQ 观察口（rdy 内部恒 false）
    OUT(Valid<chi::RReqFlit>, cc_tx_req);

    // ---- 未建模站点的环边界直通（RI n3 / HI n4 / S n6）----
    IN(Valid<chi::RReqFlit>, n3_rx_req);
    OUT(bool, n3_rx_req_rdy);
    IN(Valid<chi::RespFlit>, n3_rx_resp);
    OUT(bool, n3_rx_resp_rdy);
    IN(Valid<chi::DataFlit>, n3_rx_data);
    OUT(bool, n3_rx_data_rdy);
    OUT(Valid<chi::RespFlit>, n3_tx_resp);
    IN(bool, n3_tx_resp_rdy);
    OUT(Valid<chi::DataFlit>, n3_tx_data);
    IN(bool, n3_tx_data_rdy);

    // ---- memAXI（SNode 桥，id 6b/addr 48b/data 256b）----
    OUT(Valid<axi::AWFlit>, mem_aw);
    IN(bool, mem_aw_rdy);
    OUT(Valid<axi::WFlit>, mem_w);
    IN(bool, mem_w_rdy);
    OUT(Valid<axi::ARFlit>, mem_ar);
    IN(bool, mem_ar_rdy);
    IN(Valid<axi::BFlit>, mem_b);
    OUT(bool, mem_b_rdy);
    IN(Valid<axi::RFlit>, mem_r);
    OUT(bool, mem_r_rdy);

    // ---- cfgAXI（HiNode 桥，id 3b/addr 48b/data 256b）----
    OUT(Valid<axi::AWFlit>, cfg_aw);
    IN(bool, cfg_aw_rdy);
    OUT(Valid<axi::WFlit>, cfg_w);
    IN(bool, cfg_w_rdy);
    OUT(Valid<axi::ARFlit>, cfg_ar);
    IN(bool, cfg_ar_rdy);
    IN(Valid<axi::BFlit>, cfg_b);
    OUT(bool, cfg_b_rdy);
    IN(Valid<axi::RFlit>, cfg_r);
    OUT(bool, cfg_r_rdy);

    using HomeShellB0 = home::HomeShell<home::kHomeBank0>;
    using HomeShellB1 = home::HomeShell<home::kHomeBank1>;

    ZjL3();

    // 白盒访问（测试/调试）：hnf/shell 实例引用
    home::DongJiang& hnfAt(int i) { return i == 0 ? hnf0 : hnf1; }
    home::HomeShell<home::kHomeBank0>& shell0Ref() { return shell0; }
    home::HomeShell<home::kHomeBank1>& shell1Ref() { return shell1; }
    xs::XscChiAdapter& adapterRef() { return adapter; }
    sock::CcSocket& socketRef() { return cc_socket; }
    ring::Ring& ringRef() { return ring; }

private:
    MOD(xs::XscChiAdapter, adapter);
    MOD(sock::CcSocket, cc_socket);
    MOD(ring::Ring, ring);
    MOD(HomeShellB0, shell0);  // bank0：lan0=n0(gid0)、lan1=n7(gid7)
    MOD(HomeShellB1, shell1);  // bank1：lan0=n2(gid2)、lan1=n5(gid5)
    MOD(home::DongJiang, hnf0);
    MOD(home::DongJiang, hnf1);
    MOD(bridge::SNodeAxiBridge, snode);    // n6 → memAXI
    MOD(bridge::HiNodeAxiLiteBridge, hinode);  // n4 → cfgAXI

    // HomeShell lan ↔ Ring HF 站（端口索引视图接线；HF 站上这些字段由
    // kStopTable 保证非空）
    static void connLan(ring::StopIO& r, home::LanIO& l);

    template <class ShellT>
    void connHnx(ShellT& shell, home::DongJiang& hnf) {
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
