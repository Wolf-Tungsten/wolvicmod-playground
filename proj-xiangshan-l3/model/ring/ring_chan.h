#pragma once

// RingChan：环单通道的扁平语义模型——状态 = 10 站 × 2 方向的普通 struct，
// 逻辑 = 三个纯函数（comb / active / next），不依赖 wolvicmod 框架，可直接
// 单测。等价对象（逐拍对齐的源）：
//   注入仲裁/弹出/防饿死令牌  xijiang/router/base/ChannelTap.scala:12-91
//   弹出缓冲 + VIP 末槽       xijiang/router/base/EjectBuffer.scala:76-138
//   VIP 表                    xijiang/router/base/EjectBuffer.scala:10-74
//   双方向合流 RR             chisel3 RRArbiter（prefab/arb.h 对拍件）
//   InjQueue                  chisel3 Queue(2)（prefab/queue.h 对拍件）
//
// 等价性要点：站间只经 RingSlot 寄存器相连（无跨站组合路径），因此全站合并
// 为同一结构内的索引循环是构造性等价；每站每通道的静止门（active）覆盖所有
// 次态可能变化的条件（含 upd 打拍链自清、VIP 指针悬空自走两类无外部激励的
// 自变化）。

#include <array>
#include <cstdint>

#include "model/ring/ring_slot.h"
#include "prefab/fifo_state.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::ring {

using wolvicmod::prefab::Valid;
using zj::prefab::FifoState;

// 注入防饿死状态编码（ChannelTap.scala:30-32，one-hot）
inline constexpr uint8_t kTapNormal         = 1;
inline constexpr uint8_t kTapInjectReserved = 2;
inline constexpr uint8_t kTapWaitSlot       = 4;
inline constexpr uint8_t kTapTimerBits      = 4;  // log2Ceil(ringSize+1)

// ---------------- VipTable（EjectBuffer.scala:10-74）----------------

template <uint32_t Size>
struct VipState {
    std::array<bool, Size>     valids{};
    std::array<uint32_t, Size> table{};
    uint32_t                   ptr_oh = 1;  // 一位热，RegInit(1.U)

    bool operator==(const VipState&) const = default;
};

template <uint32_t Size>
uint32_t vipPtr(const VipState<Size>& v) {
    for (uint32_t i = 0; i < Size; ++i)
        if ((v.ptr_oh >> i) & 1u) return i;
    return 0;
}

template <uint32_t Size>
bool vipAny(const VipState<Size>& v) {
    for (uint32_t i = 0; i < Size; ++i)
        if (v.valids[i]) return true;
    return false;
}

// 指针悬空：ptr 项失效且尚有其它 valid（次拍无激励自走，静止门必须覆盖）
template <uint32_t Size>
bool vipDangling(const VipState<Size>& v) {
    return !v.valids[vipPtr(v)] && vipAny(v);
}

template <uint32_t Size>
Valid<uint32_t> vipSel(const VipState<Size>& v) {
    const uint32_t p = vipPtr(v);
    return {v.valids[p], v.table[p]};
}

template <uint32_t Size>
VipState<Size> vipNext(const VipState<Size>& v, bool upd_valid, bool upd_rel, uint32_t upd_tag) {
    VipState<Size> n       = v;
    uint32_t       enq_idx = Size;  // 首个空闲项（PriorityEncoderOH：全占用出零→不命中）
    for (uint32_t i = 0; i < Size; ++i)
        if (!v.valids[i]) {
            enq_idx = i;
            break;
        }
    bool tag_existed = false;
    for (uint32_t i = 0; i < Size; ++i)
        tag_existed = tag_existed || (v.valids[i] && v.table[i] == upd_tag);
    const bool do_alloc = upd_valid && !tag_existed && !upd_rel;
    for (uint32_t i = 0; i < Size; ++i) {
        const bool enq = do_alloc && enq_idx == i;
        const bool rel = upd_valid && upd_rel && v.valids[i] && v.table[i] == upd_tag;
        n.valids[i]    = enq || (!rel && v.valids[i]);
        if (enq) n.table[i] = upd_tag;
    }
    // 指针搬移（读当前 valids，与 valids 更新同沿提交）：当前 ptr 失效且有其它
    // valid → 高位最近 valid，无则绕回最低 valid
    const uint32_t ptr = vipPtr(v);
    if (!v.valids[ptr] && vipAny(v)) {
        uint32_t nxt = Size;
        for (uint32_t i = ptr + 1; i < Size; ++i)
            if (v.valids[i]) {
                nxt = i;
                break;
            }
        if (nxt == Size)
            for (uint32_t i = 0; i < ptr; ++i)
                if (v.valids[i]) {
                    nxt = i;
                    break;
                }
        n.ptr_oh = 1u << nxt;
    }
    return n;
}

