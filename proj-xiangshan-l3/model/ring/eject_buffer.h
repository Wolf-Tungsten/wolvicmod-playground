#pragma once

// EjectBuffer（弹出缓冲）与 VipTable：对齐 xijiang/router/base/EjectBuffer.scala。
//
// EjectBuffer<T,Size>（EjectBuffer.scala:76-138）：
//   enq → ipipe(Queue pipe,1) → oqueue(Queue,Size-1) → deq，占用计数 empties
//   （初值 Size）。防死锁 VIP 末槽：empties==1 时只放行 tag 命中 VIP 表的 flit
//   （allowEnq）。tag = Cat(src, txn, tgtAid[, dataID])（DAT 通道多拼 dataID）。
//   VIP 表更新比 enq 晚一拍：valid←RegNext(enq.valid,false)、rel←RegNext(enq.ready,true)、
//   tag←RegEnable(flitTag, enq.valid)。
// VipTable（EjectBuffer.scala:10-74）：valids/table/vipPtrOH（一位热，初值 bit0）；
//   alloc 取首个空闲项；ptr 在当前项失效后向高位找最近 valid，无则绕回最低
//   valid（PriorityEncoderOH 语义：全零输入出零）。
//
// 队列语义直接复用 wolvicmod::prefab::Queue（chisel Queue 对拍元件）。

#include <array>
#include <cstdint>

#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/dec.h"
#include "wolvicmod/prefab/queue.h"

namespace zj::ring {

using wolvicmod::prefab::Dec;
using wolvicmod::prefab::Queue;

// ---------------- VipTable ----------------

struct VipUpd {
    bool     valid = false;
    bool     rel   = true;
    uint32_t tag   = 0;

    bool operator==(const VipUpd&) const = default;
};

struct VipOut {
    bool     valid = false;
    uint32_t tag   = 0;

    bool operator==(const VipOut&) const = default;
};

template <uint32_t Size>
class VipTable : public wolvicmod::Module {
public:
    static_assert(Size >= 1);

    struct St {
        std::array<bool, Size>     valids{};
        std::array<uint32_t, Size> table{};
        uint32_t ptr_oh = 1;  // 一位热，RegInit(1.U(size.W))

        bool operator==(const St&) const = default;
    };

    IN(bool, clk);
    IN(VipUpd, update);
    OUT(VipOut, vip);

    REG(St, st);

    VipTable() {
        vip.assign().reads(st) = [](auto src) {
            auto [st] = src;
            VipOut   o;
            uint32_t ptr = ptrOf(st.ptr_oh);
            o.valid = st.valids[ptr];
            o.tag   = st.table[ptr];
            return o;
        };
        st.update().on(posedge(clk)).reads(st, update) = [](auto src) {
            auto [st, update] = src;
            St next = st;
            // 首个空闲项（PriorityEncoderOH(!valids)：全占用时出零，enq 不命中）
            uint32_t enq_idx = Size;
            for (uint32_t i = 0; i < Size; ++i)
                if (!st.valids[i]) {
                    enq_idx = i;
                    break;
                }
            bool tag_existed = false;
            for (uint32_t i = 0; i < Size; ++i)
                tag_existed = tag_existed || (st.valids[i] && st.table[i] == update.tag);
            const bool do_alloc = update.valid && !tag_existed && !update.rel;
            for (uint32_t i = 0; i < Size; ++i) {
                const bool enq = do_alloc && enq_idx == i;
                const bool rel = update.valid && update.rel && st.valids[i] && st.table[i] == update.tag;
                next.valids[i] = enq ? true : (rel ? false : st.valids[i]);
                if (enq) next.table[i] = update.tag;
            }
            // 指针搬移（读当前 valids，与 valids 更新同沿提交）：当前 ptr 项失效
            // 且有其它 valid → 高位最近 valid，否则绕回最低 valid
            const uint32_t ptr = ptrOf(st.ptr_oh);
            bool any_valid = false;
            for (uint32_t i = 0; i < Size; ++i) any_valid = any_valid || st.valids[i];
            const bool ptr_move = !st.valids[ptr] && any_valid;
            if (ptr_move) {
                uint32_t nxt = Size;
                for (uint32_t i = ptr + 1; i < Size; ++i)
                    if (st.valids[i]) {
                        nxt = i;
                        break;
                    }
                if (nxt == Size)
                    for (uint32_t i = 0; i < ptr; ++i)
                        if (st.valids[i]) {
                            nxt = i;
                            break;
                        }
                next.ptr_oh = 1u << nxt;
            }
            return next;
        };
    }

private:
    static uint32_t ptrOf(uint32_t oh) {
        for (uint32_t i = 0; i < Size; ++i)
            if ((oh >> i) & 1u) return i;
        return 0;
    }
};

// ---------------- EjectBuffer ----------------

// FlitT 须有 .src_id/.txn_id/.tgt_id 字段；IsDat 时再要 .data_id。
template <class FlitT, uint32_t Size, bool IsDat>
class EjectBuffer : public wolvicmod::Module {
public:
    static_assert(Size >= 3, "EjectBuffer.scala:81");
    static constexpr uint32_t kVipSize = 10;  // = island.size（10 站环）

