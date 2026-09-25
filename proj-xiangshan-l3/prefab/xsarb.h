#pragma once

// XiangShan 生态仲裁器：VipArb（xs-utils VipArbiter）/ QosRRArb 与
// QosFixedArb（dongjiang FastArb 的 ArbiterGenerator）/ Alloc（dongjiang
// Alloc）。chisel3 标准库的 FixedArb/RRArb 在 wolvicmod 侧（prefab/arb.h），
// 本文件的 QosFixedArb 直接复用之。
//
// 统一端口形态（阵列端口）：输入侧一路 In<std::array<Dec<T>,N>> in + 一路
// Out<std::array<bool,N>> in_rdy；输出侧 Out<Dec<T>> out + In<bool> out_rdy；
// 另有 Out<uint32_t> chosen（当前授权索引）。clk 端口仅为接口统一；纯组合
// 元件（Alloc）不采样它。Alloc 方向相反的部分对应为：Out<std::array<Dec<T>,N>>
// out（N 路分发）+ In<std::array<bool,N>> out_rdy。

#include <array>
#include <cstdint>
#include <type_traits>

#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/arb.h"
#include "wolvicmod/prefab/dec.h"

namespace zj::prefab {

using wolvicmod::prefab::Dec;
using wolvicmod::prefab::FixedArb;

// ---------------- VipArb：xs-utils VipArbiter ----------------
// vip 指针初值 0（RegInit(1.U) 的 one-hot bit0 → 索引 0）。授权规则（组合）：
// vip 路 valid 时 vip 优先，否则最低 valid 索引优先。指针更新：
//   move = (存在 vip 以外的 valid) && (vip_sel ? out.fire : true)
//   next = vip 之上最低 valid（highValidMask 优先，PriorityEncoderOH 取低索引），
//          无则绕回 vip 之下的最低 valid（lowValidMask）
// 等效后果：连续 fire 时指针正向轮转（跳过无效路）；vip 请求但反压不 fire 时
// 指针不动（粘性）；vip 不请求时指针无条件移到下一个 valid——照源码实现。

template <class T, uint32_t N>
class VipArb : public wolvicmod::Module {
public:
    static_assert(N >= 1);
    using DecT = Dec<T>;
    using InArr = std::array<DecT, N>;  // 宏参数含逗号，先取别名
    using RdyArr = std::array<bool, N>;

    IN(bool, clk);
    IN(InArr, in);
    OUT(RdyArr, in_rdy);
    OUT(DecT, out);
    IN(bool, out_rdy);
    OUT(uint32_t, chosen);

    REG(uint32_t, vip);
    WIRE(bool, w_vip_req);
    WIRE(bool, w_other_v);
    WIRE(bool, w_out_fire);
    WIRE(bool, w_move);
    WIRE(uint32_t, w_next_vip);

    VipArb() {
        chosen.assign().reads(vip, in) = [](auto src) -> uint32_t {
            auto [vip, in] = src;
            if (in[vip].valid) return vip;
            for (uint32_t i = 0; i < N; ++i)
                if (in[i].valid) return i;
            return 0;  // OHToUInt(0) = 0
        };
        out.assign().reads(in, chosen) = [](auto src) {
            auto [in, chosen] = src;
            DecT o;
            for (uint32_t i = 0; i < N; ++i) o.valid = o.valid || in[i].valid;
            // chisel Mux1H(selPtrOH)：无授权时输出零值
            o.bits = o.valid ? in[chosen].bits : T{};
            return o;
        };
        in_rdy.assign().reads(in, chosen, out_rdy) = [](auto src) {
            auto [in, chosen, out_rdy] = src;
            RdyArr rdy{};
            for (uint32_t i = 0; i < N; ++i) rdy[i] = out_rdy && in[i].valid && chosen == i;
            return rdy;
        };
        w_vip_req.assign().reads(vip, in) = [](auto src) {
            auto [vip, in] = src;
            return in[vip].valid;
        };
        w_other_v.assign().reads(vip, in) = [](auto src) {
            auto [vip, in] = src;
            for (uint32_t i = 0; i < N; ++i)
                if (i != vip && in[i].valid) return true;
            return false;
        };
        w_next_vip.assign().reads(vip, in) = [](auto src) -> uint32_t {
            auto [vip, in] = src;
            // vip 之上最低 valid（highValidMask 优先），无则绕回 vip 之下最低 valid
            for (uint32_t i = vip + 1; i < N; ++i)
                if (in[i].valid) return i;
            for (uint32_t i = 0; i < vip; ++i)
                if (in[i].valid) return i;
            return vip;
        };
        w_out_fire.assign().reads(out, out_rdy) = [](auto src) {
            auto [out, out_rdy] = src;
            return out.valid && out_rdy;
        };
        w_move.assign().reads(w_other_v, w_vip_req, w_out_fire) = [](auto src) {
            auto [w_other_v, w_vip_req, w_out_fire] = src;
            return w_other_v && (w_vip_req ? w_out_fire : true);
        };
        vip.update().on(posedge(clk)).en(w_move).reads(w_next_vip) = [](auto src) {
            auto [w_next_vip] = src;
            return w_next_vip;
        };
    }
};

// ---------------- QosArb：dongjiang FastArb（ArbiterGenerator） ----------------
// 按 qos == 0xf 拆 high/low 两组：low 组 = 全部输入，high 组 = qos==0xf 的输入
// （valid 打掩、bits 直通）；两组各过一个子仲裁器。hasHigh 时 high 组获胜、
// low 组 out_rdy 拉低；否则反之。in_rdy[i] = low.in_rdy[i] || high.in_rdy[i]。
// 注意：FastArb.scala 的 rr=true 子仲裁器是 xs-utils 的 VipArbiter（不是
// chisel3 RRArbiter）——QosRRArb 因此以 VipArb 为默认子仲裁器；QosFixedArb
// 用 FixedArb（chisel3 Arbiter）。
//
// 同名解包约定的跨模块写法：读集里的子模块端口按层次路径展开绑定名
// （arb_hi.out → arb_hi_out，点替换为下划线）。

template <class T>
struct QosOf {
    uint8_t operator()(const T& t) const { return static_cast<uint8_t>(t.qos); }
};

template <class T, uint32_t N, class Qos = QosOf<T>,
          template <class, uint32_t> class SubArb = VipArb>
class QosArb : public wolvicmod::Module {
public:
    static_assert(N >= 1);
    static_assert(std::is_default_constructible_v<Qos>, "Qos must be default-constructible");
    using DecT = Dec<T>;
    using InArr = std::array<DecT, N>;  // 宏参数含逗号，先取别名
    using RdyArr = std::array<bool, N>;
    using Sub = SubArb<T, N>;