// ---------------- 二输入 RR（chisel RRArbiter，prefab/arb.h 同款）----------------

inline uint32_t rrChosen2(bool v0, bool v1, uint32_t lg) {
    for (uint32_t k = 1; k <= 2; ++k) {
        const uint32_t i = (lg + k) & 1;
        if (i == 0 ? v0 : v1) return i;
    }
    return 1;  // chisel 默认 (n-1).U
}

// in_rdy 两遍结构（arb.h:108-131 的 N=2 展开）
inline std::array<bool, 2> rrInRdy2(bool v0, bool v1, uint32_t lg, bool out_rdy) {
    const bool any_vm = (lg == 0) && v1;
    return {out_rdy && !any_vm, out_rdy && ((lg == 0) || (!any_vm && !v0))};
}

// ---------------- EjectBuffer 状态 ----------------

template <class F, uint32_t EjD, bool IsDat>
struct EbState {
    FifoState<F, 1, false, true> ipipe{};      // Queue(pipe, 1)
    FifoState<F, EjD - 1>        oq{};         // Queue(size-1)
    uint32_t                     empties   = EjD;
    bool                         upd_valid = false;  // RegNext(enq.valid, false)
    bool                         upd_rel   = true;   // RegNext(enq.ready, true)
    uint32_t                     upd_tag   = 0;      // RegEnable(tag, enq.valid)，两态取 0
    VipState<10>                 vip{};              // vip 表项数 = island.size

    bool operator==(const EbState&) const = default;
};

// tag = Cat(src, txn, tgtAid[, dataID])
template <bool IsDat, class F>
uint32_t ebTag(const F& f) {
    const uint32_t tgt_aid = f.tgt_id & 0x7;
    if constexpr (IsDat)
        return (uint32_t(f.src_id) << 17) | (uint32_t(f.txn_id) << 5) | (tgt_aid << 2) |
               f.data_id;
    else
        return (uint32_t(f.src_id) << 15) | (uint32_t(f.txn_id) << 3) | tgt_aid;
}

// ---------------- 一站一通道状态 ----------------

template <class F, uint32_t EjD, bool IsDat>
struct ChanStop {
    struct Dir {  // SingleChannelTap::St：输出打拍 + 注入 FSM
        RingSlot<F> slot{};
        uint8_t     state   = kTapNormal;
        uint8_t     counter = 0;

        bool operator==(const Dir&) const = default;
    };
    Dir                      dir[2];
    FifoState<F, 2>          injq{};         // InjQueue(2)，仅注入站活动
    EbState<F, EjD, IsDat>   eb[2];          // 仅 tap 站活动
    uint32_t                 earb_lg    = 0;  // eject 2:1 RR last_grant
    uint32_t                 inj_arb_lg = 0;  // HRQ ERQ+SNP 合并 RR（仅 HF 站 HRQ 用）

    bool operator==(const ChanStop&) const = default;
};

// ---------------- 组合视图（comb 一次算全，next/gate/边界输出复用）----------------

template <class F>
struct ChanComb {
    RingSlot<F> rx[2]{};            // 邻居来流（next 的打拍源）
    bool        rx_act[2]{};
    // 注入侧
    Valid<F>    inj_enq{};          // 进 InjQueue（调用方已完成译码/HRQ 双源合并输入）
    bool        inj_enq_rdy{};      // = InjQueue.enq_rdy → 边界 rx_rdy / HRQ arb out_rdy
    bool        inj_src_rdy[2]{};   // HRQ 双源各自 rdy；单源用 [0]
    uint32_t    inj_chosen{};
    Valid<F>    inj_payload{};      // SrcID 盖章后的 injq.deq
    bool        inj_vld_dir[2]{};
    bool        inj_rdy_dir[2]{};
    bool        inj_fire[2]{};
    uint16_t    rsvd_mark[2]{};
    bool        injq_deq_rdy{};
    // 弹出侧
    bool        eject_vld[2]{};
    bool        eject_fire[2]{};
    bool        eb_enq_rdy[2]{};
    uint32_t    eb_tag[2]{};
    Valid<F>    ipipe_enq[2]{};
    bool        ipipe_enq_fire[2]{};
    Valid<F>    oq_enq[2]{};
    bool        oq_enq_rdy[2]{};
    bool        eb_deq_fire[2]{};
    bool        earb_in_rdy[2]{};
    uint32_t    earb_chosen{};
    Valid<F>    tx{};
    bool        tx_fire{};

