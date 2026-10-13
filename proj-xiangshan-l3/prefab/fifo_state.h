#pragma once

// FifoState：chisel3.util.Queue 语义的值类型版本（状态 + 纯函数），与
// wolvicmod::prefab::Queue 逐行对齐（语义出处见其头注释）。供扁平化模块把
// FIFO 降为普通状态成员，摆脱"模块壳 + 端口接线"的形态。
//
// 等价性担保：tests/test_prefab_fifo_state.cpp 对本族函数与 Queue 模块做
// 随机激励逐拍对拍（N=1/2/3/4、Flow/Pipe 各组合、空/满/反压角落）。
//
// 约定：所有函数为纯函数；ram 同址读写见旧值（next 基于 st 计算，与 chisel
// Mem 的 SyncReadMem 语义一致——Queue 头注释 Pipe 段）。

#include <array>
#include <cstdint>

#include "wolvicmod/prefab/valid.h"

namespace zj::prefab {

using wolvicmod::prefab::Valid;

template <class T, uint32_t N, bool Flow = false, bool Pipe = false>
struct FifoState {
    static_assert(N >= 1, "FifoState requires N >= 1");
    std::array<T, N> ram{};
    uint32_t         enq_ptr    = 0;
    uint32_t         deq_ptr    = 0;
    bool             maybe_full = false;

    bool operator==(const FifoState&) const = default;
};

template <class T, uint32_t N, bool Flow, bool Pipe>
bool fifoEmpty(const FifoState<T, N, Flow, Pipe>& s) {
    return s.enq_ptr == s.deq_ptr && !s.maybe_full;
}

template <class T, uint32_t N, bool Flow, bool Pipe>
bool fifoFull(const FifoState<T, N, Flow, Pipe>& s) {
    return s.enq_ptr == s.deq_ptr && s.maybe_full;
}

// chisel enq.ready：pipe 时满且 deq_rdy 同拍放行
template <class T, uint32_t N, bool Flow, bool Pipe>
bool fifoEnqRdy(const FifoState<T, N, Flow, Pipe>& s, bool deq_rdy) {
    if constexpr (Pipe)
        return !fifoFull(s) || deq_rdy;
    else
        return !fifoFull(s);
}

// chisel deq 输出：flow 时空直通 enq
template <class T, uint32_t N, bool Flow, bool Pipe>
Valid<T> fifoDeq(const FifoState<T, N, Flow, Pipe>& s, const Valid<T>& enq) {
    Valid<T> o;
    if constexpr (Flow) {
        const bool empty = fifoEmpty(s);
        o.valid          = !empty || enq.valid;
        o.bits           = empty ? enq.bits : s.ram[s.deq_ptr];
    } else {
        o.valid = !fifoEmpty(s);
        o.bits  = s.ram[s.deq_ptr];
    }
    return o;
}

// chisel count：ptr_match 时 0 或 N；否则 (enq_ptr - deq_ptr) mod N
template <class T, uint32_t N, bool Flow, bool Pipe>
uint32_t fifoCount(const FifoState<T, N, Flow, Pipe>& s) {
    if (s.enq_ptr == s.deq_ptr) return s.maybe_full ? N : 0u;
    return s.enq_ptr > s.deq_ptr ? s.enq_ptr - s.deq_ptr : s.enq_ptr + N - s.deq_ptr;
}

struct FifoFire {
    bool do_enq, do_deq;
};

// chisel fire 判定；flow 在 empty 分支强制 do_deq=false、deq.ready 时 do_enq=false
// （空直通被消费的那拍队列内部不动，不留幻影拷贝）。
template <class T, uint32_t N, bool Flow, bool Pipe>
FifoFire fifoFire(const FifoState<T, N, Flow, Pipe>& s, bool enq_valid, bool deq_rdy) {
    const bool empty  = fifoEmpty(s);
    const bool rdy    = fifoEnqRdy<T, N, Flow, Pipe>(s, deq_rdy);
    bool       do_enq = enq_valid && rdy;
    bool       do_deq = deq_rdy && (Flow ? (!empty || enq_valid) : !empty);
    if constexpr (Flow) {
        if (empty) {
            do_deq = false;
            if (deq_rdy) do_enq = false;
        }
    }
    return {do_enq, do_deq};
}

template <class T, uint32_t N, bool Flow, bool Pipe>
FifoState<T, N, Flow, Pipe> fifoNext(const FifoState<T, N, Flow, Pipe>& s, const T& enq_bits,
                                     bool enq_valid, bool deq_rdy) {
    FifoState<T, N, Flow, Pipe> n = s;
    const auto                  f = fifoFire(s, enq_valid, deq_rdy);
    if (f.do_enq) {
        n.ram[s.enq_ptr] = enq_bits;
        n.enq_ptr        = s.enq_ptr + 1 == N ? 0u : s.enq_ptr + 1;
    }
    if (f.do_deq) n.deq_ptr = s.deq_ptr + 1 == N ? 0u : s.deq_ptr + 1;
    if (f.do_enq != f.do_deq) n.maybe_full = f.do_enq;
    return n;
}

}  // namespace zj::prefab