    IN(bool, clk);
    IN(InArr, in);
    OUT(RdyArr, in_rdy);
    OUT(DecT, out);
    IN(bool, out_rdy);
    OUT(uint32_t, chosen);

    WIRE(bool, w_has_high);
    SUB(Sub, arb_lo);
    SUB(Sub, arb_hi);

    QosArb() {
        arb_lo.clk = clk;
        arb_hi.clk = clk;
        arb_lo.in = in;  // low 组 = 全部输入
        arb_hi.in.assign().reads(in) = [](auto src) {
            auto [in] = src;
            InArr d{};
            for (uint32_t i = 0; i < N; ++i) {
                d[i].valid = in[i].valid && Qos{}(in[i].bits) == 0xf;
                d[i].bits = in[i].bits;
            }
            return d;
        };
        in_rdy.assign().reads(arb_lo.in_rdy, arb_hi.in_rdy) = [](auto src) {
            auto [arb_lo_in_rdy, arb_hi_in_rdy] = src;
            RdyArr rdy{};
            for (uint32_t i = 0; i < N; ++i) rdy[i] = arb_lo_in_rdy[i] || arb_hi_in_rdy[i];
            return rdy;
        };
        w_has_high.assign().reads(arb_hi.out) = [](auto src) {
            auto [arb_hi_out] = src;
            return arb_hi_out.valid;
        };
        arb_hi.out_rdy.assign().reads(w_has_high, out_rdy) = [](auto src) {
            auto [w_has_high, out_rdy] = src;
            return w_has_high && out_rdy;
        };
        arb_lo.out_rdy.assign().reads(w_has_high, out_rdy) = [](auto src) {
            auto [w_has_high, out_rdy] = src;
            return !w_has_high && out_rdy;
        };
        out.assign().reads(w_has_high, arb_hi.out, arb_lo.out) = [](auto src) {
            auto [w_has_high, arb_hi_out, arb_lo_out] = src;
            return w_has_high ? arb_hi_out : arb_lo_out;
        };
        chosen.assign().reads(w_has_high, arb_hi.chosen, arb_lo.chosen) = [](auto src) {
            auto [w_has_high, arb_hi_chosen, arb_lo_chosen] = src;
            return w_has_high ? arb_hi_chosen : arb_lo_chosen;
        };
    }
};

// FastArb.scala 的 fastQosRRArb（rr=true → VipArbiter 子仲裁器）
template <class T, uint32_t N, class Qos = QosOf<T>>
using QosRRArb = QosArb<T, N, Qos, VipArb>;

// FastArb.scala 的 fastQosArb（rr=false → chisel3 Arbiter 子仲裁器）
template <class T, uint32_t N, class Qos = QosOf<T>>
using QosFixedArb = QosArb<T, N, Qos, FixedArb>;

// ---------------- Alloc：dongjiang Alloc（首个空闲项优先编码，组合） ----------------
// free_id = 最低 out_rdy 为高的索引；全忙时归 N-1（chisel PriorityMux 的
// 默认分支是末位——对拍实证：PriorityEncoder(全 0) = N-1，此时 out[N-1].valid
// 随 in.valid 拉高但不会 fire）；out[i].valid = in.valid && (free_id == i)；
// in_rdy = 存在空闲。

template <class T, uint32_t N>
class Alloc : public wolvicmod::Module {
public:
    static_assert(N >= 1);
    using DecT = Dec<T>;
    using OutArr = std::array<DecT, N>;  // 宏参数含逗号，先取别名
    using RdyArr = std::array<bool, N>;

    IN(bool, clk);  // 纯组合元件，clk 仅为接口统一保留
    IN(DecT, in);
    OUT(bool, in_rdy);
    OUT(uint32_t, free_id);
    OUT(OutArr, out);
    IN(RdyArr, out_rdy);

    Alloc() {
        free_id.assign().reads(out_rdy) = [](auto src) -> uint32_t {
            auto [out_rdy] = src;
            for (uint32_t i = 0; i < N; ++i)
                if (out_rdy[i]) return i;
            return N - 1;  // chisel PriorityEncoder 空输入归末位（PriorityMux 默认分支）
        };
        in_rdy.assign().reads(out_rdy) = [](auto src) {
            auto [out_rdy] = src;
            for (uint32_t i = 0; i < N; ++i)
                if (out_rdy[i]) return true;
            return false;
        };
        out.assign().reads(in, free_id) = [](auto src) {
            auto [in, free_id] = src;
            OutArr o{};
            for (uint32_t i = 0; i < N; ++i) {
                o[i].valid = in.valid && free_id == i;
                o[i].bits = in.bits;
            }
            return o;
        };
    }
};

}  // namespace zj::prefab