    bool operator==(const ChanComb&) const = default;
};

// 组合视图计算。参数：rx0/rx1 = 左/右邻站的输出寄存器；injA/injB = 注入源
// （REQ/RSP/DAT 用 injA，译码由调用方做；HRQ 双源 injA=ERQ injB=SNP）；
// hasInj/hasEj/hasTap 与选向表来自 kStopTable[i]；ejOutRdy = 边界 tx_rdy。
template <class F, uint32_t EjD, bool IsDat>
ChanComb<F> chanComb(const ChanStop<F, EjD, IsDat>& s, const RingSlot<F>& rx0,
                     const RingSlot<F>& rx1, Valid<F> injA, Valid<F> injB, bool dualInj,
                     uint16_t rightMask, uint16_t leftMask, uint8_t gid, bool hasInj,
                     bool hasEj, bool ejOutRdy) {
    ChanComb<F>      c;
    const bool       hasTap   = hasInj || hasEj;
    const uint16_t   matchTag = uint16_t(gid << 3);
    const RingSlot<F> rxs[2]  = {rx0, rx1};
    for (int d = 0; d < 2; ++d) {
        c.rx[d]     = rxs[d];
        c.rx_act[d] = rxs[d].valid || rxs[d].rsvd_valid;
    }

    // ---- InjQueue 出口 + 注入合并（BaseRouter.scala:144-150, 192-225）----
    c.inj_enq_rdy       = zj::prefab::fifoEnqRdy(s.injq, false);  // 非 pipe：与 deq_rdy 无关
    const Valid<F> deq  = zj::prefab::fifoDeq(s.injq, Valid<F>{});
    if (dualInj) {
        // HF HRQ：ERQ+SNP 经 ResetRRArbiter(2) 合并，out_rdy = injq.enq_rdy
        c.inj_chosen      = rrChosen2(injA.valid, injB.valid, s.inj_arb_lg);
        c.inj_enq.valid   = injA.valid || injB.valid;
        c.inj_enq.bits    = c.inj_chosen == 0 ? injA.bits : injB.bits;
        const auto rd     = rrInRdy2(injA.valid, injB.valid, s.inj_arb_lg, c.inj_enq_rdy);
        c.inj_src_rdy[0]  = rd[0];
        c.inj_src_rdy[1]  = rd[1];
    } else {
        c.inj_enq        = hasInj ? injA : Valid<F>{};
        c.inj_src_rdy[0] = c.inj_enq_rdy;
    }
    // SrcID 盖章（nid 覆写、aid 保留，BaseRouter.scala:208-210）
    c.inj_payload            = deq;
    c.inj_payload.bits.src_id = matchTag | (deq.bits.src_id & 0x7);
    // 静态最短路选向（BaseRouter.scala:199-200；ZJParameters.scala:127-133 方向表）
    std::array<bool, 2> selOh{};
    if (hasInj) {
        const uint8_t tgtGid = uint8_t(deq.bits.tgt_id >> 3);
        selOh = {bool((rightMask >> tgtGid) & 1), bool((leftMask >> tgtGid) & 1)};
    }

    // ---- 每方向：弹出匹配 → EjectBuffer →（下拍再合流），注入仲裁 ----
    for (int d = 0; d < 2; ++d) {
        const auto& rx   = c.rx[d];
        const auto& eb   = s.eb[d];
        // 弹出匹配（ChannelTap.scala:88-90）
        c.eject_vld[d]   = hasTap && rx.valid && (rx.flit.tgt_id >> 3) == gid;
        c.eb_tag[d]      = ebTag<IsDat>(rx.flit);
        const auto vipo  = vipSel(eb.vip);
        // VIP 末槽门控（EjectBuffer.scala:100-109）
        const bool allow = (eb.empties != 1) || (c.eb_tag[d] == vipo.bits && vipo.valid);
        c.eb_enq_rdy[d]  = eb.empties > 0 && allow;
        c.eject_fire[d]  = c.eject_vld[d] && c.eb_enq_rdy[d];
        // ipipe（enq.valid 不被 empties 门控——源 RTL 如此）
        c.ipipe_enq[d]   = {c.eject_vld[d] && allow, rx.flit};
        c.oq_enq_rdy[d]  = zj::prefab::fifoEnqRdy(eb.oq, false);
        const bool ipipe_enq_rdy = zj::prefab::fifoEnqRdy(eb.ipipe, c.oq_enq_rdy[d]);
        c.ipipe_enq_fire[d]      = c.ipipe_enq[d].valid && ipipe_enq_rdy;
        c.oq_enq[d]              = zj::prefab::fifoDeq(eb.ipipe, Valid<F>{});
    }
    // eject 2:1 RR 合流（ChannelTap.scala:184-190）
    {
        const Valid<F> dq0 = zj::prefab::fifoDeq(s.eb[0].oq, Valid<F>{});
        const Valid<F> dq1 = zj::prefab::fifoDeq(s.eb[1].oq, Valid<F>{});
        c.earb_chosen     = rrChosen2(dq0.valid, dq1.valid, s.earb_lg);
        c.tx.valid        = hasEj && (dq0.valid || dq1.valid);
        c.tx.bits         = c.earb_chosen == 0 ? dq0.bits : dq1.bits;
        const auto erdy   = rrInRdy2(dq0.valid, dq1.valid, s.earb_lg, hasEj && ejOutRdy);
        c.earb_in_rdy[0]  = erdy[0];
        c.earb_in_rdy[1]  = erdy[1];
        c.tx_fire         = c.tx.valid && ejOutRdy;
        c.eb_deq_fire[0]  = dq0.valid && erdy[0];
        c.eb_deq_fire[1]  = dq1.valid && erdy[1];
    }
    // 注入仲裁（ChannelTap.scala:63-67）：环上流量绝对优先 + 防饿死令牌
    for (int d = 0; d < 2; ++d) {
        const bool emptySlot = c.rx[d].valid ? c.eject_fire[d] : true;
        c.rsvd_mark[d]       = matchTag | uint16_t(d);  // Cat(nid, tapIdx)，低 3b 恒 0
        const bool avail     = c.rx[d].rsvd_valid ? (c.rx[d].rsvd_payload == c.rsvd_mark[d])
                                                  : (s.dir[d].state != kTapWaitSlot);
        c.inj_vld_dir[d]     = hasInj && deq.valid && selOh[d];
        c.inj_rdy_dir[d]     = emptySlot && avail;
        c.inj_fire[d]        = c.inj_vld_dir[d] && c.inj_rdy_dir[d];
    }
    // InjQueue deq_rdy = Mux1H(selOh, 各向 inject_rdy)（全零选择出 false）
    c.injq_deq_rdy = (selOh[0] && c.inj_rdy_dir[0]) || (selOh[1] && c.inj_rdy_dir[1]);
    return c;
}

