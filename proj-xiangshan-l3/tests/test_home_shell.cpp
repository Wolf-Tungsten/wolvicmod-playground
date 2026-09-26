// P2 HomeShell 单测：ChiBuffer 队列时序、RRArb 双 lan 合流、friends 分发、
// DAT.HomeNID / ERQ.TgtID+ReturnNID 改写、无命中停住。
// 对齐 HomeWrapper.scala:45-145；队列/仲裁元件级时序已由 prefab RTL 对拍覆盖。

#include <cstdint>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <model/home/home_shell.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace zj::home;
using namespace zj::chi;
using namespace prefabtest;

namespace {

using Shell0 = HomeShell<kHomeBank0>;

void tieOff(Shell0& sh) {
    sh.ci.set(0);
    sh.hnx_rx_req_rdy.set(false);
    sh.hnx_rx_resp_rdy.set(false);
    sh.hnx_rx_data_rdy.set(false);
    sh.hnx_tx_resp.set(Valid<RespFlit>{});
    sh.hnx_tx_data.set(Valid<DataFlit>{});
    sh.hnx_tx_snoop.set(Valid<SnoopFlit>{});
    sh.hnx_tx_erq.set(Valid<HReqFlit>{});
    sh.lan0_rx_req.set(Valid<RReqFlit>{});
    sh.lan0_rx_resp.set(Valid<RespFlit>{});
    sh.lan0_rx_data.set(Valid<DataFlit>{});
    sh.lan1_rx_req.set(Valid<RReqFlit>{});
    sh.lan1_rx_resp.set(Valid<RespFlit>{});
    sh.lan1_rx_data.set(Valid<DataFlit>{});
    sh.lan0_tx_resp_rdy.set(false);
    sh.lan0_tx_data_rdy.set(false);
    sh.lan0_tx_snoop_rdy.set(false);
    sh.lan0_tx_erq_rdy.set(false);
    sh.lan1_tx_resp_rdy.set(false);
    sh.lan1_tx_data_rdy.set(false);
    sh.lan1_tx_snoop_rdy.set(false);
    sh.lan1_tx_erq_rdy.set(false);
}

TEST_CASE("HomeShell eject：lan→队列→RR 合流到 hnx；双 lan 轮转") {
    Shell0 sh;
    sh.elaborate();
    tieOff(sh);
    sh.hnx_rx_req_rdy.set(true);

    // lan0 单源：1 拍到 hnx
    Valid<RReqFlit> q0;
    q0.valid       = true;
    q0.bits.txn_id = 0x10;
    q0.bits.src_id = 0x08;
    sh.lan0_rx_req.set(q0);
    comb(sh);
    CHECK(sh.lan0_rx_req_rdy.get());
    CHECK(!sh.hnx_rx_req.get().valid);
    edge(sh);
    sh.lan0_rx_req.set(Valid<RReqFlit>{});
    comb(sh);
    CHECK(sh.hnx_rx_req.get().valid);
    CHECK(sh.hnx_rx_req.get().bits.txn_id == 0x10);
    edge(sh);
    comb(sh);
    CHECK(!sh.hnx_rx_req.get().valid);

    // 双 lan 持续供应：连续输出应交替来自两路（无饿死）
    Valid<RReqFlit> a, b;
    a.valid = true; a.bits.txn_id = 0xA0;
    b.valid = true; b.bits.txn_id = 0xB0;
    sh.lan0_rx_req.set(a);
    sh.lan1_rx_req.set(b);
    cycle(sh);  // 两路各入 1 项
    std::array<uint16_t, 4> seen{};
    for (int i = 0; i < 4; ++i) {
        comb(sh);
        CHECK(sh.hnx_rx_req.get().valid);
        seen[i] = sh.hnx_rx_req.get().bits.txn_id;
        cycle(sh);
    }
    CHECK(seen[0] != seen[1]);
    CHECK(seen[1] != seen[2]);
    CHECK(seen[2] != seen[3]);
    bool sawA = false, sawB = false;
    for (auto t : seen) {
        sawA = sawA || (t == 0xA0);
        sawB = sawB || (t == 0xB0);
    }
    CHECK(sawA);
    CHECK(sawB);
    sh.lan0_rx_req.set(Valid<RReqFlit>{});
    sh.lan1_rx_req.set(Valid<RReqFlit>{});
    // 排空残余（每队列最多 2 项，数目不定，跑到空）
    for (int i = 0; i < 10; ++i) {
        comb(sh);
        if (!sh.hnx_rx_req.get().valid) break;
        cycle(sh);
    }
    comb(sh);
    CHECK(!sh.hnx_rx_req.get().valid);
}

TEST_CASE("HomeShell inject RSP：friends 分发选 lan + rdy 多选") {
    Shell0 sh;
    sh.elaborate();
    tieOff(sh);
    sh.lan0_tx_resp_rdy.set(true);
    sh.lan1_tx_resp_rdy.set(true);

    // tgt=CC(0x08) ∈ lan0 friends（严格打一拍）
    Valid<RespFlit> r;
    r.valid        = true;
    r.bits.tgt_id  = 0x08;
    r.bits.txn_id  = 0x55;
    sh.hnx_tx_resp.set(r);
    comb(sh);
    CHECK(sh.hnx_tx_resp_rdy.get());      // lan0 队列空 → 可收
    CHECK(!sh.lan0_tx_resp.get().valid);  // 队列打 1 拍
    CHECK(!sh.lan1_tx_resp.get().valid);
    edge(sh);
    sh.hnx_tx_resp.set(Valid<RespFlit>{});
    comb(sh);
    CHECK(sh.lan0_tx_resp.get().valid);
    CHECK(sh.lan0_tx_resp.get().bits.txn_id == 0x55);
    CHECK(!sh.lan1_tx_resp.get().valid);
    cycle(sh);  // lan0 队列出空
    comb(sh);
    CHECK(!sh.lan0_tx_resp.get().valid);

    // tgt=S(0x30) ∈ lan1 friends
    r.bits.tgt_id = 0x30;
    r.bits.txn_id = 0x66;
    sh.hnx_tx_resp.set(r);
    comb(sh);
    CHECK(sh.hnx_tx_resp_rdy.get());
    edge(sh);
    sh.hnx_tx_resp.set(Valid<RespFlit>{});
    comb(sh);
    CHECK(!sh.lan0_tx_resp.get().valid);
    CHECK(sh.lan1_tx_resp.get().valid);
    CHECK(sh.lan1_tx_resp.get().bits.txn_id == 0x66);
    cycle(sh);
    comb(sh);
    CHECK(!sh.lan1_tx_resp.get().valid);

    // tgt=HI(0x20)：无命中 → 停住（rdy=0、两 lan 均无 valid）
    r.bits.tgt_id = 0x20;
    sh.hnx_tx_resp.set(r);
    comb(sh);
    CHECK(!sh.hnx_tx_resp_rdy.get());
    CHECK(!sh.lan0_tx_resp.get().valid);
    CHECK(!sh.lan1_tx_resp.get().valid);
    sh.hnx_tx_resp.set(Valid<RespFlit>{});
}

TEST_CASE("HomeShell inject DAT：HomeNID 改写为命中 lan 的 nid") {
    Shell0 sh;
    sh.elaborate();
    tieOff(sh);
    sh.lan0_tx_data_rdy.set(true);
    sh.lan1_tx_data_rdy.set(true);

    Valid<DataFlit> d;
    d.valid         = true;
    d.bits.tgt_id   = 0x30;  // S → lan1（nid 0x38）
    d.bits.home_nid = 0x7FF;
    d.bits.txn_id   = 0x77;
    sh.hnx_tx_data.set(d);
    cycle(sh);
    sh.hnx_tx_data.set(Valid<DataFlit>{});
    comb(sh);
    CHECK(sh.lan1_tx_data.get().valid);
    CHECK(sh.lan1_tx_data.get().bits.home_nid == 0x38);
    CHECK(sh.lan1_tx_data.get().bits.tgt_id == 0x30);
    CHECK(!sh.lan0_tx_data.get().valid);
    cycle(sh);  // 出空
    comb(sh);
    CHECK(!sh.lan1_tx_data.get().valid);

    d.bits.tgt_id = 0x08;  // CC → lan0（nid 0x00）
    sh.hnx_tx_data.set(d);
    cycle(sh);
    sh.hnx_tx_data.set(Valid<DataFlit>{});
    comb(sh);
    CHECK(sh.lan0_tx_data.get().valid);
    CHECK(sh.lan0_tx_data.get().bits.home_nid == 0x00);
    CHECK(!sh.lan1_tx_data.get().valid);
}

TEST_CASE("HomeShell inject ERQ：mems 选址 + ReturnNID noDmt 改写") {
    Shell0 sh;
    sh.elaborate();
    tieOff(sh);
    sh.lan1_tx_erq_rdy.set(true);

    // addr.ci=0 命中 ci → tgt=S(0x30) ∈ lan1；ReturnNID 全 1（noDmt）→ 改写 srcId
    Valid<HReqFlit> e;
    e.valid            = true;
    e.bits.addr        = 0x80000000ULL;
    e.bits.return_nid  = 0x7FF;
    e.bits.tgt_id      = 0x123;  // 待改写
    sh.hnx_tx_erq.set(e);
    comb(sh);
    CHECK(sh.hnx_tx_erq_rdy.get());
    edge(sh);
    sh.hnx_tx_erq.set(Valid<HReqFlit>{});
    comb(sh);
    CHECK(sh.lan1_tx_erq.get().valid);
    CHECK(sh.lan1_tx_erq.get().bits.tgt_id == 0x30);
    CHECK(sh.lan1_tx_erq.get().bits.return_nid == 0x38);  // lan1 nid
    cycle(sh);  // 出空
    comb(sh);
    CHECK(!sh.lan1_tx_erq.get().valid);

    // ReturnNID 非全 1（DMT）→ 保持
    e.bits.return_nid = 0x123;
    sh.hnx_tx_erq.set(e);
    comb(sh);
    edge(sh);
    sh.hnx_tx_erq.set(Valid<HReqFlit>{});
    comb(sh);
    CHECK(sh.lan1_tx_erq.get().bits.return_nid == 0x123);
    cycle(sh);
    comb(sh);
    CHECK(!sh.lan1_tx_erq.get().valid);

    // addr.ci≠ci（addr[47:44]=1）→ 无命中 → 停住
    e.bits.addr = 1ULL << 44;
    sh.hnx_tx_erq.set(e);
    comb(sh);
    CHECK(!sh.hnx_tx_erq_rdy.get());
    CHECK(!sh.lan1_tx_erq.get().valid);
    sh.hnx_tx_erq.set(Valid<HReqFlit>{});
}

TEST_CASE("HomeShell inject 反压：命中 lan 队列满则 hnx 停收") {
    Shell0 sh;
    sh.elaborate();
    tieOff(sh);
    // lan0_tx_resp_rdy 保持 false → 队列（深 2）积满后 hnx_tx_resp_rdy 应落 0
    Valid<RespFlit> r;
    r.valid       = true;
    r.bits.tgt_id = 0x08;
    sh.hnx_tx_resp.set(r);
    comb(sh);
    CHECK(sh.hnx_tx_resp_rdy.get());
    cycle(sh);
    comb(sh);
    CHECK(sh.hnx_tx_resp_rdy.get());   // 第 2 项
    cycle(sh);
    comb(sh);
    CHECK(!sh.hnx_tx_resp_rdy.get());  // 队列满
    CHECK(sh.lan0_tx_resp.get().valid);
    // 放行 → 恢复
    sh.lan0_tx_resp_rdy.set(true);
    cycle(sh);
    comb(sh);
    CHECK(sh.hnx_tx_resp_rdy.get());
    sh.hnx_tx_resp.set(Valid<RespFlit>{});
}

}  // namespace
