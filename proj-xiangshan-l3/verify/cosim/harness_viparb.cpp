// VipArb（xs-utils arb/VipArbiter.scala）对拍 harness：N=2/4/8。
// 激励：定向全 valid 连发（vip 轮转/粘性拍序）+ 随机掩码密度分段 +
// 稀疏单路。每拍比对 in_ready 掩码 / out_valid / out.bits(valid 时) / chosen。

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>

#include "VVipArbRef_n2.h"
#include "VVipArbRef_n4.h"
#include "VVipArbRef_n8.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/prefab.h>
#include <prefab/prefab.h>

using namespace wolvicmod;
using namespace zj::prefab;

namespace {

template <class VRef, uint32_t N>
uint64_t cosimVipArb(const char* cfg, uint32_t seed, uint64_t cycles) {
    VRef ref;
    VipArb<uint32_t, N> dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;
    using InArr = std::array<Dec<uint32_t>, N>;

    cosim::resetRef(ref, [&] {
        ref.in_valid = 0;
        ref.out_ready = 0;
    });
    {
        InArr ins{};
        dut.in.set(ins);
    }
    dut.out_rdy.set(false);
    dut.clk.set(0);
    dut.eval();

    const uint64_t directedEnd = cycles / 5;
    for (uint64_t c = 0; c < cycles; ++c) {
        uint64_t vm;
        bool odr;
        if (c < directedEnd) {
            // 定向：全 valid 连发与单路稀疏交替，中间插反压
            const uint64_t ph = (c / 8) % 4;
            if (ph == 0)      vm = (1u << N) - 1u;          // 全 valid
            else if (ph == 1) vm = (1u << N) - 1u;          // 全 valid + 反压
            else if (ph == 2) vm = 1u << ((c / 8) % N);     // 单路轮换
            else              vm = ((1u << N) - 1u) & ~1u;  // 除 0 路外全 valid
            odr = (ph != 1);
        } else {
            const uint32_t pct = cosim::densityAt(c - directedEnd, cycles - directedEnd);
            vm = 0;
            for (uint32_t i = 0; i < N; ++i) vm |= cosim::roll(rng, pct) ? (1ull << i) : 0;
            odr = cosim::roll(rng, pct);
        }

        InArr ins{};
        for (uint32_t i = 0; i < N; ++i) ins[i] = {((vm >> i) & 1u) != 0, static_cast<uint32_t>(rng())};

        ref.in_valid = vm;
        ref.in_bits_0 = ins[0].bits;
        ref.in_bits_1 = ins[1].bits;
        if constexpr (N >= 4) {
            ref.in_bits_2 = ins[2].bits;
            ref.in_bits_3 = ins[3].bits;
        }
        if constexpr (N >= 8) {
            ref.in_bits_4 = ins[4].bits;
            ref.in_bits_5 = ins[5].bits;
            ref.in_bits_6 = ins[6].bits;
            ref.in_bits_7 = ins[7].bits;
        }
        ref.out_ready = odr;
        dut.in.set(ins);
        dut.out_rdy.set(odr);
        rp.push("vm=0x" + std::to_string(vm) + " odr=" + std::to_string(odr));

        cosim::phaseLow(ref, dut);
        // in_ready 为 Vec 输出（逐路端口 in_ready_i），按 N 用 if constexpr 保护引用
        auto refRdy = [&](uint32_t i) -> uint64_t {
            switch (i) {
                case 0: return ref.in_ready_0;
                case 1: return ref.in_ready_1;
                default: break;
            }
            if constexpr (N >= 4) {
                switch (i) {
                    case 2: return ref.in_ready_2;
                    case 3: return ref.in_ready_3;
                    default: break;
                }
            }
            if constexpr (N >= 8) {
                switch (i) {
                    case 4: return ref.in_ready_4;
                    case 5: return ref.in_ready_5;
                    case 6: return ref.in_ready_6;
                    case 7: return ref.in_ready_7;
                    default: break;
                }
            }
            return 0;
        };
        for (uint32_t i = 0; i < N; ++i)
            cosim::check(st, "viparb", cfg, seed, c, cosim::lanePort("in_ready", i).c_str(), refRdy(i), dut.in_rdy.get()[i], rp);
        cosim::check(st, "viparb", cfg, seed, c, "out_valid", ref.out_valid, dut.out.get().valid, rp);
        if (ref.out_valid && dut.out.get().valid)
            cosim::check(st, "viparb", cfg, seed, c, "out_bits", ref.out_bits, dut.out.get().bits, rp);
        cosim::check(st, "viparb", cfg, seed, c, "chosen", ref.chosen, dut.chosen.get(), rp);
        cosim::phaseHigh(ref, dut);
    }
    if (st.mismatches > 0) {
        std::cout << "FAIL viparb " << cfg << " seed=" << seed
                  << " mismatches=" << st.mismatches << "/" << st.checks << "\n";
        return st.mismatches;
    }
    std::cout << "PASS viparb " << cfg << " seed=" << seed << " cycles=" << cycles
              << " checks=" << st.checks << "\n";
    return 0;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {5u, 55u, 555u}) {
        bad += cosimVipArb<VVipArbRef_n2, 2>("n2", seed, 100000);
        bad += cosimVipArb<VVipArbRef_n4, 4>("n4", seed, 100000);
        bad += cosimVipArb<VVipArbRef_n8, 8>("n8", seed, 100000);
    }
    std::cout << (bad ? "FAIL viparb" : "ALL-PASS viparb") << "\n";
    return bad ? 1 : 0;
}
