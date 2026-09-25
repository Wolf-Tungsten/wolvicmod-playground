#pragma once

// FastQueue<T, N>：xs-utils FastQueue（queue/FastQueue.scala，N≥2）的拍级对齐
// 实现——移位队列而非循环队列：项永远压实占据 array[0..count-1]。
//
// 与参考 RTL 的状态对应关系：RTL 的 valids(N) + array(N) + enqRdyReg 在本实现
// 中按"压实不变式"合并为 {array, count, enq_rdy}（count == PopCount(valids)）：
//   deq.valid = count > 0（组合）；deq.bits = array[0]
//   enq_rdy 输出是寄存的 enq_rdy（RegInit(true.B)）——这是与 chisel pipe Queue
//   的本质差别：满 → deq 后下一拍 ready 才恢复，有一拍气泡。
// 一个周期内（enqF = enq.valid && enq_rdy，deqF = count>0 && deq_rdy）：
//   enqF && deqF：低 count-1 项下移一格，array[count-1] ← enq.bits，count 不变
//   仅 deqF     ：整体下移一格，count--，enq_rdy ← true
//   仅 enqF     ：array[count] ← enq.bits，count++，enq_rdy ← (当前 count < N-1)
//   否则        ：保持（RTL 中 enqRdyReg 仅在恰好一侧 fire 时更新）
// NoX=true 对应 RTL 的 deqDataNoX：deq 无效时 bits 输出零值而非陈旧数据。

#include <array>
#include <cstdint>

#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/dec.h"

namespace zj::prefab {

using wolvicmod::prefab::Dec;

template <class T, uint32_t N, bool NoX = false>
class FastQueue : public wolvicmod::Module {
public:
    static_assert(N >= 2, "FastQueue requires N >= 2");
    using DecT = Dec<T>;

    struct FqState {
        std::array<T, N> array{};
        uint32_t count = 0;
        bool enq_rdy = true;  // RegInit(true.B)

        bool operator==(const FqState&) const = default;
    };

    IN(bool, clk);
    IN(DecT, enq);
    OUT(bool, enq_rdy);
    OUT(DecT, deq);
    IN(bool, deq_rdy);
    OUT(uint32_t, count);
    OUT(uint32_t, free_num);

    REG(FqState, st);
    WIRE(bool, w_enq_fire);
    WIRE(bool, w_deq_fire);

    FastQueue() {
        w_enq_fire.assign().reads(enq, st) = [](auto src) {
            auto [enq, st] = src;
            return enq.valid && st.enq_rdy;
        };
        w_deq_fire.assign().reads(st, deq_rdy) = [](auto src) {
            auto [st, deq_rdy] = src;
            return st.count > 0 && deq_rdy;
        };

        deq.assign().reads(st) = [](auto src) {
            auto [st] = src;
            DecT o;
            o.valid = st.count > 0;
            if constexpr (NoX) {
                o.bits = st.count > 0 ? st.array[0] : T{};
            } else {
                o.bits = st.array[0];
            }
            return o;
        };
        enq_rdy.assign().reads(st) = [](auto src) {
            auto [st] = src;
            return st.enq_rdy;
        };
        count.assign().reads(st) = [](auto src) {
            auto [st] = src;
            return st.count;
        };
        free_num.assign().reads(st) = [](auto src) {
            auto [st] = src;
            return N - st.count;
        };

        st.update().on(wolvicmod::posedge(clk)).reads(st, enq, w_enq_fire, w_deq_fire) = [](auto src) {
            auto [st, enq, w_enq_fire, w_deq_fire] = src;
            FqState next = st;
            if (w_enq_fire && w_deq_fire) {
                // 出 array[0]、低位下移、新项落在 array[count-1]；enq_rdy 保持
                for (uint32_t i = 0; i + 1 < st.count; ++i) next.array[i] = st.array[i + 1];
                next.array[st.count - 1] = enq.bits;
            } else if (w_deq_fire) {
                for (uint32_t i = 0; i + 1 < st.count; ++i) next.array[i] = st.array[i + 1];
                next.count = st.count - 1;
                next.enq_rdy = true;
            } else if (w_enq_fire) {
                next.array[st.count] = enq.bits;
                next.count = st.count + 1;
                next.enq_rdy = st.count < N - 1;  // RTL 的 !valids(N-2)，读当前 count
            }
            return next;
        };
    }
};

}  // namespace zj::prefab