// 静止门：次态可能变化 ⇒ true。覆盖：来流（rx_act）、打拍未清空、FSM/计数器
// 活动、弹出活动及 upd 打拍链自清（upd_* 与源值不一致）、EjectBuffer 占用、
// VIP 表 update/指针悬空自走、InjQueue 非空（注入阻塞期间计数器在走）。
template <class F, uint32_t EjD, bool IsDat>
bool chanActive(const ChanStop<F, EjD, IsDat>& s, const ChanComb<F>& c) {
    for (int d = 0; d < 2; ++d) {
        if (c.rx_act[d]) return true;
        const auto& sd = s.dir[d];
        if (sd.slot.valid || sd.slot.rsvd_valid || sd.state != kTapNormal || sd.counter != 0)
            return true;
        const auto& eb = s.eb[d];
        if (c.eject_vld[d] || eb.upd_valid != c.eject_vld[d] || eb.upd_rel != c.eb_enq_rdy[d])
            return true;
        if (eb.empties != EjD || eb.upd_valid || vipDangling(eb.vip)) return true;
    }
    if (c.inj_enq.valid || !zj::prefab::fifoEmpty(s.injq)) return true;
    return false;
}

// 次态（纯函数；NBA 提交由框架 Reg 承担）
template <class F, uint32_t EjD, bool IsDat>
ChanStop<F, EjD, IsDat> chanNext(const ChanStop<F, EjD, IsDat>& s, const ChanComb<F>& c,
                                 bool dualInj, bool hasInj, bool hasEj) {
    ChanStop<F, EjD, IsDat> n = s;
    const bool              hasTap = hasInj || hasEj;
    for (int d = 0; d < 2; ++d) {
        auto       nd     = s.dir[d];
        const auto& sd    = s.dir[d];
        // 防饿死计数（ChannelTap.scala:42-46）
        if (c.inj_fire[d])
            nd.counter = 0;
        else if (c.inj_vld_dir[d] && !c.inj_rdy_dir[d] && sd.state == kTapNormal &&
                 !((sd.counter >> (kTapTimerBits - 1)) & 1))
            nd.counter = sd.counter + 1;
        // 状态机（ChannelTap.scala:48-58）
        switch (sd.state) {
            case kTapNormal:
                nd.state = ((sd.counter >> (kTapTimerBits - 1)) & 1) ? kTapInjectReserved
                                                                     : kTapNormal;
                break;
            case kTapInjectReserved:
                nd.state = c.inj_fire[d] ? kTapNormal
                                         : (c.rx[d].rsvd_valid ? kTapInjectReserved : kTapWaitSlot);
                break;
            default:
                nd.state = c.inj_fire[d] ? kTapNormal : kTapWaitSlot;
                break;
        }
        // 输出打拍（ChannelTap.scala:69-86）
        nd.slot.valid = c.inj_fire[d] || (c.rx[d].valid && !c.eject_fire[d]);
        if (c.inj_vld_dir[d] || c.rx[d].valid)  // RegEnable 语义：否则保持
            nd.slot.flit = c.inj_fire[d] ? c.inj_payload.bits : c.rx[d].flit;
        const bool rsvd_res   = sd.state == kTapInjectReserved;
        nd.slot.rsvd_valid    = (rsvd_res || c.rx[d].rsvd_valid) && !c.inj_fire[d];
        if (rsvd_res || c.rx[d].rsvd_valid)
            nd.slot.rsvd_payload =
                (rsvd_res && !c.rx[d].rsvd_valid) ? c.rsvd_mark[d] : c.rx[d].rsvd_payload;
        n.dir[d] = nd;
        // EjectBuffer 次态
        if (hasTap) {
            auto       neb      = s.eb[d];
            const auto& seb     = s.eb[d];
            if (c.ipipe_enq_fire[d] && !c.eb_deq_fire[d])
                neb.empties = seb.empties - 1;
            else if (!c.ipipe_enq_fire[d] && c.eb_deq_fire[d])
                neb.empties = seb.empties + 1;
            neb.upd_valid = c.eject_vld[d];  // = EjectBuffer.enq.valid（tap.eject）
            neb.upd_rel   = c.eb_enq_rdy[d];
            if (c.eject_vld[d]) neb.upd_tag = c.eb_tag[d];  // RegEnable
            neb.vip   = vipNext(seb.vip, seb.upd_valid, seb.upd_rel, seb.upd_tag);
            neb.ipipe = zj::prefab::fifoNext(seb.ipipe, c.ipipe_enq[d].bits,
                                             c.ipipe_enq[d].valid, c.oq_enq_rdy[d]);
            neb.oq    = zj::prefab::fifoNext(seb.oq, c.oq_enq[d].bits, c.oq_enq[d].valid,
                                             c.earb_in_rdy[d]);
            n.eb[d] = neb;
        }
    }
    n.injq = zj::prefab::fifoNext(s.injq, c.inj_enq.bits, c.inj_enq.valid, c.injq_deq_rdy);
    if (c.tx_fire) n.earb_lg = c.earb_chosen;
    if (dualInj && c.inj_enq.valid && c.inj_enq_rdy) n.inj_arb_lg = c.inj_chosen;
    return n;
}

}  // namespace zj::ring
