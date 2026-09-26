#pragma once

// 环通道 tap：对齐 xijiang/router/base/ChannelTap.scala。
//
// SingleChannelTap<T>（ChannelTap.scala:12-91，一站一通道一方向）：
//   注入仲裁（组合）：inject.ready = emptySlot && availableSlot
//     emptySlot     = in.flit.valid ? ejectFire : true      —— 环上流量绝对优先
//     availableSlot = in.rsvd.valid ? (rsvd.bits == {nid,tapIdx}) : !wait_slot
//   防饿死：inject 阻塞（valid && !ready）满 8 拍（10 站环 timerBits=4，counter[3]）
//     → s_inject_reserved：本站在环槽上盖 rsvd 令牌；令牌绕环一周回来
//     （meetRsvdSlot）前其它注入被 wait_slot 挡住，令牌槽必得。
//   弹出：eject.valid = in.flit.tgt.router == matchTag.router && in.flit.valid
//   输出打拍：flitV/RegNext + flitB/RegEnable(inject.valid || in.flit.valid)、
//     rsvdV/RegNext + rsvdB/RegEnable(s_inject_reserved || in.rsvd.valid)
// ChannelTap<T>（ChannelTap.scala:153-191）：双方向 tap + 每方向 EjectBuffer +
//   eject 2:1 ResetRRArbiter（= chisel RRArbiter，仅复位风格不同）合流；
//   inject.ready = Mux1H(injectTapSelOH, taps.ready)。
// RingPipe<T>：无 tap 通道的纯打拍直透（BaseRouter.scala:186-189，flit/rsvd
//   各一条 chisel Pipe：valid RegNext、bits RegEnable(valid)）。

#include <array>
#include <cstdint>

#include "model/ring/eject_buffer.h"
#include "model/ring/ring_slot.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/arb.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::ring {

using wolvicmod::prefab::Valid;
using wolvicmod::prefab::RRArb;

// ---------------- SingleChannelTap ----------------

template <class FlitT>
class SingleChannelTap : public wolvicmod::Module {
public:
    using ValidT = Valid<FlitT>;
    using Slot = RingSlot<FlitT>;

    IN(bool, clk);
    IN(Slot, rx);
    OUT(Slot, tx);
    IN(ValidT, inject);
    OUT(bool, inject_rdy);
    OUT(ValidT, eject);
    IN(bool, eject_rdy);
    IN(uint16_t, match_tag);  // 本节点 nodeId（nid<<3，aid=0）
    IN(uint8_t, tap_idx);     // 方向 idx（0/1）

    SingleChannelTap();

private:
    // 状态编码（one-hot，ChannelTap.scala:30-32）
    static constexpr uint8_t kNormal         = 1;
    static constexpr uint8_t kInjectReserved = 2;
    static constexpr uint8_t kWaitSlot       = 4;
    static constexpr uint8_t kTimerBits      = 4;  // log2Ceil(ringSize+1)，ringSize=10

    struct St {
        uint8_t  state   = kNormal;  // RegInit(s_normal)
        uint8_t  counter = 0;        // RegInit(0.U(timerBits.W))
        Slot     out{};              // 输出打拍寄存器（flitV/flitB/rsvdV/rsvdB）

        bool operator==(const St&) const = default;
    };

    REG(St, st);

    WIRE(bool, w_eject_vld);
    WIRE(bool, w_eject_fire);
    WIRE(bool, w_empty_slot);
    WIRE(uint16_t, w_rsvd_mark);
    WIRE(bool, w_available_slot);
    WIRE(bool, w_inject_rdy);
    WIRE(bool, w_inject_fire);
};

// ---------------- RingPipe：无 tap 通道的纯打拍 ----------------

template <class FlitT>
class RingPipe : public wolvicmod::Module {
public:
    using Slot = RingSlot<FlitT>;

    IN(bool, clk);
    IN(Slot, rx);
    OUT(Slot, tx);

    RingPipe();

private:
    struct St {
        Slot slot{};

        bool operator==(const St&) const = default;
    };

    REG(St, st);
};

// ---------------- ChannelTap：双方向 tap + EjectBuffer + RR 合流 ----------------

template <class FlitT, uint32_t EjDepth, bool IsDat>
class ChannelTap : public wolvicmod::Module {
public:
    using ValidT  = Valid<FlitT>;
    using Slot  = RingSlot<FlitT>;
    using SelArr = std::array<bool, 2>;

