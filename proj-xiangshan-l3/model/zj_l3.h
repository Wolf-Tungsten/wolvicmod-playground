#pragma once

// ZjL3：P2 阶段的部分组装顶层（WolvicZjTop 雏形，后续步骤在同一文件演进）：
//   L2 CHI 边界 → XscChiAdapter → CcSocket → Ring n1(CC)
//   Ring n0/n7 → HomeShell(bank0) → HnfStub；n2/n5 → HomeShell(bank1) → HnfStub
// 未建模站点（RI n3 / HI n4 / S n6）的环边界端口直通外露（P4 接
// SNode/HiNode bridge；当前由测试 tie-off）。M/P 无环边界通道。
//
// ZhuJiangBridge 的 tie-off 角色（ZhuJiangBridge.scala:116-127,147-149）：
//   - eject REQ（l2_tx_req）：rdy 恒 false，valid/bits 外露 cc_tx_req 供观察
//     （RTL 中该通道存在但本集成永不消费；断言"不应有 REQ 到 CC"由测试检查）
//   - rx.hpr/snoop/debug、tx.hpr/debug：CC 节点这些通道不存在，天然无对应端口

#include <cstdint>

#include "model/cc/cc_socket.h"
#include "model/cc/xsc_chi_adapter.h"
#include "model/home/hnf_stub.h"
#include "model/home/home_shell.h"
#include "model/ring/ring.h"
#include "model/wire_conn.h"
#include "model/xs_flit.h"
#include "model/zj_flit.h"
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

    OUT(Dec<chi::RReqFlit>, n4_tx_req);
    IN(bool, n4_tx_req_rdy);
    OUT(Dec<chi::RespFlit>, n4_tx_resp);
    IN(bool, n4_tx_resp_rdy);
    OUT(Dec<chi::DataFlit>, n4_tx_data);
    IN(bool, n4_tx_data_rdy);
    IN(Dec<chi::HReqFlit>, n4_rx_req);
    OUT(bool, n4_rx_req_rdy);
    IN(Dec<chi::RespFlit>, n4_rx_resp);
    OUT(bool, n4_rx_resp_rdy);
    IN(Dec<chi::DataFlit>, n4_rx_data);
    OUT(bool, n4_rx_data_rdy);

    IN(Dec<chi::RespFlit>, n6_rx_resp);
    OUT(bool, n6_rx_resp_rdy);
    IN(Dec<chi::DataFlit>, n6_rx_data);
    OUT(bool, n6_rx_data_rdy);
    OUT(Dec<chi::HReqFlit>, n6_tx_req);
    IN(bool, n6_tx_req_rdy);
    OUT(Dec<chi::DataFlit>, n6_tx_data);
    IN(bool, n6_tx_data_rdy);

    using HomeShellB0 = home::HomeShell<home::kHomeBank0>;
    using HomeShellB1 = home::HomeShell<home::kHomeBank1>;

    SUB(xs::XscChiAdapter, adapter);
    SUB(sock::CcSocket, cc_socket);
    SUB(ring::Ring, ring);
    SUB(HomeShellB0, shell0);  // bank0：lan0=n0(gid0)、lan1=n7(gid7)
    SUB(HomeShellB1, shell1);  // bank1：lan0=n2(gid2)、lan1=n5(gid5)
    SUB(home::HnfStub, hnf0);
    SUB(home::HnfStub, hnf1);

    ZjL3() {
        adapter.chi_tx_req = chi_tx_req;
        adapter.chi_tx_rsp = chi_tx_rsp;
        adapter.chi_tx_dat = chi_tx_dat;
        detail::wireConn(chi_tx_req_rdy, adapter.chi_tx_req_rdy);
        detail::wireConn(chi_tx_rsp_rdy, adapter.chi_tx_rsp_rdy);
        detail::wireConn(chi_tx_dat_rdy, adapter.chi_tx_dat_rdy);
        detail::wireConn(chi_rx_rsp, adapter.chi_rx_rsp);
        detail::wireConn(chi_rx_dat, adapter.chi_rx_dat);
        detail::wireConn(chi_rx_snp, adapter.chi_rx_snp);
        adapter.chi_rx_rsp_rdy = chi_rx_rsp_rdy;
        adapter.chi_rx_dat_rdy = chi_rx_dat_rdy;
        adapter.chi_rx_snp_rdy = chi_rx_snp_rdy;

        // adapter ↔ cc_socket（L2 侧）
        detail::wireConn(cc_socket.l2_rx_req, adapter.zj_rx_req);
        detail::wireConn(cc_socket.l2_rx_resp, adapter.zj_rx_rsp);
        detail::wireConn(cc_socket.l2_rx_data, adapter.zj_rx_dat);
        detail::wireConn(adapter.zj_rx_req_rdy, cc_socket.l2_rx_req_rdy);
        detail::wireConn(adapter.zj_rx_rsp_rdy, cc_socket.l2_rx_resp_rdy);
        detail::wireConn(adapter.zj_rx_dat_rdy, cc_socket.l2_rx_data_rdy);
        detail::wireConn(adapter.zj_tx_rsp, cc_socket.l2_tx_resp);
        detail::wireConn(adapter.zj_tx_dat, cc_socket.l2_tx_data);
        detail::wireConn(adapter.zj_tx_snp, cc_socket.l2_tx_snoop);
        detail::wireConn(cc_socket.l2_tx_resp_rdy, adapter.zj_tx_rsp_rdy);
        detail::wireConn(cc_socket.l2_tx_data_rdy, adapter.zj_tx_dat_rdy);
        detail::wireConn(cc_socket.l2_tx_snoop_rdy, adapter.zj_tx_snp_rdy);
        // eject REQ：观察口外露 + rdy 恒 false
        detail::wireConn(cc_tx_req, cc_socket.l2_tx_req);
        cc_socket.l2_tx_req_rdy = false;

        // cc_socket ↔ ring n1(CC)
        detail::wireConn(ring.n1_rx_req, cc_socket.ring_rx_req);
        detail::wireConn(ring.n1_rx_resp, cc_socket.ring_rx_resp);
        detail::wireConn(ring.n1_rx_data, cc_socket.ring_rx_data);
        detail::wireConn(cc_socket.ring_rx_req_rdy, ring.n1_rx_req_rdy);
        detail::wireConn(cc_socket.ring_rx_resp_rdy, ring.n1_rx_resp_rdy);
        detail::wireConn(cc_socket.ring_rx_data_rdy, ring.n1_rx_data_rdy);
        detail::wireConn(cc_socket.ring_tx_req, ring.n1_tx_req);
        detail::wireConn(cc_socket.ring_tx_resp, ring.n1_tx_resp);
        detail::wireConn(cc_socket.ring_tx_data, ring.n1_tx_data);
        detail::wireConn(cc_socket.ring_tx_snoop, ring.n1_tx_snoop);
        detail::wireConn(ring.n1_tx_req_rdy, cc_socket.ring_tx_req_rdy);
        detail::wireConn(ring.n1_tx_resp_rdy, cc_socket.ring_tx_resp_rdy);
        detail::wireConn(ring.n1_tx_data_rdy, cc_socket.ring_tx_data_rdy);
        detail::wireConn(ring.n1_tx_snoop_rdy, cc_socket.ring_tx_snoop_rdy);

        // ring HF 站 ↔ HomeShell lan
        connLan(shell0, 0, ring.n0_tx_req, ring.n0_tx_req_rdy, ring.n0_tx_resp, ring.n0_tx_resp_rdy,
                ring.n0_tx_data, ring.n0_tx_data_rdy, ring.n0_rx_resp, ring.n0_rx_resp_rdy,
                ring.n0_rx_data, ring.n0_rx_data_rdy, ring.n0_rx_snoop, ring.n0_rx_snoop_rdy,
                ring.n0_rx_req, ring.n0_rx_req_rdy);
        connLan(shell0, 1, ring.n7_tx_req, ring.n7_tx_req_rdy, ring.n7_tx_resp, ring.n7_tx_resp_rdy,
                ring.n7_tx_data, ring.n7_tx_data_rdy, ring.n7_rx_resp, ring.n7_rx_resp_rdy,
                ring.n7_rx_data, ring.n7_rx_data_rdy, ring.n7_rx_snoop, ring.n7_rx_snoop_rdy,
                ring.n7_rx_req, ring.n7_rx_req_rdy);
        connLan(shell1, 0, ring.n2_tx_req, ring.n2_tx_req_rdy, ring.n2_tx_resp, ring.n2_tx_resp_rdy,
                ring.n2_tx_data, ring.n2_tx_data_rdy, ring.n2_rx_resp, ring.n2_rx_resp_rdy,
                ring.n2_rx_data, ring.n2_rx_data_rdy, ring.n2_rx_snoop, ring.n2_rx_snoop_rdy,
                ring.n2_rx_req, ring.n2_rx_req_rdy);
        connLan(shell1, 1, ring.n5_tx_req, ring.n5_tx_req_rdy, ring.n5_tx_resp, ring.n5_tx_resp_rdy,
                ring.n5_tx_data, ring.n5_tx_data_rdy, ring.n5_rx_resp, ring.n5_rx_resp_rdy,
                ring.n5_rx_data, ring.n5_rx_data_rdy, ring.n5_rx_snoop, ring.n5_rx_snoop_rdy,
                ring.n5_rx_req, ring.n5_rx_req_rdy);

        // HomeShell ↔ HnfStub（hnx 边界）
        connHnx(shell0, hnf0);
        connHnx(shell1, hnf1);

        // 未建模站点直通
        ring.n3_rx_req = n3_rx_req;
        ring.n3_rx_resp = n3_rx_resp;
        ring.n3_rx_data = n3_rx_data;
        detail::wireConn(n3_rx_req_rdy, ring.n3_rx_req_rdy);
        detail::wireConn(n3_rx_resp_rdy, ring.n3_rx_resp_rdy);
        detail::wireConn(n3_rx_data_rdy, ring.n3_rx_data_rdy);
        detail::wireConn(n3_tx_resp, ring.n3_tx_resp);
        detail::wireConn(n3_tx_data, ring.n3_tx_data);
        ring.n3_tx_resp_rdy = n3_tx_resp_rdy;
        ring.n3_tx_data_rdy = n3_tx_data_rdy;

        ring.n4_rx_req = n4_rx_req;
        ring.n4_rx_resp = n4_rx_resp;
        ring.n4_rx_data = n4_rx_data;
        detail::wireConn(n4_rx_req_rdy, ring.n4_rx_req_rdy);
        detail::wireConn(n4_rx_resp_rdy, ring.n4_rx_resp_rdy);
        detail::wireConn(n4_rx_data_rdy, ring.n4_rx_data_rdy);
        detail::wireConn(n4_tx_req, ring.n4_tx_req);
        detail::wireConn(n4_tx_resp, ring.n4_tx_resp);
        detail::wireConn(n4_tx_data, ring.n4_tx_data);
        ring.n4_tx_req_rdy = n4_tx_req_rdy;
        ring.n4_tx_resp_rdy = n4_tx_resp_rdy;
        ring.n4_tx_data_rdy = n4_tx_data_rdy;

        ring.n6_rx_resp = n6_rx_resp;
        ring.n6_rx_data = n6_rx_data;
        detail::wireConn(n6_rx_resp_rdy, ring.n6_rx_resp_rdy);
        detail::wireConn(n6_rx_data_rdy, ring.n6_rx_data_rdy);
        detail::wireConn(n6_tx_req, ring.n6_tx_req);
        detail::wireConn(n6_tx_data, ring.n6_tx_data);
        ring.n6_tx_req_rdy = n6_tx_req_rdy;
        ring.n6_tx_data_rdy = n6_tx_data_rdy;

        ring.ci = ci;
        shell0.ci = ci;
        shell1.ci = ci;
        cc_socket.clk = clk;
        ring.clk = clk;
        shell0.clk = clk;
        shell1.clk = clk;
        hnf0.clk = clk;
        hnf1.clk = clk;
    }

