// P1 环单测（扁平语义模型版）：
//   chanComb/chanNext 纯函数：打拍/保持、注入仲裁/弹出匹配/防饿死令牌；
//   vipNext：alloc/rel/指针轮转；EjectBuffer 占用反压 + VIP 末槽（经 ChanComb）；
//   Ring 端到端：RnRouter 译码、SrcID 盖章、HRQ 通道 ERQ/SNP 混合、弹出反压。
// 精确逐拍时序的权威验收在 verify/ 的 RTL 对拍（ZRING 合成流量）与 golden trace
// 重放（test_wolvic_top_replay）；此处为定向语义用例。

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

// REQ 通道单站驱动助手：等价于旧 SingleChannelTap 单测（gid0、双向 tap、
// InjQueue 在环模块内——经 inj_enq 注入）。
using ReqStop = ChanStop<RReqFlit, 5, false>;

ChanComb<RReqFlit> reqComb(const ReqStop& s, const RingSlot<RReqFlit>& rx0,
                           const RingSlot<RReqFlit>& rx1, Valid<RReqFlit> injA,
                           bool ejOutRdy, uint8_t gid = 0) {
    const auto& sp = kStopTable[gid];
    return chanComb<RReqFlit, 5, false>(s, rx0, rx1, injA, Valid<RReqFlit>{}, false,
                                        sp.rightMask, sp.leftMask, gid,
                                        /*hasInj=*/true, /*hasEj=*/true, ejOutRdy);
}

ReqStop reqNext(const ReqStop& s, const ChanComb<RReqFlit>& c) {
    return chanNext<RReqFlit, 5, false>(s, c, false, true, true);
}

TEST_CASE("环打拍（RingPipe 等价）：1 拍延迟 + RegEnable 保持") {
    ReqStop s{};
    // 空输入：无活动
    auto c0 = reqComb(s, {}, {}, {}, false);
    CHECK(!chanActive<RReqFlit, 5, false>(s, c0));

    // t0：valid 进入（纯打拍形态用 hasInj/hasEj=false 重算）
    RingSlot<RReqFlit> in{};
    in.valid        = true;
    in.flit.txn_id  = 0x55;
    in.rsvd_valid   = true;
    in.rsvd_payload = 0x123;
    auto pipeComb   = [](const ReqStop& st, const RingSlot<RReqFlit>& rx0) {
        return chanComb<RReqFlit, 5, false>(st, rx0, RingSlot<RReqFlit>{}, Valid<RReqFlit>{},
                                            Valid<RReqFlit>{}, false, 0, 0, 0, false, false,
                                            false);
    };
    auto pipeNext = [](const ReqStop& st, const ChanComb<RReqFlit>& c) {
        return chanNext<RReqFlit, 5, false>(st, c, false, false, false);
    };
    auto c1 = pipeComb(s, in);
    CHECK(chanActive<RReqFlit, 5, false>(s, c1));  // 来流唤醒
    s = pipeNext(s, c1);
    // t1：输出寄存器可见
    CHECK(s.dir[0].slot.valid);
    CHECK(s.dir[0].slot.flit.txn_id == 0x55);
    CHECK(s.dir[0].slot.rsvd_valid);
    CHECK(s.dir[0].slot.rsvd_payload == 0x123);
    // t2：输入撤掉后 bits 保持（RegEnable）
    in.valid        = false;
    in.rsvd_valid   = false;
    in.flit.txn_id  = 0xAA;
    auto c2 = pipeComb(s, in);
    s       = pipeNext(s, c2);
    CHECK(!s.dir[0].slot.valid);
    CHECK(s.dir[0].slot.flit.txn_id == 0x55);  // 保持
    CHECK(!s.dir[0].slot.rsvd_valid);
    CHECK(s.dir[0].slot.rsvd_payload == 0x123);
}