    IN(bool, clk);
    IN(Slot, rx0);
    IN(Slot, rx1);
    OUT(Slot, tx0);
    OUT(Slot, tx1);
    IN(ValidT, inject);
    OUT(bool, inject_rdy);
    IN(SelArr, tap_sel_oh);
    OUT(ValidT, eject);
    IN(bool, eject_rdy);
    IN(uint16_t, match_tag);

    ChannelTap();

private:
    using Tap  = SingleChannelTap<FlitT>;
    using Eb   = EjectBuffer<FlitT, EjDepth, IsDat>;
    using Earb = RRArb<FlitT, 2>;

    MOD(Tap, tap0);
    MOD(Tap, tap1);
    MOD(Eb, eb0);
    MOD(Eb, eb1);
    MOD(Earb, earb);
};

template <class FlitT>
SingleChannelTap<FlitT>::SingleChannelTap() {
    // 弹出匹配（ChannelTap.scala:88-90）：tgt.router == matcher.router
    w_eject_vld.assign().reads(rx, match_tag) = [](auto src) {
        auto [rx, match_tag] = src;
        return rx.valid && (rx.flit.tgt_id >> 3) == (match_tag >> 3);
    };
    eject.assign().reads(rx, w_eject_vld) = [](auto src) {
        auto [rx, w_eject_vld] = src;
        ValidT d;
        d.valid = w_eject_vld;
        d.bits  = rx.flit;
        return d;
    };
    w_eject_fire.assign().reads(w_eject_vld, eject_rdy) = [](auto src) {
        auto [w_eject_vld, eject_rdy] = src;
        return w_eject_vld && eject_rdy;
    };

    // 注入仲裁（ChannelTap.scala:63-67）
    w_empty_slot.assign().reads(rx, w_eject_fire) = [](auto src) {
        auto [rx, w_eject_fire] = src;
        return rx.valid ? w_eject_fire : true;
    };
    w_rsvd_mark.assign().reads(match_tag, tap_idx) = [](auto src) -> uint16_t {
        auto [match_tag, tap_idx] = src;
        return (match_tag & ~uint16_t{0x7}) | tap_idx;  // Cat(nid, tapIdx)
    };
    w_available_slot.assign().reads(rx, w_rsvd_mark, st) = [](auto src) {
        auto [rx, w_rsvd_mark, st] = src;
        if (rx.rsvd_valid) return rx.rsvd_payload == w_rsvd_mark;
        return st.state != kWaitSlot;
    };
    w_inject_rdy.assign().reads(w_empty_slot, w_available_slot) = [](auto src) {
        auto [w_empty_slot, w_available_slot] = src;
        return w_empty_slot && w_available_slot;
    };
    inject_rdy = w_inject_rdy;
    w_inject_fire.assign().reads(inject, w_inject_rdy) = [](auto src) {
        auto [inject, w_inject_rdy] = src;
        return inject.valid && w_inject_rdy;
    };

    tx.assign().reads(st) = [](auto src) {
        auto [st] = src;
        return st.out;
    };

    st.update().on(posedge(clk))
        .reads(st, rx, inject, w_inject_rdy, w_inject_fire, w_eject_fire, w_rsvd_mark) =
        [](auto src) {
        auto [st, rx, inject, w_inject_rdy, w_inject_fire, w_eject_fire, w_rsvd_mark] = src;
        St next = st;
        // 防饿死计数（ChannelTap.scala:42-46）
        if (w_inject_fire)
            next.counter = 0;
        else if (inject.valid && !w_inject_rdy && st.state == kNormal &&
                 !(st.counter >> (kTimerBits - 1) & 1))
            next.counter = st.counter + 1;
        // 状态机（ChannelTap.scala:48-58）
        switch (st.state) {
            case kNormal:
                next.state = (st.counter >> (kTimerBits - 1) & 1) ? kInjectReserved : kNormal;
                break;
            case kInjectReserved:
                next.state = w_inject_fire ? kNormal
                                           : (rx.rsvd_valid ? kInjectReserved : kWaitSlot);
                break;
            default:  // kWaitSlot
                next.state = w_inject_fire ? kNormal : kWaitSlot;
                break;
        }
        // 输出打拍（ChannelTap.scala:69-86）
        const bool in_rsvd_reserved = st.state == kInjectReserved;
        next.out.valid              = w_inject_fire || (rx.valid && !w_eject_fire);
        if (inject.valid || rx.valid)  // RegEnable 语义：否则保持
            next.out.flit = w_inject_fire ? inject.bits : rx.flit;
        next.out.rsvd_valid = (in_rsvd_reserved || rx.rsvd_valid) && !w_inject_fire;
        if (in_rsvd_reserved || rx.rsvd_valid)
            next.out.rsvd_payload =
                (in_rsvd_reserved && !rx.rsvd_valid) ? w_rsvd_mark : rx.rsvd_payload;
        return next;
    };
}

