// DpSram（xs-utils DualPortSramTemplate）对拍 harness：3 个配置——
// replArray(bypass,setup=1,latency=1,+outReg,ways=1)、(nobypass)、(bypass,ways=2)。
// 激励：随机独立读写 + 定向同址同拍读写冲突（bypass 写优先 / nobypass 读旧值）+
// 连续写后跟读。每拍比对 wreq_ready / rreq_ready / rresp_valid / rresp.data
//（valid 时）。

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>

#include "VDpSramRef_s16w1_b1_su1_l1_o1.h"
#include "VDpSramRef_s16w1_b0_su1_l1_o1.h"
#include "VDpSramRef_s16w2_b1_su1_l1_o1.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/prefab.h>
#include <prefab/prefab.h>

using namespace wolvicmod;
using namespace zj::prefab;

namespace {

template <class VRef, uint32_t Sets, uint32_t Ways, bool Bypass>
uint64_t cosimDpSram(const char* cfg, uint32_t seed, uint64_t cycles) {
    VRef ref;
    DpSram<uint32_t, Sets, Ways, Bypass, 1, 1, false, true, false> dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;
    using Wr = SramReqBits<uint32_t, Ways>;

    cosim::resetRef(ref, [&] {
        ref.wreq_valid = 0;
        ref.wreq_addr = 0;
        if constexpr (Ways > 1) ref.wreq_mask = 0;
        ref.wreq_data_0 = 0;
        if constexpr (Ways > 1) ref.wreq_data_1 = 0;
        ref.rreq_valid = 0;
        ref.rreq_addr = 0;
    });
    dut.wreq.set({false, {}});
    dut.rreq.set({false, 0});
    dut.clk.set(0);
    dut.eval();

    const uint64_t directedEnd = cycles / 5;
    for (uint64_t c = 0; c < cycles; ++c) {
        bool wv, rv;
        uint32_t wa, ra, mask = 0;
        if (c < directedEnd) {
            const uint64_t ph = (c / 2) % 4;
            if (ph == 0) {          // 写
                wv = true; rv = false;
                wa = (c / 8) % Sets;
                ra = 0;
                mask = (Ways > 1) ? (1u << ((c / 2) % Ways)) : 0;
            } else if (ph == 1) {   // 读同址（刚写的）
                wv = false; rv = true;
                wa = 0;
                ra = (c / 8) % Sets;
            } else if (ph == 2) {   // 同址同拍读写冲突
                wv = true; rv = true;
                wa = (c / 8) % Sets;
                ra = wa;
                mask = (Ways > 1) ? static_cast<uint32_t>((1u << Ways) - 1u) : 0;
            } else {                // 随机组合
                wv = (c % 2) == 0; rv = true;
                wa = rng() % Sets;
                ra = rng() % Sets;
                mask = (Ways > 1) ? static_cast<uint32_t>((1u << Ways) - 1u) : 0;
            }
        } else {
            const uint32_t pct = cosim::densityAt(c - directedEnd, cycles - directedEnd);
            wv = cosim::roll(rng, pct);
            rv = cosim::roll(rng, pct);
            wa = rng() % Sets;
            ra = rng() % Sets;
            mask = (Ways > 1) ? static_cast<uint32_t>(rng() % ((1u << Ways) + 1u)) : 0;
            if (Ways > 1 && mask == 0) mask = 1;
        }
        uint32_t d0 = rng(), d1 = rng();

        ref.wreq_valid = wv;
        ref.wreq_addr = wa;
        if constexpr (Ways > 1) ref.wreq_mask = mask;
        ref.wreq_data_0 = d0;
        if constexpr (Ways > 1) ref.wreq_data_1 = d1;
        ref.rreq_valid = rv;
        ref.rreq_addr = ra;
        Wr wb{};
        wb.write = true;
        wb.addr = wa;
        wb.mask = mask;
        wb.data[0] = d0;
        if constexpr (Ways > 1) wb.data[1] = d1;
        dut.wreq.set({wv, wb});
        dut.rreq.set({rv, ra});
        rp.push("wv=" + std::to_string(wv) + " wa=" + std::to_string(wa) +
                " rv=" + std::to_string(rv) + " ra=" + std::to_string(ra) +
                " mask=" + std::to_string(mask));

        cosim::phaseLow(ref, dut);
        cosim::check(st, "dpsram", cfg, seed, c, "wreq_ready", ref.wreq_ready, dut.wreq_rdy.get(), rp);
        cosim::check(st, "dpsram", cfg, seed, c, "rreq_ready", ref.rreq_ready, dut.rreq_rdy.get(), rp);
        cosim::check(st, "dpsram", cfg, seed, c, "rresp_valid", ref.rresp_valid, dut.rresp.get().valid, rp);
        if (ref.rresp_valid && dut.rresp.get().valid) {
            cosim::check(st, "dpsram", cfg, seed, c, "rresp_data_0", ref.rresp_data_0,
                         dut.rresp.get().bits.data[0], rp);
            if constexpr (Ways > 1)
                cosim::check(st, "dpsram", cfg, seed, c, "rresp_data_1", ref.rresp_data_1,
                             dut.rresp.get().bits.data[1], rp);
        }
        cosim::phaseHigh(ref, dut);
    }
    if (st.mismatches > 0) {
        std::cout << "FAIL dpsram " << cfg << " seed=" << seed
                  << " mismatches=" << st.mismatches << "/" << st.checks << "\n";
        return st.mismatches;
    }
    std::cout << "PASS dpsram " << cfg << " seed=" << seed << " cycles=" << cycles
              << " checks=" << st.checks << "\n";
    return 0;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {19u, 91u, 191u}) {
        bad += cosimDpSram<VDpSramRef_s16w1_b1_su1_l1_o1, 16, 1, true>("b1_w1", seed, 100000);
        bad += cosimDpSram<VDpSramRef_s16w1_b0_su1_l1_o1, 16, 1, false>("b0_w1", seed, 100000);
        bad += cosimDpSram<VDpSramRef_s16w2_b1_su1_l1_o1, 16, 2, true>("b1_w2", seed, 100000);
    }
    std::cout << (bad ? "FAIL dpsram" : "ALL-PASS dpsram") << "\n";
    return bad ? 1 : 0;
}
