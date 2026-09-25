#pragma once

// HomeShell：HomeWrapper 外壳（DongJiang 内部在 P2 用行为桩替代，见
// hnf_stub.h），对齐 zhujiang/device/home/HomeWrapper.scala。
// kunminghu-v3 单核：hnxPipelineDepth=0 → 每 lan 恰好 1 级 ChiBuffer
// （每通道 Queue(flit, entries=2, pipe=false)）；每 bank 1 实例、2 lan。
//
// 结构（HomeWrapper.scala:45-145，剥离 DongJiang/时钟门控/mbist/perf 后）：
//   eject（环→hnx，REQ/RSP/DAT）：lan_rx_* → ChiBuffer 队列 →
//     ResetRRArbiter(2) 合流 → hnx_rx_*（:80-89；ResetRRArbiter 语义 =
//     chisel RRArbiter，见 ring.h as-built 备注，用 prefab::RRArb）
//   inject（hnx→环，RSP/DAT/SNP/ERQ）：hnx_tx_* → friends 分发（组合）
//     → ChiBuffer 队列 → lan_tx_*（:93-143）
//
// friends 分发（:90-143）：
//   tgt：ERQ → 按 mems（island 内 S 节点）addrCheck 选址（本配置单 SN 0x30、
//     addrSets=[(0,0)] 全匹配 → tgt = addr.ci==ci ? 0x30 : 0）；其余 →
//     flit.TgtID 直通
//   friendsHitVec(i) = tgt.router ∈ friends[i]（router = nid<<3，nodeId 低位
//     aid 清零比较）；srcId = Mux1H(hit, nids)
//   字段改写：ERQ → TgtID:=tgt、ReturnNID:=noDmt?srcId:保持（noDmt =
//     ReturnNID 全 1，:117）；DAT → TgtID:=tgt、HomeNID:=srcId；RSP/SNP →
//     TgtID:=tgt
//   lan_q(i).valid = hnx.valid && hit(i)；hnx.ready = |（hit(i) && q(i).ready）
//     （Mux1H 语义；无命中 → 全 0 反压停住，与 RTL HAssert 场景一致）
//
// friends/nids 在 RTL 顶层是 elaboration 常量（Zhujiang.scala:99-104），
// 本模型作构造参数；ci 为运行时端口（顶层 IO，单核恒 0）。

#include <array>
#include <cstdint>

#include "model/wire_conn.h"
#include "model/zj_flit.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/arb.h"
#include "wolvicmod/prefab/dec.h"
#include "wolvicmod/prefab/queue.h"

namespace zj::home {

using namespace zj::chi;
using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Dec;
using wolvicmod::prefab::Queue;
using wolvicmod::prefab::RRArb;

// HomeShell 配置（elaboration 常量，ZJParameters.scala:137-158  friends 推导）
struct HomeShellCfg {
    uint8_t                              bank = 0;
    std::array<uint16_t, 2>              nids{};     // 每 lan 自身 nodeId
    std::array<std::array<uint16_t, 2>, 2> friends{};  // 每 lan 的 friend nodeId
    uint16_t mem_nid = 0x30;  // 单 SN（gid6）；addrSets 全匹配，仅判 addr.ci==ci
};

// 站序（ring.h 头注释）：bank0 lans = [gid0(hfp0), gid7(hfp1)]，
//   friends = [[CC 0x08, RI 0x18], [M 0x40, S 0x30]]
// bank1 lans = [gid2(hfp0), gid5(hfp1)]，
//   friends = [[RI 0x18, CC 0x08], [S 0x30, M 0x40]]
inline constexpr HomeShellCfg kHomeBank0 = {0, {0x00, 0x38},
                                            {{{0x08, 0x18}, {0x40, 0x30}}}, 0x30};
inline constexpr HomeShellCfg kHomeBank1 = {1, {0x10, 0x28},
                                            {{{0x18, 0x08}, {0x30, 0x40}}}, 0x30};

// Cfg 为 NTTP（createChildModule 仅支持默认构造；HomeShellCfg 是 structural
// type，配置等价于 RTL elaboration 常量）
template <HomeShellCfg Cfg>
class HomeShell : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(uint8_t, ci);  // io.ci（4b，单核恒 0）

