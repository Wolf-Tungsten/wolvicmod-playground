#pragma once

// PowerDomainCrossing（PDC）：CC socket 的信用弹性缓冲单元，对齐
// zhujiang/device/socket/PowerDomaincCrossing.scala:16-62（文件名含原仓笔误
// "cCrossing"）。kunminghu-v3 CC 节点 socket="sync"，dev/icn 两侧同钟，
// PDC 线（valid/bits/grant）在 CcSocket 内部直连。
//
// PdcTx（PowerDomainCrossingTx）：
//   enq → 1 级寄存 → pdc；token 计数（RegInit 5，3b）门控 enq.ready =
//   tokens.orR；对端 grant（对侧 deq.fire 打一拍）还 token。
//   tokens 更新：enqFire && !rxg → -1；!enqFire && rxg → +1（同拍不变）。
// PdcRx（PowerDomainCrossingRx）：
//   pdc → 1 级寄存 → 5 项 flow Queue → deq；grant = RegNext(deq.fire)。
//   rxq.enq.ready 悬空（RTL 仅 assert 恒 ready，token 协议保证不溢出）。
//
// 队列语义复用 wolvicmod::prefab::Queue（flow=true：空时 deq 同拍直通 enq）。
// 初始态 = 复位后态（无复位端口，同环模型约定）。

#include <cstdint>

#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"
#include "wolvicmod/prefab/queue.h"

namespace zj::sock {

using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Valid;
using wolvicmod::prefab::Queue;

inline constexpr uint32_t kPdcTokens = 5;  // PowerDomainCrossing.tokens

// ---------------- PdcTx ----------------

template <class F>
class PdcTx : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(Valid<F>, enq);
    OUT(bool, enq_rdy);
    OUT(Valid<F>, pdc);
    IN(bool, pdc_grant);

    PdcTx();

private:
    struct St {
        uint8_t tokens = kPdcTokens;  // RegInit(tokens.U(3b))
        bool    rxg    = false;       // RegNext(pdc.grant, false)
        bool    txv    = false;       // RegNext(enqFire, false)
        F       txd{};                // RegEnable(enq.bits, enqFire)，两态取 0

        bool operator==(const St&) const = default;
    };

    REG(St, st);

    WIRE(bool, w_enq_fire);
    // 整条 update 的静止门（perf-breakdown §23 续）
    WIRE(bool, w_any);
};

// ---------------- PdcRx ----------------

template <class F>
class PdcRx : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(Valid<F>, pdc);
    OUT(bool, pdc_grant);
    OUT(Valid<F>, deq);
    IN(bool, deq_rdy);

    PdcRx();

private:
    struct St {
        bool rxv = false;  // RegNext(pdc.valid, false)
        F    rxd{};        // RegEnable(pdc.bits, pdc.valid)，两态取 0
        bool txg = false;  // RegNext(deq.fire, false)

        bool operator==(const St&) const = default;
    };

    using RxQ = Queue<F, kPdcTokens, true>;  // Queue(gen, 5, flow=true)
    MOD(RxQ, rxq);

    REG(St, st);

    WIRE(bool, w_deq_fire);
    // 整条 update 的静止门（perf-breakdown §23 续）
    WIRE(bool, w_any);
};

template <class F>
PdcTx<F>::PdcTx() {
    enq_rdy.assign().reads(st) = [](auto src) {
        auto [st] = src;
        return st.tokens != 0;  // tokens.orR
    };
    pdc.assign().reads(st) = [](auto src) {
        auto [st] = src;
        Valid<F> d;
        d.valid = st.txv;
        d.bits  = st.txd;
        return d;
    };
    w_enq_fire.assign().reads(enq, st) = [](auto src) {
        auto [enq, st] = src;
        return enq.valid && st.tokens != 0;
    };
    // 静止门（§23 续）：候选 = enq fire || 对端 grant || txv/rxg 未清。
    // txv=RegNext(enqFire)、rxg=RegNext(grant) 均自清，旧值项不可漏；tokens
    // 的变化要求 fire 或 rxg；txd 为 RegEnable(fire)。
    w_any.assign().reads(st, pdc_grant, w_enq_fire) = [](auto src) {
        auto [st, pdc_grant, w_enq_fire] = src;
        return w_enq_fire || pdc_grant || st.txv || st.rxg;
    };
    st.update().on(posedge(clk)).en(w_any).reads(st, enq, pdc_grant, w_enq_fire) = [](auto src) {
        auto [st, enq, pdc_grant, w_enq_fire] = src;
        St next = st;
        if (w_enq_fire && !st.rxg)
            next.tokens = st.tokens - 1;  // RTL 断言 tokens 不越界
        else if (!w_enq_fire && st.rxg)
            next.tokens = st.tokens + 1;
        next.txv = w_enq_fire;
        if (w_enq_fire) next.txd = enq.bits;  // RegEnable 语义
        next.rxg = pdc_grant;
        return next;
    };
}

template <class F>
PdcRx<F>::PdcRx() {
    rxq.clk = clk;
    rxq.enq.assign().reads(st) = [](auto src) {
        auto [st] = src;
        Valid<F> d;
        d.valid = st.rxv;
        d.bits  = st.rxd;
        return d;
    };
    // rxq.enq_rdy 悬空（RTL 中仅接 assert(rxq.io.enq.ready)）
    deq = rxq.deq;
    rxq.deq_rdy = deq_rdy;
    pdc_grant.assign().reads(st) = [](auto src) {
        auto [st] = src;
        return st.txg;
    };
    w_deq_fire.assign().reads(rxq.deq, deq_rdy) = [](auto src) {
        auto [rxq_deq, deq_rdy] = src;
        return rxq_deq.valid && deq_rdy;
    };
    // 静止门（§23 续）：候选 = pdc.valid || deq fire || rxv/txg 未清。
    // rxv=RegNext(pdc.valid)、txg=RegNext(deq.fire) 均自清，旧值项不可漏；
    // rxd 为 RegEnable(pdc.valid)。
    w_any.assign().reads(st, pdc, w_deq_fire) = [](auto src) {
        auto [st, pdc, w_deq_fire] = src;
        return pdc.valid || w_deq_fire || st.rxv || st.txg;
    };
    st.update().on(posedge(clk)).en(w_any).reads(st, pdc, w_deq_fire) = [](auto src) {
        auto [st, pdc, w_deq_fire] = src;
        St next = st;
        next.rxv = pdc.valid;
        if (pdc.valid) next.rxd = pdc.bits;  // RegEnable 语义
        next.txg = w_deq_fire;
        return next;
    };
}

}  // namespace zj::sock