TEST_CASE("注入仲裁：空环注入 1 拍到打拍寄存器；环流量优先；弹出匹配") {
    ReqStop s{};
    // 注入请求进 InjQueue（tgt gid=2：gid0 的 right 表内 → dir0）
    auto c0 = reqComb(s, {}, {}, {true, mkReq(2)}, false);
    CHECK(c0.inj_enq_rdy);  // 空 InjQueue
    s = reqNext(s, c0);

    // t1：InjQueue 非空 → 注入仲裁；空环 → dir0 fire
    auto c1 = reqComb(s, {}, {}, {}, false);
    CHECK(c1.inj_vld_dir[0]);
    CHECK(c1.inj_rdy_dir[0]);
    CHECK(c1.inj_fire[0]);
    CHECK(!c1.eject_vld[0]);
    s = reqNext(s, c1);
    // t2：flit 上环（打拍寄存器），盖章 = nid(0<<3)|aid(3)
    CHECK(s.dir[0].slot.valid);
    CHECK(s.dir[0].slot.flit.txn_id == 0x12);
    CHECK(s.dir[0].slot.flit.src_id == 0x03);
    CHECK(!s.dir[0].slot.rsvd_valid);

    // 环流量优先：rx0 占用（tgt gid=5，非本站）→ 注入被挡（emptySlot=false）
    s = ReqStop{};  // 重置场景
    s = reqNext(s, reqComb(s, {}, {}, {true, mkReq(2)}, false));  // injq 压入
    RingSlot<RReqFlit> busy{};
    busy.valid = true;
    busy.flit  = mkReq(5);
    auto c2 = reqComb(s, busy, {}, {}, false);
    CHECK(!c2.inj_rdy_dir[0]);  // 环上占用
    CHECK(!c2.eject_vld[0]);    // 不弹出

    // 弹出匹配 + 同拍空槽注入：rx0 tgt gid=0（本站）
    RingSlot<RReqFlit> mine{};
    mine.valid       = true;
    mine.flit        = mkReq(0);
    mine.flit.txn_id = 0x77;
    auto c3 = reqComb(s, mine, {}, {}, /*ejOutRdy=*/true);
    CHECK(c3.eject_vld[0]);
    CHECK(c3.tx.valid == false);  // 弹出先进 EjectBuffer，不直达 tx
    CHECK(c3.inj_rdy_dir[0]);     // ejectFire → emptySlot
    CHECK(c3.inj_fire[0]);
    s = reqNext(s, c3);
    // 注入的 flit 上环（弹出的已进 EjectBuffer）
    CHECK(s.dir[0].slot.valid);
    CHECK(s.dir[0].slot.flit.txn_id == 0x12);
}

TEST_CASE("防饿死：阻塞 8 拍盖令牌，令牌回来必注入") {
    // gid=3（奇序站，right=[4..8]）；注入 tgt gid=9 → left 表 → dir1
    ReqStop s{};
    s = reqNext(s, reqComb(s, {}, {}, {true, mkReq(9)}, false, /*gid=*/3));

    // 持续环流量（rx1，tgt gid=5 非本站）+ InjQueue 待注入
    RingSlot<RReqFlit> busy{};
    busy.valid = true;
    busy.flit  = mkReq(5);
    for (int t = 0; t < 8; ++t) {
        auto c = reqComb(s, {}, busy, {}, false, /*gid=*/3);
        CHECK(!c.inj_rdy_dir[1]);
        CHECK(!c.inj_fire[1]);
        CHECK(!s.dir[1].slot.rsvd_valid);
        s = reqNext(s, c);
    }
    CHECK(s.dir[1].counter == 8);  // timerBits=4：bit3 置位
    // 下一拍：state 转入 inject_reserved
    s = reqNext(s, reqComb(s, {}, busy, {}, false, 3));
    CHECK(s.dir[1].state == kTapInjectReserved);
    // 再一拍：输出寄存器载令牌（rsvd_valid）
    s = reqNext(s, reqComb(s, {}, busy, {}, false, 3));
    CHECK(s.dir[1].slot.rsvd_valid);
    CHECK(s.dir[1].slot.rsvd_payload == ((3 << 3) | 1));  // {nid=3, tapIdx=1}

    // 令牌绕环回来（rx1.rsvd 匹配）+ 空槽 → 必注入
    RingSlot<RReqFlit> tok{};
    tok.rsvd_valid   = true;
    tok.rsvd_payload = (3 << 3) | 1;
    auto c = reqComb(s, {}, tok, {}, false, 3);
    CHECK(c.inj_rdy_dir[1]);
    CHECK(c.inj_fire[1]);
    s = reqNext(s, c);
    CHECK(s.dir[1].slot.valid);
    CHECK(s.dir[1].slot.flit.txn_id == 0x12);
    CHECK(!s.dir[1].slot.rsvd_valid);  // 令牌消费掉
    CHECK(s.dir[1].state == kTapNormal);
}