    // ---- 环侧（io.lans，DeviceIcnBundle：rx=eject 输入、tx=inject 输出）----
    IN(Dec<RReqFlit>, lan0_rx_req);
    OUT(bool, lan0_rx_req_rdy);
    IN(Dec<RespFlit>, lan0_rx_resp);
    OUT(bool, lan0_rx_resp_rdy);
    IN(Dec<DataFlit>, lan0_rx_data);
    OUT(bool, lan0_rx_data_rdy);
    OUT(Dec<RespFlit>, lan0_tx_resp);
    IN(bool, lan0_tx_resp_rdy);
    OUT(Dec<DataFlit>, lan0_tx_data);
    IN(bool, lan0_tx_data_rdy);
    OUT(Dec<SnoopFlit>, lan0_tx_snoop);
    IN(bool, lan0_tx_snoop_rdy);
    OUT(Dec<HReqFlit>, lan0_tx_erq);
    IN(bool, lan0_tx_erq_rdy);

    IN(Dec<RReqFlit>, lan1_rx_req);
    OUT(bool, lan1_rx_req_rdy);
    IN(Dec<RespFlit>, lan1_rx_resp);
    OUT(bool, lan1_rx_resp_rdy);
    IN(Dec<DataFlit>, lan1_rx_data);
    OUT(bool, lan1_rx_data_rdy);
    OUT(Dec<RespFlit>, lan1_tx_resp);
    IN(bool, lan1_tx_resp_rdy);
    OUT(Dec<DataFlit>, lan1_tx_data);
    IN(bool, lan1_tx_data_rdy);
    OUT(Dec<SnoopFlit>, lan1_tx_snoop);
    IN(bool, lan1_tx_snoop_rdy);
    OUT(Dec<HReqFlit>, lan1_tx_erq);
    IN(bool, lan1_tx_erq_rdy);

    // ---- hnx 侧（DongJiang io.lan：rx=eject 输出、tx=inject 输入）----
    OUT(Dec<RReqFlit>, hnx_rx_req);
    IN(bool, hnx_rx_req_rdy);
    OUT(Dec<RespFlit>, hnx_rx_resp);
    IN(bool, hnx_rx_resp_rdy);
    OUT(Dec<DataFlit>, hnx_rx_data);
    IN(bool, hnx_rx_data_rdy);
    IN(Dec<RespFlit>, hnx_tx_resp);
    OUT(bool, hnx_tx_resp_rdy);
    IN(Dec<DataFlit>, hnx_tx_data);
    OUT(bool, hnx_tx_data_rdy);
    IN(Dec<SnoopFlit>, hnx_tx_snoop);
    OUT(bool, hnx_tx_snoop_rdy);
    IN(Dec<HReqFlit>, hnx_tx_erq);
    OUT(bool, hnx_tx_erq_rdy);

    using ReqQ  = Queue<RReqFlit, 2>;
    using RspQ  = Queue<RespFlit, 2>;
    using DatQ  = Queue<DataFlit, 2>;
    using SnpQ  = Queue<SnoopFlit, 2>;
    using ErqQ  = Queue<HReqFlit, 2>;
    using ReqArb = RRArb<RReqFlit, 2>;
    using RspArb = RRArb<RespFlit, 2>;
    using DatArb = RRArb<DataFlit, 2>;

    // ChiBuffer 队列（每 lan：eject 3 + inject 4）
    SUB(ReqQ, lan0_ej_req_q);
    SUB(RspQ, lan0_ej_rsp_q);
    SUB(DatQ, lan0_ej_dat_q);
    SUB(ReqQ, lan1_ej_req_q);
    SUB(RspQ, lan1_ej_rsp_q);
    SUB(DatQ, lan1_ej_dat_q);
    SUB(RspQ, lan0_ij_rsp_q);
    SUB(DatQ, lan0_ij_dat_q);
    SUB(SnpQ, lan0_ij_snp_q);
    SUB(ErqQ, lan0_ij_erq_q);
    SUB(RspQ, lan1_ij_rsp_q);
    SUB(DatQ, lan1_ij_dat_q);
    SUB(SnpQ, lan1_ij_snp_q);
    SUB(ErqQ, lan1_ij_erq_q);
    // eject 合流仲裁
    SUB(ReqArb, arb_req);
    SUB(RspArb, arb_rsp);
    SUB(DatArb, arb_dat);

