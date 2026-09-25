// Alloc（dongjiang utils/Alloc.scala）对拍 harness：N=4、16。
// 激励：定向全空闲/全忙/单空闲扫掠 + 随机 out_rdy 掩码密度分段。
// 每拍比对 in_ready / out_valid 掩码 / out.bits（对应 valid 时）。
// （freeId 是参考侧内部信号非端口，不参与对拍；预制菜侧 free_id 由单测覆盖。）

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>

#include "VAllocRef_n4.h"
#include "VAllocRef_n16.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/prefab.h>
#include <prefab/prefab.h>

using namespace wolvicmod;
using namespace zj::prefab;

namespace {

template <class VRef, uint32_t N>
uint64_t cosimAlloc(const char* cfg, uint32_t seed, uint64_t cycles) {
    VRef ref;
    Alloc<uint32_t, N> dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;

    cosim::resetRef(ref, [&] {
        ref.in_valid = 0;
        ref.in_bits = 0;
        ref.out_ready = 0;
    });
    dut.in.set({false, 0});
    dut.out_rdy.set({});
    dut.clk.set(0);
    dut.eval();

    const uint64_t directedEnd = cycles / 5;
    for (uint64_t c = 0; c < cycles; ++c) {
        uint64_t rm;   // out_rdy 掩码
        bool iv;
        if (c < directedEnd) {
            const uint64_t ph = (c / 4) % 4;
            if (ph == 0)      rm = (N >= 64 ? ~0ull : ((1ull << N) - 1u));  // 全空闲
            else if (ph == 1) rm = 0;                                        // 全忙
            else if (ph == 2) rm = 1ull << ((c / 4) % N);                    // 单空闲轮换
            else              rm = ((1ull << N) - 1u) & ~1ull;               // 除 0 外空闲
            iv = (c % 4) != 1;   // in.valid 间歇
        } else {
            const uint32_t pct = cosim::densityAt(c - directedEnd, cycles - directedEnd);
            rm = 0;
            for (uint32_t i = 0; i < N; ++i) rm |= cosim::roll(rng, pct) ? (1ull << i) : 0;
            iv = cosim::roll(rng, pct);
        }
        const uint32_t bits = rng();

        ref.in_valid = iv;
        ref.in_bits = bits;
        ref.out_ready = rm;
        dut.in.set({iv, bits});
        std::array<bool, N> rdy{};
        for (uint32_t i = 0; i < N; ++i) rdy[i] = ((rm >> i) & 1u) != 0;
        dut.out_rdy.set(rdy);
        rp.push("iv=" + std::to_string(iv) + " rm=0x" + std::to_string(rm));

        cosim::phaseLow(ref, dut);
        cosim::check(st, "alloc", cfg, seed, c, "in_ready", ref.in_ready, dut.in_rdy.get(), rp);
        // out_valid / out_bits 为 Vec 输出（逐路端口），按 N 用 if constexpr 保护引用
        auto refValid = [&](uint32_t i) -> uint64_t {
            switch (i) {
                case 0: return ref.out_valid_0;
                case 1: return ref.out_valid_1;
                case 2: return ref.out_valid_2;
                case 3: return ref.out_valid_3;
                default: break;
            }
            if constexpr (N >= 16) {
                switch (i) {
                    case 4: return ref.out_valid_4;
                    case 5: return ref.out_valid_5;
                    case 6: return ref.out_valid_6;
                    case 7: return ref.out_valid_7;
                    case 8: return ref.out_valid_8;
                    case 9: return ref.out_valid_9;
                    case 10: return ref.out_valid_10;
                    case 11: return ref.out_valid_11;
                    case 12: return ref.out_valid_12;
                    case 13: return ref.out_valid_13;
                    case 14: return ref.out_valid_14;
                    case 15: return ref.out_valid_15;
                    default: break;
                }
            }
            return 0;
        };
        auto refBits = [&](uint32_t i) -> uint32_t {
            switch (i) {
                case 0: return ref.out_bits_0;
                case 1: return ref.out_bits_1;
                case 2: return ref.out_bits_2;
                case 3: return ref.out_bits_3;
                default: break;
            }
            if constexpr (N >= 16) {
                switch (i) {
                    case 4: return ref.out_bits_4;
                    case 5: return ref.out_bits_5;
                    case 6: return ref.out_bits_6;
                    case 7: return ref.out_bits_7;
                    case 8: return ref.out_bits_8;
                    case 9: return ref.out_bits_9;
                    case 10: return ref.out_bits_10;
                    case 11: return ref.out_bits_11;
                    case 12: return ref.out_bits_12;
                    case 13: return ref.out_bits_13;
                    case 14: return ref.out_bits_14;
                    case 15: return ref.out_bits_15;
                    default: break;
                }
            }
            return 0;
        };
        for (uint32_t i = 0; i < N; ++i) {
            const bool rv = refValid(i) != 0;
            const bool dv = dut.out.get()[i].valid;
            cosim::check(st, "alloc", cfg, seed, c, cosim::lanePort("out_valid", i).c_str(), rv, dv, rp);
            if (rv && dv)
                cosim::check(st, "alloc", cfg, seed, c, cosim::lanePort("out_bits", i).c_str(), refBits(i), dut.out.get()[i].bits, rp);
        }
        cosim::phaseHigh(ref, dut);
    }
    if (st.mismatches > 0) {
        std::cout << "FAIL alloc " << cfg << " seed=" << seed
                  << " mismatches=" << st.mismatches << "/" << st.checks << "\n";
        return st.mismatches;
    }
    std::cout << "PASS alloc " << cfg << " seed=" << seed << " cycles=" << cycles
              << " checks=" << st.checks << "\n";
    return 0;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {3u, 13u, 31u}) {
        bad += cosimAlloc<VAllocRef_n4, 4>("n4", seed, 100000);
        bad += cosimAlloc<VAllocRef_n16, 16>("n16", seed, 100000);
    }
    std::cout << (bad ? "FAIL alloc" : "ALL-PASS alloc") << "\n";
    return bad ? 1 : 0;
}
