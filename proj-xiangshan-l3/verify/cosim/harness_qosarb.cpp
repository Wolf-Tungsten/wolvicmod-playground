// Qos 仲裁（dongjiang FastArb.scala 的 ArbiterGenerator）对拍 harness：
// QosRRArb ↔ fastQosRRArb、QosFixedArb ↔ fastQosArb，N=4，bits=(qos 4b, data 32b)。
// 激励：定向高/低 qos 混合（qos=0xf 抢占拍序）+ 全 valid 轮转 + 随机密度分段。
// 每拍比对 in_ready 掩码 / out_valid / out.qos / out.data（valid 时）。
// （ArbiterGenerator 无 chosen 端口，chosen 不参与对拍。）

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>

#include "VQosArbRef_n4_rr.h"
#include "VQosArbRef_n4_fx.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/prefab.h>
#include <prefab/prefab.h>

using namespace wolvicmod;
using namespace zj::prefab;

namespace {

// 与 refgen 的 QosBits Bundle 对应（qos 低 4 位有效）
struct QosBitsC {
    uint32_t qos = 0;
    uint32_t data = 0;

    bool operator==(const QosBitsC&) const = default;
};

template <class T>
struct QosOfC {
    uint8_t operator()(const T& t) const { return static_cast<uint8_t>(t.qos); }
};

template <class VRef, template <class, uint32_t, class> class QosArbT>
uint64_t cosimQosArb(const char* cfg, uint32_t seed, uint64_t cycles) {
    VRef ref;
    QosArbT<QosBitsC, 4, QosOfC<QosBitsC>> dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;
    constexpr uint32_t N = 4;
    using InArr = std::array<Dec<QosBitsC>, N>;

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
        uint8_t qosSel[N];
        if (c < directedEnd) {
            // 定向：全 valid 高低混合 / 仅高 qos 两路 / 仅低 qos / 全 valid 反压
            const uint64_t ph = (c / 8) % 4;
            if (ph == 0)      vm = 0b1111;                 // 全 valid，混合 qos
            else if (ph == 1) vm = 0b1010;                 // 两路
            else if (ph == 2) vm = 0b0101;
            else              vm = 0b1111;                 // 全 valid + 反压
            odr = (ph != 3);
            for (uint32_t i = 0; i < N; ++i)
                qosSel[i] = ((c / 4 + i) % 3 == 0) ? 0xf : static_cast<uint8_t>(i);
        } else {
            const uint32_t pct = cosim::densityAt(c - directedEnd, cycles - directedEnd);
            vm = 0;
            for (uint32_t i = 0; i < N; ++i) vm |= cosim::roll(rng, pct) ? (1ull << i) : 0;
            odr = cosim::roll(rng, pct);
            for (uint32_t i = 0; i < N; ++i)
                qosSel[i] = cosim::roll(rng, 25) ? 0xf : static_cast<uint8_t>(rng() % 15);
        }

        InArr ins{};
        for (uint32_t i = 0; i < N; ++i)
            ins[i] = {((vm >> i) & 1u) != 0, QosBitsC{qosSel[i], static_cast<uint32_t>(rng())}};

        ref.in_valid = vm;
        for (uint32_t i = 0; i < N; ++i) {
            switch (i) {
                case 0: ref.in_qos_0 = ins[0].bits.qos; ref.in_data_0 = ins[0].bits.data; break;
                case 1: ref.in_qos_1 = ins[1].bits.qos; ref.in_data_1 = ins[1].bits.data; break;
                case 2: ref.in_qos_2 = ins[2].bits.qos; ref.in_data_2 = ins[2].bits.data; break;
                case 3: ref.in_qos_3 = ins[3].bits.qos; ref.in_data_3 = ins[3].bits.data; break;
            }
        }
        ref.out_ready = odr;
        dut.in.set(ins);
        dut.out_rdy.set(odr);
        rp.push("vm=0x" + std::to_string(vm) + " odr=" + std::to_string(odr));

        cosim::phaseLow(ref, dut);
        // in_ready 为 Vec 输出（逐路端口 in_ready_i，N=4 固定）
        const uint64_t refRdy[4] = {ref.in_ready_0, ref.in_ready_1, ref.in_ready_2, ref.in_ready_3};
        for (uint32_t i = 0; i < N; ++i)
            cosim::check(st, "qosarb", cfg, seed, c, cosim::lanePort("in_ready", i).c_str(), refRdy[i], dut.in_rdy.get()[i], rp);
        cosim::check(st, "qosarb", cfg, seed, c, "out_valid", ref.out_valid, dut.out.get().valid, rp);
        if (ref.out_valid && dut.out.get().valid) {
            cosim::check(st, "qosarb", cfg, seed, c, "out_qos", ref.out_qos, dut.out.get().bits.qos, rp);
            cosim::check(st, "qosarb", cfg, seed, c, "out_data", ref.out_data, dut.out.get().bits.data, rp);
        }
        cosim::phaseHigh(ref, dut);
    }
    if (st.mismatches > 0) {
        std::cout << "FAIL qosarb " << cfg << " seed=" << seed
                  << " mismatches=" << st.mismatches << "/" << st.checks << "\n";
        return st.mismatches;
    }
    std::cout << "PASS qosarb " << cfg << " seed=" << seed << " cycles=" << cycles
              << " checks=" << st.checks << "\n";
    return 0;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {9u, 99u, 999u}) {
        bad += cosimQosArb<VQosArbRef_n4_rr, QosRRArb>("n4_rr", seed, 100000);
        bad += cosimQosArb<VQosArbRef_n4_fx, QosFixedArb>("n4_fx", seed, 100000);
    }
    std::cout << (bad ? "FAIL qosarb" : "ALL-PASS qosarb") << "\n";
    return bad ? 1 : 0;
}