    HomeShell() {
        lan0_ej_req_q.clk = clk; lan0_ej_rsp_q.clk = clk; lan0_ej_dat_q.clk = clk;
        lan1_ej_req_q.clk = clk; lan1_ej_rsp_q.clk = clk; lan1_ej_dat_q.clk = clk;
        lan0_ij_rsp_q.clk = clk; lan0_ij_dat_q.clk = clk;
        lan0_ij_snp_q.clk = clk; lan0_ij_erq_q.clk = clk;
        lan1_ij_rsp_q.clk = clk; lan1_ij_dat_q.clk = clk;
        lan1_ij_snp_q.clk = clk; lan1_ij_erq_q.clk = clk;
        arb_req.clk = clk; arb_rsp.clk = clk; arb_dat.clk = clk;

        // ---- eject：lan_rx_* → q → arb → hnx_rx_* ----
        buildEjArb(arb_req, lan0_ej_req_q, lan1_ej_req_q);
        buildEjArb(arb_rsp, lan0_ej_rsp_q, lan1_ej_rsp_q);
        buildEjArb(arb_dat, lan0_ej_dat_q, lan1_ej_dat_q);
        buildEjLan(lan0_rx_req, lan0_rx_req_rdy, lan0_ej_req_q, arb_req, 0);
        buildEjLan(lan1_rx_req, lan1_rx_req_rdy, lan1_ej_req_q, arb_req, 1);
        buildEjLan(lan0_rx_resp, lan0_rx_resp_rdy, lan0_ej_rsp_q, arb_rsp, 0);
        buildEjLan(lan1_rx_resp, lan1_rx_resp_rdy, lan1_ej_rsp_q, arb_rsp, 1);
        buildEjLan(lan0_rx_data, lan0_rx_data_rdy, lan0_ej_dat_q, arb_dat, 0);
        buildEjLan(lan1_rx_data, lan1_rx_data_rdy, lan1_ej_dat_q, arb_dat, 1);
        detail::wireConn(hnx_rx_req, arb_req.out);
        arb_req.out_rdy = hnx_rx_req_rdy;
        detail::wireConn(hnx_rx_resp, arb_rsp.out);
        arb_rsp.out_rdy = hnx_rx_resp_rdy;
        detail::wireConn(hnx_rx_data, arb_dat.out);
        arb_dat.out_rdy = hnx_rx_data_rdy;

        // ---- inject：hnx_tx_* → friends 分发 → q → lan_tx_* ----
        // RSP：tgt 直通，无字段改写
        buildIj(hnx_tx_resp, hnx_tx_resp_rdy, lan0_ij_rsp_q, lan1_ij_rsp_q,
                [](const RespFlit& b, uint8_t) { return b.tgt_id; },
                [](RespFlit b, uint16_t tgt, uint16_t) {
                    b.tgt_id = tgt;
                    return b;
                });
        // DAT：HomeNID := srcId
        buildIj(hnx_tx_data, hnx_tx_data_rdy, lan0_ij_dat_q, lan1_ij_dat_q,
                [](const DataFlit& b, uint8_t) { return b.tgt_id; },
                [](DataFlit b, uint16_t tgt, uint16_t srcId) {
                    b.tgt_id   = tgt;
                    b.home_nid = srcId;
                    return b;
                });
        // SNP：tgt 直通，无字段改写
        buildIj(hnx_tx_snoop, hnx_tx_snoop_rdy, lan0_ij_snp_q, lan1_ij_snp_q,
                [](const SnoopFlit& b, uint8_t) { return b.tgt_id; },
                [](SnoopFlit b, uint16_t tgt, uint16_t) {
                    b.tgt_id = tgt;
                    return b;
                });
        // ERQ：按 mems addrCheck 选址；ReturnNID noDmt 改写
        buildIj(hnx_tx_erq, hnx_tx_erq_rdy, lan0_ij_erq_q, lan1_ij_erq_q,
                [](const HReqFlit& b, uint8_t ci_v) {
                    const bool hit = ((b.addr >> 44) & 0xF) == (ci_v & 0xF);
                    return hit ? Cfg.mem_nid : uint16_t{0};
                },
                [](HReqFlit b, uint16_t tgt, uint16_t srcId) {
                    const bool noDmt = b.return_nid == 0x7FF;  // andR
                    b.return_nid   = noDmt ? srcId : b.return_nid;
                    b.tgt_id       = tgt;
                    return b;
                });

        detail::wireConn(lan0_tx_resp, lan0_ij_rsp_q.deq);
        lan0_ij_rsp_q.deq_rdy = lan0_tx_resp_rdy;
        detail::wireConn(lan0_tx_data, lan0_ij_dat_q.deq);
        lan0_ij_dat_q.deq_rdy = lan0_tx_data_rdy;
        detail::wireConn(lan0_tx_snoop, lan0_ij_snp_q.deq);
        lan0_ij_snp_q.deq_rdy = lan0_tx_snoop_rdy;
        detail::wireConn(lan0_tx_erq, lan0_ij_erq_q.deq);
        lan0_ij_erq_q.deq_rdy = lan0_tx_erq_rdy;
        detail::wireConn(lan1_tx_resp, lan1_ij_rsp_q.deq);
        lan1_ij_rsp_q.deq_rdy = lan1_tx_resp_rdy;
        detail::wireConn(lan1_tx_data, lan1_ij_dat_q.deq);
        lan1_ij_dat_q.deq_rdy = lan1_tx_data_rdy;
        detail::wireConn(lan1_tx_snoop, lan1_ij_snp_q.deq);
        lan1_ij_snp_q.deq_rdy = lan1_tx_snoop_rdy;
        detail::wireConn(lan1_tx_erq, lan1_ij_erq_q.deq);
        lan1_ij_erq_q.deq_rdy = lan1_tx_erq_rdy;
    }

private:
    static bool friendsHit(uint16_t tgt, int lan) {
        const uint16_t router = tgt & ~uint16_t{0x7};  // NodeIdBundle.router
        return Cfg.friends[lan][0] == router || Cfg.friends[lan][1] == router;
    }

