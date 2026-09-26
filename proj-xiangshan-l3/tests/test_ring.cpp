// P1 环模块单测：
//   RingPipe 打拍/保持语义；SingleChannelTap 注入仲裁/弹出/防饿死令牌；
//   VipTable alloc/rel/指针轮转；EjectBuffer 占用反压 + VIP 末槽；
//   Ring 端到端：RnRouter 译码、SrcID 盖章、HRQ 通道 ERQ/SNP 混合、弹出反压。
// 精确逐拍时序的权威验收在 verify/ 的 RTL 对拍（ZRING 合成流量）；此处为定向
// 语义用例。

#include <cstdint>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <model/ring/ring.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace zj;
using namespace zj::ring;
using namespace zj::chi;
using namespace prefabtest;

namespace {

RReqFlit mkReq(uint16_t tgtGid, uint64_t addr = 0x80000000, uint8_t memAttr = 0,
               uint16_t srcId = 0x03) {
    RReqFlit f;
    f.tgt_id   = uint16_t(tgtGid << 3);
    f.src_id   = srcId;  // aid=3，等待盖章
    f.addr     = addr;
    f.mem_attr = memAttr;
    f.txn_id   = 0x12;
    f.opcode   = 0x19;
    f.qos      = 0x5;
    return f;
}

TEST_CASE("RingPipe：1 拍延迟 + RegEnable 保持") {
    RingPipe<RReqFlit> pipe;
    pipe.elaborate();

    RingSlot<RReqFlit> in{};
    pipe.rx.set(in);
    comb(pipe);
    CHECK(!pipe.tx.get().valid);

    // t0：valid 进入
    in.valid        = true;
    in.flit.txn_id  = 0x55;
    in.rsvd_valid   = true;
    in.rsvd_payload = 0x123;
    pipe.rx.set(in);
    cycle(pipe);
    // t1：输出可见；输入撤掉后 bits 保持（RegEnable）
    comb(pipe);
    CHECK(pipe.tx.get().valid);
    CHECK(pipe.tx.get().flit.txn_id == 0x55);
    CHECK(pipe.tx.get().rsvd_valid);
    CHECK(pipe.tx.get().rsvd_payload == 0x123);
    in.valid = false;
    in.rsvd_valid = false;
    in.flit.txn_id = 0xAA;
    pipe.rx.set(in);
    cycle(pipe);
    comb(pipe);
    CHECK(!pipe.tx.get().valid);
    CHECK(pipe.tx.get().flit.txn_id == 0x55);  // 保持
    CHECK(!pipe.tx.get().rsvd_valid);
    CHECK(pipe.tx.get().rsvd_payload == 0x123);
}

TEST_CASE("SingleChannelTap：空环注入 1 拍到 tx；环流量优先；弹出匹配") {
    SingleChannelTap<RReqFlit> tap;
    tap.elaborate();
    tap.match_tag.set(uint16_t(0));  // 本节点 gid0
    tap.tap_idx.set(uint8_t(0));
    tap.rx.set(RingSlot<RReqFlit>{});
    tap.inject.set(Valid<RReqFlit>{});
    tap.eject_rdy.set(true);

    // 空环：inject_rdy 组合即真
    comb(tap);
    CHECK(tap.inject_rdy.get());
    CHECK(!tap.tx.get().valid);

    // 注入（tgt gid=2，非本节点 → 不弹出）
    Valid<RReqFlit> inj;
    inj.valid = true;
    inj.bits  = mkReq(2);
    tap.inject.set(inj);
    comb(tap);
    CHECK(tap.inject_rdy.get());     // fire
    CHECK(!tap.eject.get().valid);
    edge(tap);
    tap.inject.set(Valid<RReqFlit>{});
    comb(tap);
    CHECK(tap.tx.get().valid);       // 1 拍后上环
    CHECK(tap.tx.get().flit.txn_id == 0x12);
    CHECK(!tap.tx.get().rsvd_valid);

    // 下一拍：该 flit 回到 rx（模拟邻站直透），tgt gid=2 不匹配 → 不弹出、续传
    edge(tap);
    RingSlot<RReqFlit> rin{};
    rin.valid = true;
    rin.flit  = mkReq(2);
    tap.rx.set(rin);
    comb(tap);
    CHECK(!tap.eject.get().valid);
    CHECK(!tap.inject_rdy.get());    // 环上占用：注入被挡（emptySlot=false）
    edge(tap);

    // 换成 tgt gid=0（本节点）：弹出 + 同拍空槽可注入（emptySlot=ejectFire）
    rin.flit       = mkReq(0);
    rin.flit.txn_id = 0x77;
    tap.rx.set(rin);
    tap.inject.set(inj);
    comb(tap);
    CHECK(tap.eject.get().valid);
    CHECK(tap.eject.get().bits.txn_id == 0x77);
    CHECK(tap.inject_rdy.get());     // ejectFire → emptySlot
    edge(tap);
    tap.rx.set(RingSlot<RReqFlit>{});
    tap.inject.set(Valid<RReqFlit>{});
    comb(tap);
    CHECK(tap.tx.get().valid);       // 注入的 flit 上环（弹出的已被消费）
    CHECK(tap.tx.get().flit.txn_id == 0x12);
}

TEST_CASE("SingleChannelTap：防饿死——阻塞 8 拍盖令牌，令牌回来必注入") {
    SingleChannelTap<RReqFlit> tap;
    tap.elaborate();
    tap.match_tag.set(uint16_t(3 << 3));  // 本节点 gid3
    tap.tap_idx.set(uint8_t(1));
    tap.eject_rdy.set(false);             // 弹出端永不接收（制造阻塞）
    tap.inject.set(Valid<RReqFlit>{});
    tap.rx.set(RingSlot<RReqFlit>{});

    // 持续环流量（tgt 其它节点，不弹出）+ 持续注入请求
    RingSlot<RReqFlit> rin{};
    rin.valid = true;
    rin.flit  = mkReq(5);
    Valid<RReqFlit> inj;
    inj.valid = true;
    inj.bits  = mkReq(6);
    tap.rx.set(rin);
    tap.inject.set(inj);

    // 阻塞期间：inject_rdy 恒 0，tx.rsvd 不置位
    for (int t = 0; t < 8; ++t) {
        comb(tap);
        CHECK(!tap.inject_rdy.get());
        CHECK(!tap.tx.get().rsvd_valid);
        edge(tap);
    }
    // 第 9 拍：state 转入 inject_reserved；再 1 拍输出寄存器才载令牌
    comb(tap);
    CHECK(!tap.inject_rdy.get());  // 环仍占用
    edge(tap);                     // state ← inject_reserved
    comb(tap);
    CHECK(!tap.tx.get().rsvd_valid);  // rsvdNext 本拍才算出
    edge(tap);                        // out ← rsvd
    comb(tap);
    CHECK(tap.tx.get().rsvd_valid);
    CHECK(tap.tx.get().rsvd_payload == ((3 << 3) | 1));  // {nid=3, tapIdx=1}

    // 令牌绕环回来（rx.rsvd_valid=1, payload 匹配）：即使环上有 flit 也……
    // （emptySlot 仍需空槽；本场景 rx.flit 一直占用且 eject 不开 → 等空槽）
    // 先给空槽 + 令牌：
    rin.valid       = false;       // 空槽
    rin.rsvd_valid  = true;
    rin.rsvd_payload = (3 << 3) | 1;
    tap.rx.set(rin);
    comb(tap);
    CHECK(tap.inject_rdy.get());   // 令牌在手 → 必注入
    edge(tap);
    comb(tap);
    CHECK(tap.tx.get().valid);     // 注入成功上环
    CHECK(tap.tx.get().flit.txn_id == 0x12);
    CHECK(!tap.tx.get().rsvd_valid);  // 令牌消费掉
}

TEST_CASE("VipTable：alloc/rel/指针轮转") {
    VipTable<4> vt;
    vt.elaborate();
    vt.update.set(VipUpd{});
    comb(vt);
    CHECK(!vt.vip.get().valid);

    // alloc tag=0x11（valid 且 rel=false）
    vt.update.set(VipUpd{true, false, 0x11});
    edge(vt);
    vt.update.set(VipUpd{});
    comb(vt);
    CHECK(vt.vip.get().valid);       // ptr=0 → tag 0x11
    CHECK(vt.vip.get().tag == 0x11);

    // 同 tag 重复 update 不重复 alloc；再 alloc 0x22
    vt.update.set(VipUpd{true, false, 0x11});
    edge(vt);
    comb(vt);  // edge 后 clk 停在 1，须先 comb 归零才有下一个 posedge
    vt.update.set(VipUpd{true, false, 0x22});
    edge(vt);
    vt.update.set(VipUpd{});
    comb(vt);
    CHECK(vt.vip.get().tag == 0x11);  // ptr 不动

    // rel 0x11：当拍 valids[0] 仍在（vip 仍 0x11）；下一拍 valids[0] 清、vip 失效、
    // ptrMove 触发；再下一拍 ptr 才搬到 0x22（与 RTL 逐拍一致）
    vt.update.set(VipUpd{true, true, 0x11});
    comb(vt);
    CHECK(vt.vip.get().valid);       // rel 当拍 vip 仍指向 0x11
    CHECK(vt.vip.get().tag == 0x11);
    edge(vt);
    vt.update.set(VipUpd{});
    comb(vt);
    CHECK(!vt.vip.get().valid);      // valids[0] 已清、ptr 未搬
    edge(vt);
    comb(vt);
    CHECK(vt.vip.get().valid);
    CHECK(vt.vip.get().tag == 0x22);
}

TEST_CASE("EjectBuffer：占用反压 + VIP 末槽只放行命中 tag") {
    EjectBuffer<RReqFlit, 3, false> eb;  // RSP/DAT 配置深度 3
    eb.elaborate();
    eb.enq.set(Valid<RReqFlit>{});
    eb.deq_rdy.set(false);

    auto flitWith = [](uint16_t src, uint16_t txn) {
        auto f   = mkReq(0);
        f.src_id = src;
        f.txn_id = txn;
        return f;
    };
    // tag = Cat(src, txn, tgtAid)：src/txn 区分即可
    auto tagOf = [](const RReqFlit& f) {
        return (uint32_t(f.src_id) << 15) | (uint32_t(f.txn_id) << 3) | (f.tgt_id & 0x7);
    };

    // 填满到 empties==1（A、B 进，deq 不开）。注意：末槽（empties==1）是 VIP
    // 保留槽——未登记的 tag 直接进不了第 3 项。
    Valid<RReqFlit> d;
    d.valid = true;
    for (int k = 0; k < 2; ++k) {
        d.bits = flitWith(0x10 + k, 0x20 + k);
        eb.enq.set(d);
        comb(eb);
        CHECK(eb.enq_rdy.get());
        edge(eb);
    }
    comb(eb);
    CHECK(!eb.enq_rdy.get());  // empties==1 且 VIP 表空 → 新 tag 被挡

    // 提供 D（新 tag）：t2 被挡（VIP 表未登记）；t3 upd 链命中但仍未提交 alloc；
    // t4 D 已登记为 VIP → 放行
    d.bits = flitWith(0x77, 0x33);
    eb.enq.set(d);
    comb(eb);
    const bool try1 = eb.enq_rdy.get();
    edge(eb);
    eb.enq.set(d);
    comb(eb);
    const bool try2 = eb.enq_rdy.get();
    edge(eb);
    eb.enq.set(d);
    comb(eb);
    const bool try3 = eb.enq_rdy.get();
    CHECK(!try1);  // VIP 表还没登记 D
    CHECK(!try2);  // alloc 本拍末才提交
    CHECK(try3);   // D 已是 VIP → 放行
    CHECK(eb.deq.get().valid);  // A 在出口
}

TEST_CASE("tapSelOf/STOP_TABLE：方向表抽查") {
    // n1 CC：ns=[2,3,4,5,6,7,8,9,0]，odd → right=[2..6]，left=[0,7,8,9]
    CHECK(kStopTable[1].rightMask == 0b0001111100);
    CHECK(kStopTable[1].leftMask == 0b1110000001);
    // n0：even → right=[1,2,3,4]，left=[5,6,7,8,9]
    CHECK(kStopTable[0].rightMask == 0b0000011110);
    CHECK(kStopTable[0].leftMask == 0b1111100000);
    // 每站 right/left 不重叠且覆盖其它 9 站
    for (const auto& sp : kStopTable) {
        CHECK((sp.rightMask & sp.leftMask) == 0);
        CHECK(((sp.rightMask | sp.leftMask) & 0x3FF) == (0x3FF & ~(1u << sp.gid)));
    }
}

TEST_CASE("Ring e2e：CC REQ 译码到 HF0 + SrcID 盖章 + 弹出") {
    Ring ring;
    ring.elaborate();
    ring.ci.set(0);
    // 默认所有 tx 接收
    ring.n0_tx_req_rdy.set(true);
    ring.n1_tx_req_rdy.set(true);
    ring.n1_tx_resp_rdy.set(true);
    ring.n1_tx_data_rdy.set(true);
    ring.n1_tx_snoop_rdy.set(true);

    ring.n1_rx_req.set(Valid<RReqFlit>{});
    comb(ring);
    CHECK(ring.n1_rx_req_rdy.get());  // InjQueue 空

    // CC 注入：非 device（memAttr=0）、addr[12]=0 → bank0 → tgt HF0(nodeId 0x00)
    Valid<RReqFlit> d;
    d.valid = true;
    d.bits  = mkReq(0, 0x80000000, /*memAttr=*/0, /*srcId=*/0x03);
    ring.n1_rx_req.set(d);
    comb(ring);
    CHECK(ring.n1_rx_req_rdy.get());
    edge(ring);
    ring.n1_rx_req.set(Valid<RReqFlit>{});

    // 等弹出（注入→译码→injq→tap→1 跳→EjectBuffer→RR：上限放宽）
    bool got = false;
    for (int t = 0; t < 12 && !got; ++t) {
        comb(ring);
        if (ring.n0_tx_req.get().valid) {
            got = true;
            CHECK(ring.n0_tx_req.get().bits.tgt_id == 0x00);   // RnRouter 译码到 HF0
            CHECK(ring.n0_tx_req.get().bits.src_id == 0x0B);   // 盖章：nid=1|aid=3
            CHECK(ring.n0_tx_req.get().bits.txn_id == 0x12);
        }
        edge(ring);
    }
    CHECK(got);
}

TEST_CASE("Ring e2e：device REQ 未命中窗口 → defaultHni(HI)；命中 CC 窗口 → RI 注入译码到 CC") {
    Ring ring;
    ring.elaborate();
    ring.ci.set(0);
    ring.n4_tx_req_rdy.set(true);
    ring.n1_tx_req_rdy.set(true);
    ring.n1_rx_req.set(Valid<RReqFlit>{});

    // device=1（memAttr bit1）、addr[43:20]!=0 → 不属 CC 窗口 → HI（gid4，nodeId 0x20）
    Valid<RReqFlit> d;
    d.valid = true;
    d.bits  = mkReq(0, 0x100000, /*memAttr=*/0b0010);
    ring.n1_rx_req.set(d);
    edge(ring);
    ring.n1_rx_req.set(Valid<RReqFlit>{});
    bool got = false;
    for (int t = 0; t < 12 && !got; ++t) {
        comb(ring);
        if (ring.n4_tx_req.get().valid) {
            got = true;
            CHECK(ring.n4_tx_req.get().bits.tgt_id == 0x20);
        }
        edge(ring);
    }
    CHECK(got);

    // device=1、addr[43:20]==0（CC 本地设备窗口）：由 RI 注入（CC 自注自身在
    // 环上不可路由——tgt 不在本站的 left/right 方向表内，RTL 触发断言），
    // 译码到 CC（gid1）→ CC 弹出
    ring.n3_rx_req.set(Valid<RReqFlit>{});
    comb(ring);
    CHECK(ring.n3_rx_req_rdy.get());
    d.bits = mkReq(0, 0x00040, /*memAttr=*/0b0010);
    ring.n3_rx_req.set(d);
    edge(ring);
    ring.n3_rx_req.set(Valid<RReqFlit>{});
    got = false;
    for (int t = 0; t < 16 && !got; ++t) {
        comb(ring);
        if (ring.n1_tx_req.get().valid) {
            got = true;
            CHECK(ring.n1_tx_req.get().bits.tgt_id == 0x08);
            CHECK(ring.n1_tx_req.get().bits.src_id == (0x18 | 0x3));  // RI nid=3 盖章
        }
        edge(ring);
    }
    CHECK(got);
}

TEST_CASE("Ring e2e：HRQ 通道——HF 注入 SNP 经仲裁到 CC；HF ERQ 到 S") {
    Ring ring;
    ring.elaborate();
    ring.ci.set(0);
    ring.n1_tx_snoop_rdy.set(true);
    ring.n6_tx_req_rdy.set(true);
    ring.n0_rx_req.set(Valid<HReqFlit>{});
    ring.n0_rx_snoop.set(Valid<SnoopFlit>{});

    // HF0 注入 Snoop：tgt CC（gid1，nodeId 0x08）
    Valid<SnoopFlit> snp;
    snp.valid       = true;
    snp.bits.tgt_id = 0x08;
    snp.bits.src_id = 0x05;  // aid 待盖章
    snp.bits.txn_id = 0x34;
    snp.bits.opcode = 0x11;
    snp.bits.addr   = 0x12345;
    snp.bits.ret_to_src = true;
    ring.n0_rx_snoop.set(snp);
    comb(ring);
    CHECK(ring.n0_rx_snoop_rdy.get());
    edge(ring);
    ring.n0_rx_snoop.set(Valid<SnoopFlit>{});
    bool got = false;
    for (int t = 0; t < 12 && !got; ++t) {
        comb(ring);
        if (ring.n1_tx_snoop.get().valid) {
            got = true;
            const auto& f = ring.n1_tx_snoop.get().bits;
            CHECK(f.tgt_id == 0x08);
            CHECK(f.src_id == 0x05);      // HF0 nid=0 → 盖章后 0x00|5 不变
            CHECK(f.txn_id == 0x34);
            CHECK(f.opcode == 0x11);
            CHECK(f.addr == 0x12345);
            CHECK(f.ret_to_src);
        }
        edge(ring);
    }
    CHECK(got);

    // HF0 注入 ERQ（HReqFlit）：tgt S（gid6，nodeId 0x30）
    Valid<HReqFlit> erq;
    erq.valid       = true;
    erq.bits.tgt_id = 0x30;
    erq.bits.src_id = 0x02;
    erq.bits.txn_id = 0x56;
    erq.bits.addr   = 0x80001000;
    erq.bits.return_nid = 0x11;
    erq.bits.exp_comp_ack = true;
    ring.n0_rx_req.set(erq);
    comb(ring);
    CHECK(ring.n0_rx_req_rdy.get());
    edge(ring);
    ring.n0_rx_req.set(Valid<HReqFlit>{});
    got = false;
    for (int t = 0; t < 16 && !got; ++t) {
        comb(ring);
        if (ring.n6_tx_req.get().valid) {
            got = true;
            const auto& f = ring.n6_tx_req.get().bits;
            CHECK(f.tgt_id == 0x30);
            CHECK(f.src_id == 0x02);
            CHECK(f.txn_id == 0x56);
            CHECK(f.return_nid == 0x11);   // DMT 字段保留
            CHECK(f.exp_comp_ack);
        }
        edge(ring);
    }
    CHECK(got);
}

}  // namespace