template <class FlitT>
RingPipe<FlitT>::RingPipe() {
    tx.assign().reads(st) = [](auto src) {
        auto [st] = src;
        return st.slot;
    };
    st.update().on(posedge(clk)).reads(st, rx) = [](auto src) {
        auto [st, rx] = src;
        St next = st;
        next.slot.valid = rx.valid;  // RegNext
        if (rx.valid) next.slot.flit = rx.flit;  // RegEnable
        next.slot.rsvd_valid = rx.rsvd_valid;
        if (rx.rsvd_valid) next.slot.rsvd_payload = rx.rsvd_payload;
        return next;
    };
}

template <class FlitT, uint32_t EjDepth, bool IsDat>
ChannelTap<FlitT, EjDepth, IsDat>::ChannelTap() {
    tap0.clk = clk;
    tap1.clk = clk;
    eb0.clk = clk;
    eb1.clk = clk;
    earb.clk = clk;

    tap0.rx = rx0;
    tap1.rx = rx1;
    tx0 = tap0.tx;
    tx1 = tap1.tx;
    tap0.match_tag = match_tag;
    tap1.match_tag = match_tag;
    tap0.tap_idx = uint8_t{0};
    tap1.tap_idx = uint8_t{1};

    // 注入分发（ChannelTap.scala:174-175, 191）
    tap0.inject.assign().reads(inject, tap_sel_oh) = [](auto src) {
        auto [inject, tap_sel_oh] = src;
        ValidT d;
        d.valid = inject.valid && tap_sel_oh[0];
        d.bits  = inject.bits;
        return d;
    };
    tap1.inject.assign().reads(inject, tap_sel_oh) = [](auto src) {
        auto [inject, tap_sel_oh] = src;
        ValidT d;
        d.valid = inject.valid && tap_sel_oh[1];
        d.bits  = inject.bits;
        return d;
    };
    inject_rdy.assign().reads(tap_sel_oh, tap0.inject_rdy, tap1.inject_rdy) = [](auto src) {
        auto [tap_sel_oh, tap0_inject_rdy, tap1_inject_rdy] = src;
        // Mux1H：全零选择出 false
        return (tap_sel_oh[0] && tap0_inject_rdy) || (tap_sel_oh[1] && tap1_inject_rdy);
    };

    // 弹出：tap → EjectBuffer → RR 合流（ChannelTap.scala:184-190）
    eb0.enq.assign().reads(tap0.eject) = [](auto src) {
        auto [tap0_eject] = src;
        return tap0_eject;
    };
    tap0.eject_rdy.assign().reads(eb0.enq_rdy) = [](auto src) {
        auto [eb0_enq_rdy] = src;
        return eb0_enq_rdy;
    };
    eb1.enq.assign().reads(tap1.eject) = [](auto src) {
        auto [tap1_eject] = src;
        return tap1_eject;
    };
    tap1.eject_rdy.assign().reads(eb1.enq_rdy) = [](auto src) {
        auto [eb1_enq_rdy] = src;
        return eb1_enq_rdy;
    };
    earb.in.assign().reads(eb0.deq, eb1.deq) = [](auto src) {
        auto [eb0_deq, eb1_deq] = src;
        return std::array<ValidT, 2>{eb0_deq, eb1_deq};
    };
    eb0.deq_rdy.assign().reads(earb.in_rdy) = [](auto src) {
        auto [earb_in_rdy] = src;
        return earb_in_rdy[0];
    };
    eb1.deq_rdy.assign().reads(earb.in_rdy) = [](auto src) {
        auto [earb_in_rdy] = src;
        return earb_in_rdy[1];
    };
    eject.assign().reads(earb.out) = [](auto src) {
        auto [earb_out] = src;
        return earb_out;
    };
    earb.out_rdy.assign().reads(eject_rdy) = [](auto src) {
        auto [eject_rdy] = src;
        return eject_rdy;
    };
}

}  // namespace zj::ring
