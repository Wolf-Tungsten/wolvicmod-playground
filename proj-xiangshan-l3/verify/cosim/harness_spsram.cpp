// SpSram（xs-utils SinglePortSramTemplate）对拍 harness：4 个配置——
// Directory(setup=1,latency=2,+outReg,ways=2)、BeatStorage(2,2,+outReg,ways=1)、
// basic(1,1,无 outReg,ways=2)、shouldReset(1,1,+outReg,sets=4，验证横扫回压)。
// 激励：随机读写混合（valid 顶着 rdy 压满窗口）+ 定向写后跟读、掩码扫掠。
// 每拍比对 req_ready / resp_valid / resp.data（valid 时）。

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>

#include "VSpSramRef_s16w2_r0_su1_l2_o1.h"
#include "VSpSramRef_s16w1_r0_su2_l2_o1.h"
#include "VSpSramRef_s16w2_r0_su1_l1_o0.h"
#include "VSpSramRef_s4w1_r1_su1_l1_o1.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/prefab.h>
#include <prefab/prefab.h>

using namespace wolvicmod;
using namespace zj::prefab;

namespace {

template <class VRef, uint32_t Sets, uint32_t Ways, uint32_t Setup, uint32_t Latency,
          bool OutputReg, bool ShouldReset>
uint64_t cosimSpSram(const char* cfg, uint32_t seed, uint64_t cycles) {
    VRef ref;
    SpSram<uint32_t, Sets, Ways, Setup, Latency, false, OutputReg, ShouldReset> dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;
    using Req = SramReqBits<uint32_t, Ways>;

    cosim::resetRef(ref, [&] {
        ref.req_valid = 0;
        ref.req_addr = 0;
        ref.req_write = 0;
        if constexpr (Ways > 1) ref.req_mask = 0;
        ref.req_data_0 = 0;
        if constexpr (Ways > 1) ref.req_data_1 = 0;
    });
    dut.req.set({false, {}});
    dut.clk.set(0);
    dut.eval();

    const uint64_t directedEnd = cycles / 5;
    uint32_t lastWrAddr = 0;
    for (uint64_t c = 0; c < cycles; ++c) {
        bool v, wr;
        uint32_t addr, mask = 0;
        if (c < directedEnd) {
            // 定向：写后跟读同址 / 掩码扫掠 / 连续写后连续读
            const uint64_t ph = (c / 2) % 4;
            if (ph == 0) {          // 写
                v = true; wr = true;
                addr = (c / 8) % Sets;
                mask = (Ways > 1) ? (1u << ((c / 2) % Ways)) : 0;
            } else if (ph == 1) {   // 紧接着读同址
                v = true; wr = false;
                addr = (c / 8) % Sets;
            } else if (ph == 2) {   // 随机写
                v = true; wr = true;
                addr = rng() % Sets;
                mask = (Ways > 1) ? static_cast<uint32_t>((1u << Ways) - 1u) : 0;
            } else {                // 随机读
                v = true; wr = false;
                addr = rng() % Sets;
            }
            if (ph == 0) lastWrAddr = addr;
        } else {
            const uint32_t pct = cosim::densityAt(c - directedEnd, cycles - directedEnd);
            v = cosim::roll(rng, pct);
            wr = cosim::roll(rng, 40);
            addr = rng() % Sets;
            mask = (Ways > 1) ? static_cast<uint32_t>(rng() % ((1u << Ways) + 1u)) : 0;
            if (Ways > 1 && mask == 0) mask = 1;  // 掩码非零
        }
        uint32_t d0 = rng(), d1 = rng();

        ref.req_valid = v;
        ref.req_addr = addr;
        ref.req_write = wr;
        if constexpr (Ways > 1) ref.req_mask = mask;
        ref.req_data_0 = d0;
        if constexpr (Ways > 1) ref.req_data_1 = d1;
        Req rb{};
        rb.write = wr;
        rb.addr = addr;
        rb.mask = mask;
        rb.data[0] = d0;
        if constexpr (Ways > 1) rb.data[1] = d1;
        dut.req.set({v, rb});
        rp.push("v=" + std::to_string(v) + " wr=" + std::to_string(wr) +
                " addr=" + std::to_string(addr) + " mask=" + std::to_string(mask));

        cosim::phaseLow(ref, dut);
        cosim::check(st, "spsram", cfg, seed, c, "req_ready", ref.req_ready, dut.req_rdy.get(), rp);
        cosim::check(st, "spsram", cfg, seed, c, "resp_valid", ref.resp_valid, dut.resp.get().valid, rp);
        if (ref.resp_valid && dut.resp.get().valid) {
            cosim::check(st, "spsram", cfg, seed, c, "resp_data_0", ref.resp_data_0,
                         dut.resp.get().bits.data[0], rp);
            if constexpr (Ways > 1)
                cosim::check(st, "spsram", cfg, seed, c, "resp_data_1", ref.resp_data_1,
                             dut.resp.get().bits.data[1], rp);
        }
        cosim::phaseHigh(ref, dut);
    }
    if (st.mismatches > 0) {
        std::cout << "FAIL spsram " << cfg << " seed=" << seed
                  << " mismatches=" << st.mismatches << "/" << st.checks << "\n";
        return st.mismatches;
    }
    std::cout << "PASS spsram " << cfg << " seed=" << seed << " cycles=" << cycles
              << " checks=" << st.checks << "\n";
    return 0;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {17u, 71u, 137u}) {
        bad += cosimSpSram<VSpSramRef_s16w2_r0_su1_l2_o1, 16, 2, 1, 2, true, false>(
            "dir_su1l2o1w2", seed, 100000);
        bad += cosimSpSram<VSpSramRef_s16w1_r0_su2_l2_o1, 16, 1, 2, 2, true, false>(
            "ds_su2l2o1w1", seed, 100000);
        bad += cosimSpSram<VSpSramRef_s16w2_r0_su1_l1_o0, 16, 2, 1, 1, false, false>(
            "basic_su1l1o0w2", seed, 100000);
        bad += cosimSpSram<VSpSramRef_s4w1_r1_su1_l1_o1, 4, 1, 1, 1, true, true>(
            "rst_s4w1", seed, 100000);
    }
    std::cout << (bad ? "FAIL spsram" : "ALL-PASS spsram") << "\n";
    return bad ? 1 : 0;
}