    using DecT = Dec<FlitT>;

    struct St {
        uint32_t empties   = Size;   // RegInit(size.U)
        bool     upd_valid = false;  // RegNext(enq.valid, false)
        bool     upd_rel   = true;   // RegNext(enq.ready, true)
        uint32_t upd_tag   = 0;      // RegEnable(flitTag, enq.valid)，两态取 0

        bool operator==(const St&) const = default;
    };

    IN(bool, clk);
    IN(DecT, enq);
    OUT(bool, enq_rdy);
    OUT(DecT, deq);
    IN(bool, deq_rdy);

    using IPipe    = Queue<FlitT, 1, false, true>;  // Queue(gen, 1, pipe=true)
    using OQueue   = Queue<FlitT, Size - 1>;        // Queue(gen, size-1)
    using VipTableT = VipTable<kVipSize>;

    MOD(IPipe, ipipe);
    MOD(OQueue, oqueue);
    MOD(VipTableT, vip_table);

    REG(St, st);

    WIRE(uint32_t, w_tag);
    WIRE(bool, w_allow_enq);
    WIRE(bool, w_enq_rdy);
    WIRE(bool, w_enq_fire);  // ipipe.enq.fire
    WIRE(bool, w_deq_fire);  // oqueue.deq.fire

    EjectBuffer() {
        ipipe.clk     = clk;
        oqueue.clk    = clk;
        vip_table.clk = clk;

        w_tag.assign().reads(enq) = [](auto src) -> uint32_t {
            auto [enq] = src;
            const uint32_t tgt_aid = enq.bits.tgt_id & 0x7;  // NodeIdBundle.aid
            if constexpr (IsDat)
                return (enq.bits.src_id << 17) | (enq.bits.txn_id << 5) | (tgt_aid << 2) |
                       enq.bits.data_id;
            else
                return (enq.bits.src_id << 15) | (enq.bits.txn_id << 3) | tgt_aid;
        };
        w_allow_enq.assign().reads(st, w_tag, vip_table.vip) = [](auto src) {
            auto [st, w_tag, vip_table_vip] = src;
            if (st.empties != 1) return true;
            return w_tag == vip_table_vip.tag && vip_table_vip.valid;
        };
        w_enq_rdy.assign().reads(st, w_allow_enq) = [](auto src) {
            auto [st, w_allow_enq] = src;
            return st.empties > 0 && w_allow_enq;
        };
        enq_rdy = w_enq_rdy;

        ipipe.enq.assign().reads(enq, w_allow_enq) = [](auto src) {
            auto [enq, w_allow_enq] = src;
            DecT d;
            d.valid = enq.valid && w_allow_enq;  // 注意：不被 empties 门控（源 RTL）
            d.bits  = enq.bits;
            return d;
        };
        // ipipe → oqueue → deq 直通连接
        oqueue.enq.assign().reads(ipipe.deq) = [](auto src) {
            auto [ipipe_deq] = src;
            return ipipe_deq;
        };
        ipipe.deq_rdy.assign().reads(oqueue.enq_rdy) = [](auto src) {
            auto [oqueue_enq_rdy] = src;
            return oqueue_enq_rdy;
        };
        deq.assign().reads(oqueue.deq) = [](auto src) {
            auto [oqueue_deq] = src;
            return oqueue_deq;
        };
        oqueue.deq_rdy.assign().reads(deq_rdy) = [](auto src) {
            auto [deq_rdy] = src;
            return deq_rdy;
        };

        w_enq_fire.assign().reads(ipipe.enq, ipipe.enq_rdy) = [](auto src) {
            auto [ipipe_enq, ipipe_enq_rdy] = src;
            return ipipe_enq.valid && ipipe_enq_rdy;
        };
        w_deq_fire.assign().reads(oqueue.deq, deq_rdy) = [](auto src) {
            auto [oqueue_deq, deq_rdy] = src;
            return oqueue_deq.valid && deq_rdy;
        };

        st.update().on(posedge(clk)).reads(st, enq, w_tag, w_enq_rdy, w_enq_fire, w_deq_fire) =
            [](auto src) {
                auto [st, enq, w_tag, w_enq_rdy, w_enq_fire, w_deq_fire] = src;
                St next = st;
                if (w_enq_fire && !w_deq_fire)
                    next.empties = st.empties - 1;  // RTL 断言 >0
                else if (!w_enq_fire && w_deq_fire)
                    next.empties = st.empties + 1;  // RTL 断言 <size
                // VIP 表更新的打拍链
                next.upd_valid = enq.valid;
                next.upd_rel   = w_enq_rdy;
                if (enq.valid) next.upd_tag = w_tag;  // RegEnable 语义
                return next;
            };

        vip_table.update.assign().reads(st) = [](auto src) {
            auto [st] = src;
            VipUpd u;
            u.valid = st.upd_valid;
            u.rel   = st.upd_rel;
            u.tag   = st.upd_tag;
            return u;
        };
    }
};

}  // namespace zj::ring