    static uint16_t srcIdOf(uint16_t tgt) {
        return friendsHit(tgt, 0) ? Cfg.nids[0] : (friendsHit(tgt, 1) ? Cfg.nids[1] : uint16_t{0});
    }

    // eject 仲裁输入（每通道一次）：arb.in = {q0.deq, q1.deq}
    template <class F>
    void buildEjArb(RRArb<F, 2>& arb, Queue<F, 2>& q0, Queue<F, 2>& q1) {
        arb.in.assign().reads(q0.deq, q1.deq) = [](auto src) {
            auto [q0_deq, q1_deq] = src;
            return std::array<Dec<F>, 2>{q0_deq, q1_deq};
        };
    }

    // eject 单 lan：lan_rx → q.enq；q.deq_rdy ← arb.in_rdy[idx]
    template <class F>
    void buildEjLan(In<Dec<F>>& lanRx, Out<bool>& lanRxRdy, Queue<F, 2>& q, RRArb<F, 2>& arb,
                    uint32_t idx) {
        q.enq = lanRx;
        detail::wireConn(lanRxRdy, q.enq_rdy);
        q.deq_rdy.assign().reads(arb.in_rdy) = [idx](auto src) {
            auto [arb_in_rdy] = src;
            return arb_in_rdy[idx];
        };
    }

    // inject 单通道：hnx_tx →（tgt/命中/改写，组合）→ q0/q1.enq。
    // tgt/命中/srcId 在三个 lambda 内各自重算（纯组合，无共享线网语义差异）。
    template <class F, class TgtOf, class Fix>
    void buildIj(In<Dec<F>>& hnxTx, Out<bool>& hnxTxRdy, Queue<F, 2>& q0, Queue<F, 2>& q1,
                 TgtOf tgtOf, Fix fix) {
        q0.enq.assign().reads(hnxTx, ci) = [tgtOf, fix](auto src) {
            auto [hnxTx, ci] = src;
            const uint16_t tgt = tgtOf(hnxTx.bits, ci);
            Dec<F> d;
            d.valid = hnxTx.valid && friendsHit(tgt, 0);
            d.bits  = fix(hnxTx.bits, tgt, srcIdOf(tgt));
            return d;
        };
        q1.enq.assign().reads(hnxTx, ci) = [tgtOf, fix](auto src) {
            auto [hnxTx, ci] = src;
            const uint16_t tgt = tgtOf(hnxTx.bits, ci);
            Dec<F> d;
            d.valid = hnxTx.valid && friendsHit(tgt, 1);
            d.bits  = fix(hnxTx.bits, tgt, srcIdOf(tgt));
            return d;
        };
        // txBd.ready = Mux1H(friendsHitVec, q*.enq_rdy)（无命中 → false，与 RTL 停住一致）
        hnxTxRdy.assign().reads(hnxTx, ci, q0.enq_rdy, q1.enq_rdy) = [tgtOf](auto src) {
            auto [hnxTx, ci, q0_enq_rdy, q1_enq_rdy] = src;
            const uint16_t tgt = tgtOf(hnxTx.bits, ci);
            return (friendsHit(tgt, 0) && q0_enq_rdy) || (friendsHit(tgt, 1) && q1_enq_rdy);
        };
    }
};

}  // namespace zj::home
