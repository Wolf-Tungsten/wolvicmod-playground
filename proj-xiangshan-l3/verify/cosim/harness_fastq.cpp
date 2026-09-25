// FastQueue（xs-utils queue/FastQueue.scala）对拍 harness：N=2、N=4（NoX=false）、
// N=2 NoX=true。激励：定向灌满→排空振荡（含满时同拍推拉/气泡拍序）+ 随机密度
// 分段。每拍比对 enq_ready / deq_valid / deq.bits / count / free_num。
// NoX=true 配置 deq.bits 在无效时为 0（两侧约定一致，仍按 valid 门控比对）。

#include <cstdint>
#include <iostream>
#include <random>
#include <string>

#include "VFastQueueRef_s2_x0.h"
#include "VFastQueueRef_s4_x0.h"
#include "VFastQueueRef_s2_x1.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/prefab.h>
#include <prefab/prefab.h>

using namespace wolvicmod;
using namespace zj::prefab;

namespace {

template <class VRef, uint32_t N, bool NoX>
uint64_t cosimFastQueue(const char* cfg, uint32_t seed, uint64_t cycles) {
    VRef ref;
    FastQueue<uint32_t, N, NoX> dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;

    cosim::resetRef(ref, [&] {
        ref.enq_valid = 0;
        ref.enq_bits = 0;
        ref.deq_ready = 0;
    });
    dut.enq.set({false, 0});
    dut.deq_rdy.set(false);
    dut.clk.set(0);
    dut.eval();

    const uint64_t directedEnd = cycles / 5;
    bool filling = true;
    for (uint64_t c = 0; c < cycles; ++c) {
        bool ev, dr;
        if (c < directedEnd) {
            const uint32_t cnt = dut.count.get();
            if (filling) {
                if (cnt == N) {
                    // 满：enq 顶着 valid 不放（寄存 ready 的气泡拍序），deq 排空
                    ev = (c % 2) == 0;   // 顶着 valid 试探 enq_rdy 恢复拍
                    dr = true;
                    filling = false;
                } else {
                    ev = true;
                    dr = false;
                }
            } else {
                if (cnt == 0) {
                    filling = true;
                    ev = true;
                    dr = (c % 2) == 0;
                } else {
                    ev = false;
                    dr = true;
                }
            }
        } else {
            const uint32_t pct = cosim::densityAt(c - directedEnd, cycles - directedEnd);
            ev = cosim::roll(rng, pct);
            dr = cosim::roll(rng, pct);
        }
        const uint32_t bits = rng();

        ref.enq_valid = ev;
        ref.enq_bits = bits;
        ref.deq_ready = dr;
        dut.enq.set({ev, bits});
        dut.deq_rdy.set(dr);
        rp.push("ev=" + std::to_string(ev) + " bits=0x" + [&] {
            char b[16];
            std::snprintf(b, sizeof b, "%08x", bits);
            return std::string(b);
        }() + " dr=" + std::to_string(dr));

        cosim::phaseLow(ref, dut);
        cosim::check(st, "fastq", cfg, seed, c, "enq_ready", ref.enq_ready, dut.enq_rdy.get(), rp);
        cosim::check(st, "fastq", cfg, seed, c, "deq_valid", ref.deq_valid, dut.deq.get().valid, rp);
        if (ref.deq_valid && dut.deq.get().valid)
            cosim::check(st, "fastq", cfg, seed, c, "deq_bits", ref.deq_bits, dut.deq.get().bits, rp);
        cosim::check(st, "fastq", cfg, seed, c, "count", ref.count, dut.count.get(), rp);
        cosim::check(st, "fastq", cfg, seed, c, "free_num", ref.free_num, dut.free_num.get(), rp);
        cosim::phaseHigh(ref, dut);
    }
    if (st.mismatches > 0) {
        std::cout << "FAIL fastq " << cfg << " seed=" << seed
                  << " mismatches=" << st.mismatches << "/" << st.checks << "\n";
        return st.mismatches;
    }
    std::cout << "PASS fastq " << cfg << " seed=" << seed << " cycles=" << cycles
              << " checks=" << st.checks << "\n";
    return 0;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {101u, 202u, 303u}) {
        bad += cosimFastQueue<VFastQueueRef_s2_x0, 2, false>("s2_x0", seed, 120000);
        bad += cosimFastQueue<VFastQueueRef_s4_x0, 4, false>("s4_x0", seed, 120000);
        bad += cosimFastQueue<VFastQueueRef_s2_x1, 2, true>("s2_x1", seed, 120000);
    }
    std::cout << (bad ? "FAIL fastq" : "ALL-PASS fastq") << "\n";
    return bad ? 1 : 0;
}