TEST_CASE("vipNext：alloc/rel/指针轮转") {
    VipState<4> v{};
    CHECK(!vipSel(v).valid);

    // alloc tag=0x11
    v = vipNext(v, true, false, 0x11);
    CHECK(vipSel(v).valid);
    CHECK(vipSel(v).bits == 0x11);

    // 同 tag 重复不重复 alloc；再 alloc 0x22
    v = vipNext(v, true, false, 0x11);
    v = vipNext(v, true, false, 0x22);
    CHECK(vipSel(v).bits == 0x11);  // ptr 不动

    // rel 0x11：次态 valids[0] 清、ptr 未搬（vip 失效）；再一次 next 指针搬到 0x22
    v = vipNext(v, true, true, 0x11);
    CHECK(!vipSel(v).valid);
    CHECK(vipDangling(v));
    v = vipNext(v, false, true, 0);
    CHECK(vipSel(v).valid);
    CHECK(vipSel(v).bits == 0x22);
}

TEST_CASE("EjectBuffer 占用反压 + VIP 末槽只放行命中 tag") {
    // RSP/DAT 配置深度 3：gid0 仅弹出场景
    using EbStop = ChanStop<RReqFlit, 3, false>;
    auto ejComb = [](const EbStop& s, const RingSlot<RReqFlit>& rx0, bool ejRdy) {
        const auto& sp = kStopTable[0];
        return chanComb<RReqFlit, 3, false>(s, rx0, RingSlot<RReqFlit>{}, Valid<RReqFlit>{},
                                            Valid<RReqFlit>{}, false, sp.rightMask,
                                            sp.leftMask, 0, false, true, ejRdy);
    };
    auto flitWith = [](uint16_t src, uint16_t txn) {
        auto f   = mkReq(0);
        f.src_id = src;
        f.txn_id = txn;
        return f;
    };

    // 填满到 empties==1（A、B 进，tx 不开）。末槽是 VIP 保留槽。
    EbStop s{};
    for (int k = 0; k < 2; ++k) {
        RingSlot<RReqFlit> in{};
        in.valid = true;
        in.flit  = flitWith(0x10 + k, 0x20 + k);
        auto c   = ejComb(s, in, false);
        CHECK(c.eject_vld[0]);
        CHECK(c.eb_enq_rdy[0]);  // empties>1 → 放行
        s = chanNext<RReqFlit, 3, false>(s, c, false, false, true);
    }
    // 新 tag D：empties==1 且 VIP 表空 → 被挡；upd 链登记后放行
    RingSlot<RReqFlit> in{};
    in.valid = true;
    in.flit  = flitWith(0x77, 0x33);
    auto c1  = ejComb(s, in, false);
    CHECK(!c1.eb_enq_rdy[0]);  // VIP 表还没登记 D
    s = chanNext<RReqFlit, 3, false>(s, c1, false, false, true);  // upd 链打拍
    auto c2 = ejComb(s, in, false);
    CHECK(!c2.eb_enq_rdy[0]);  // alloc 本拍末才提交
    s = chanNext<RReqFlit, 3, false>(s, c2, false, false, true);
    auto c3 = ejComb(s, in, false);
    CHECK(c3.eb_enq_rdy[0]);  // D 已是 VIP → 放行
    CHECK(c3.tx.valid);       // A 在出口（oqueue 头部）
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