private:
    // HomeShell lan ↔ Ring HF 站（lanIdx 0/1；ring 侧端口由调用方按站传入）
    template <class ShellT>
    void connLan(ShellT& shell, int lanIdx, Out<Dec<chi::RReqFlit>>& r_tx_req,
                 In<bool>& r_tx_req_rdy, Out<Dec<chi::RespFlit>>& r_tx_resp,
                 In<bool>& r_tx_resp_rdy, Out<Dec<chi::DataFlit>>& r_tx_data,
                 In<bool>& r_tx_data_rdy, In<Dec<chi::RespFlit>>& r_rx_resp,
                 Out<bool>& r_rx_resp_rdy, In<Dec<chi::DataFlit>>& r_rx_data,
                 Out<bool>& r_rx_data_rdy, In<Dec<chi::SnoopFlit>>& r_rx_snoop,
                 Out<bool>& r_rx_snoop_rdy, In<Dec<chi::HReqFlit>>& r_rx_erq,
                 Out<bool>& r_rx_erq_rdy) {
        // eject（ring → shell）：ring nX_tx_* → shell lan_rx_*
        auto& l_rx_req  = lanIdx == 0 ? shell.lan0_rx_req : shell.lan1_rx_req;
        auto& l_rx_req_rdy =
            lanIdx == 0 ? shell.lan0_rx_req_rdy : shell.lan1_rx_req_rdy;
        auto& l_rx_resp = lanIdx == 0 ? shell.lan0_rx_resp : shell.lan1_rx_resp;
        auto& l_rx_resp_rdy =
            lanIdx == 0 ? shell.lan0_rx_resp_rdy : shell.lan1_rx_resp_rdy;
        auto& l_rx_data = lanIdx == 0 ? shell.lan0_rx_data : shell.lan1_rx_data;
        auto& l_rx_data_rdy =
            lanIdx == 0 ? shell.lan0_rx_data_rdy : shell.lan1_rx_data_rdy;
        // inject（shell → ring）：shell lan_tx_* → ring nX_rx_*
        auto& l_tx_resp = lanIdx == 0 ? shell.lan0_tx_resp : shell.lan1_tx_resp;
        auto& l_tx_resp_rdy =
            lanIdx == 0 ? shell.lan0_tx_resp_rdy : shell.lan1_tx_resp_rdy;
        auto& l_tx_data = lanIdx == 0 ? shell.lan0_tx_data : shell.lan1_tx_data;
        auto& l_tx_data_rdy =
            lanIdx == 0 ? shell.lan0_tx_data_rdy : shell.lan1_tx_data_rdy;
        auto& l_tx_snoop = lanIdx == 0 ? shell.lan0_tx_snoop : shell.lan1_tx_snoop;
        auto& l_tx_snoop_rdy =
            lanIdx == 0 ? shell.lan0_tx_snoop_rdy : shell.lan1_tx_snoop_rdy;
        auto& l_tx_erq = lanIdx == 0 ? shell.lan0_tx_erq : shell.lan1_tx_erq;
        auto& l_tx_erq_rdy =
            lanIdx == 0 ? shell.lan0_tx_erq_rdy : shell.lan1_tx_erq_rdy;

        detail::wireConn(l_rx_req, r_tx_req);
        detail::wireConn(l_rx_resp, r_tx_resp);
        detail::wireConn(l_rx_data, r_tx_data);
        detail::wireConn(r_tx_req_rdy, l_rx_req_rdy);
        detail::wireConn(r_tx_resp_rdy, l_rx_resp_rdy);
        detail::wireConn(r_tx_data_rdy, l_rx_data_rdy);
        detail::wireConn(r_rx_resp, l_tx_resp);
        detail::wireConn(r_rx_data, l_tx_data);
        detail::wireConn(r_rx_snoop, l_tx_snoop);
        detail::wireConn(r_rx_erq, l_tx_erq);
        detail::wireConn(l_tx_resp_rdy, r_rx_resp_rdy);
        detail::wireConn(l_tx_data_rdy, r_rx_data_rdy);
        detail::wireConn(l_tx_snoop_rdy, r_rx_snoop_rdy);
        detail::wireConn(l_tx_erq_rdy, r_rx_erq_rdy);
    }

    template <class ShellT>
    void connHnx(ShellT& shell, home::HnfStub& hnf) {
        detail::wireConn(hnf.hnx_rx_req, shell.hnx_rx_req);
        detail::wireConn(hnf.hnx_rx_resp, shell.hnx_rx_resp);
        detail::wireConn(hnf.hnx_rx_data, shell.hnx_rx_data);
        detail::wireConn(shell.hnx_rx_req_rdy, hnf.hnx_rx_req_rdy);
        detail::wireConn(shell.hnx_rx_resp_rdy, hnf.hnx_rx_resp_rdy);
        detail::wireConn(shell.hnx_rx_data_rdy, hnf.hnx_rx_data_rdy);
        detail::wireConn(shell.hnx_tx_resp, hnf.hnx_tx_resp);
        detail::wireConn(shell.hnx_tx_data, hnf.hnx_tx_data);
        detail::wireConn(shell.hnx_tx_snoop, hnf.hnx_tx_snoop);
        detail::wireConn(shell.hnx_tx_erq, hnf.hnx_tx_erq);
        detail::wireConn(hnf.hnx_tx_resp_rdy, shell.hnx_tx_resp_rdy);
        detail::wireConn(hnf.hnx_tx_data_rdy, shell.hnx_tx_data_rdy);
        detail::wireConn(hnf.hnx_tx_snoop_rdy, shell.hnx_tx_snoop_rdy);
        detail::wireConn(hnf.hnx_tx_erq_rdy, shell.hnx_tx_erq_rdy);
    }
};

}  // namespace zj
