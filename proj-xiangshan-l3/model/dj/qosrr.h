#pragma once

// QosRRArb：fastQosRRArb 同构（dongjiang/utils/FastArb.scala，rr=true,qos=true）。
// 两层 VipArbiter（RR）：高优层只收 qos==0xf 的输入，有候选时低优层停摆。

#include <array>
#include <cstdint>

#include "prefab/xsarb.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::dj {

template <class T, uint32_t N>
class QosRRArb : public wolvicmod::Module {
public:
    using ValidT = wolvicmod::prefab::Valid<T>;
    using InArr = std::array<ValidT, N>;
    using RdyArr = std::array<bool, N>;
    using ArbT = zj::prefab::VipArb<T, N>;

    IN(bool, clk);
    IN(InArr, in);
    OUT(RdyArr, in_rdy);
    OUT(ValidT, out);
    IN(bool, out_rdy);

    MOD(ArbT, hi_arb);
    MOD(ArbT, lo_arb);
    WIRE(bool, w_has_high);

    QosRRArb() {
        hi_arb.clk = clk;
        lo_arb.clk = clk;
        hi_arb.in.assign().reads(in) = [](auto src) {
            auto [in] = src;
            InArr a;
            for (uint32_t i = 0; i < N; ++i) {
                a[i].valid = in[i].valid && in[i].bits.qos == 0xF;
                a[i].bits = in[i].bits;
            }
            return a;
        };
        lo_arb.in = in;
        w_has_high.assign().reads(hi_arb.out) = [](auto src) {
            auto [hi_out] = src;
            return hi_out.valid;
        };
        out.assign().reads(w_has_high, hi_arb.out, lo_arb.out) = [](auto src) {
            auto [w_has_high, hi_out, lo_out] = src;
            return w_has_high ? hi_out : lo_out;
        };
        hi_arb.out_rdy.assign().reads(w_has_high, out_rdy) = [](auto src) {
            auto [w_has_high, out_rdy] = src;
            return w_has_high && out_rdy;
        };
        lo_arb.out_rdy.assign().reads(w_has_high, out_rdy) = [](auto src) {
            auto [w_has_high, out_rdy] = src;
            return !w_has_high && out_rdy;
        };
        in_rdy.assign().reads(hi_arb.in_rdy, lo_arb.in_rdy) = [](auto src) {
            auto [hi_rdy, lo_rdy] = src;
            RdyArr r;
            for (uint32_t i = 0; i < N; ++i) r[i] = hi_rdy[i] || lo_rdy[i];
            return r;
        };
    }
};

}  // namespace zj::dj
