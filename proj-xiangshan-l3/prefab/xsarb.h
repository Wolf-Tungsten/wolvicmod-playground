#pragma once

// XiangShan 生态仲裁器：VipArb（xs-utils VipArbiter）/ QosRRArb 与
// QosFixedArb（dongjiang FastArb 的 ArbiterGenerator）/ Alloc（dongjiang
// Alloc）。chisel3 标准库的 FixedArb/RRArb 在 wolvicmod 侧（prefab/arb.h），
// 本文件的 QosFixedArb 直接复用之。
//
// 统一端口形态（阵列端口）：输入侧一路 In<std::array<Valid<T>,N>> in + 一路
// Out<std::array<bool,N>> in_rdy；输出侧 Out<Valid<T>> out + In<bool> out_rdy；
// 另有 Out<uint32_t> chosen（当前授权索引）。clk 端口仅为接口统一；纯组合
// 元件（Alloc）不采样它。Alloc 方向相反的部分对应为：Out<std::array<Valid<T>,N>>
// out（N 路分发）+ In<std::array<bool,N>> out_rdy。

#include <array>
#include <cstdint>

#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/arb.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::prefab {

using wolvicmod::prefab::Valid;
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
    using ValidT = Valid<T>;
    using InArr = std::array<ValidT, N>;  // 宏参数含逗号，先取别名
    using RdyArr = std::array<bool, N>;

    IN(bool, clk);
    IN(InArr, in);
    OUT(RdyArr, in_rdy);
    OUT(ValidT, out);
    IN(bool, out_rdy);
    OUT(uint32_t, chosen);

    REG(uint32_t, vip);
    WIRE(bool, w_move);
    // 各路 valid 的密集拷贝（perf-breakdown §25 SoA）：chosen/in_rdy/w_move
    // 只依赖 valid 位——改读 w_v 后，in 中任一路 bits 翻动不再唤醒它们，
    // 且扫描从跨步读 Valid<T> 变成连续 bool。in 的读者只剩 w_v 自身与
    // out（bits 本来就要随数据翻动）。
    WIRE(RdyArr, w_v);

    VipArb() {
        w_v.assign().reads(in) = [](auto src) {
            auto [in] = src;
            RdyArr v{};
            for (uint32_t i = 0; i < N; ++i) v[i] = in[i].valid;
            return v;
        };
        chosen.assign().reads(vip, w_v) = [](auto src) -> uint32_t {
            auto [vip, w_v] = src;
            if (w_v[vip]) return vip;
            for (uint32_t i = 0; i < N; ++i)
                if (w_v[i]) return i;
            return 0;  // OHToUInt(0) = 0
        };
        out.assign().reads(in, chosen) = [](auto src) {
            auto [in, chosen] = src;
            ValidT o;
            // chosen 指向首个 valid 路（无 valid 时归 0），o.valid 等价于
            // 全路 valid 归约；无授权时输出零值（chisel Mux1H(selPtrOH)）
            o.valid = in[chosen].valid;
            o.bits = o.valid ? in[chosen].bits : T{};
            return o;
        };
        in_rdy.assign().reads(w_v, chosen, out_rdy) = [](auto src) {
            auto [w_v, chosen, out_rdy] = src;
            RdyArr rdy{};
            if (out_rdy && w_v[chosen]) rdy[chosen] = true;
            return rdy;
        };
        // 指针转移门（perf-breakdown §22：原 w_vip_req/w_other_v/w_out_fire
        // 三条单消费中转内联）
        w_move.assign().reads(vip, w_v, out, out_rdy) = [](auto src) {
            auto [vip, w_v, out, out_rdy] = src;
            bool other_v = false;
            for (uint32_t i = 0; i < N; ++i)
                if (i != vip && w_v[i]) {
                    other_v = true;
                    break;
                }
            if (!other_v) return false;
            return w_v[vip] ? (out.valid && out_rdy) : true;
        };
        // 原 w_next_vip 中转内联：vip 之上最低 valid（highValidMask 优先），
        // 无则绕回 vip 之下的最低 valid（lowValidMask）
        vip.update().on(posedge(clk)).en(w_move).reads(vip, w_v) = [](auto src) {
            auto [vip, w_v] = src;
            for (uint32_t i = vip + 1; i < N; ++i)
                if (w_v[i]) return i;
            for (uint32_t i = 0; i < vip; ++i)
                if (w_v[i]) return i;
            return vip;
        };
    }
};

// ---------------- QosArb：dongjiang FastArb（ArbiterGenerator） ----------------
// 按 qos == 0xf 拆 high/low 两组：low 组 = 全部输入，high 组 = qos==0xf 的输入
// （valid 打掩、bits 直通）；两组各过一个子仲裁器。hasHigh 时 high 组获胜、
// low 组 out_rdy 拉低；否则反之。in_rdy[i] = low.in_rdy[i] || high.in_rdy[i]。
// 注意：FastArb.scala 的 rr=true 子仲裁器是 xs-utils 的 VipArbiter（不是
// chisel3 RRArbiter）——QosRRArb 因此以 VipArb 为默认子仲裁器；QosFixedArb
// 用 FixedArb（chisel3 Arbiter）。T 须带 .qos 字段（与 EjectBuffer 要求
// .src_id/.txn_id 同款约定）；子仲裁器类型 Sub 对应 FastArb 的 rr 参数。
//
// 同名解包约定的跨模块写法：读集里的子模块端口按层次路径展开绑定名
// （arb_hi.out → arb_hi_out，点替换为下划线）。

template <class T, uint32_t N, class Sub = VipArb<T, N>>
class QosArb : public wolvicmod::Module {
public:
    static_assert(N >= 1);
    using ValidT = Valid<T>;
    using InArr = std::array<ValidT, N>;  // 宏参数含逗号，先取别名
    using RdyArr = std::array<bool, N>;

    IN(bool, clk);
    IN(InArr, in);
    OUT(RdyArr, in_rdy);
    OUT(ValidT, out);
    IN(bool, out_rdy);
    OUT(uint32_t, chosen);

    MOD(Sub, arb_lo);
    MOD(Sub, arb_hi);

    QosArb() {
        arb_lo.clk = clk;
        arb_hi.clk = clk;
        arb_lo.in = in;  // low 组 = 全部输入
        arb_hi.in.assign().reads(in) = [](auto src) {
            auto [in] = src;
            InArr d{};
            for (uint32_t i = 0; i < N; ++i) {
                d[i].valid = in[i].valid && in[i].bits.qos == 0xf;
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
        // hasHigh 直接取 arb_hi.out.valid（§22：原中转线 w_has_high 内联消除）
        arb_hi.out_rdy.assign().reads(arb_hi.out, out_rdy) = [](auto src) {
            auto [arb_hi_out, out_rdy] = src;
            return arb_hi_out.valid && out_rdy;
        };
        arb_lo.out_rdy.assign().reads(arb_hi.out, out_rdy) = [](auto src) {
            auto [arb_hi_out, out_rdy] = src;
            return !arb_hi_out.valid && out_rdy;
        };
        out.assign().reads(arb_hi.out, arb_lo.out) = [](auto src) {
            auto [arb_hi_out, arb_lo_out] = src;
            return arb_hi_out.valid ? arb_hi_out : arb_lo_out;
        };
        chosen.assign().reads(arb_hi.out, arb_hi.chosen, arb_lo.chosen) = [](auto src) {
            auto [arb_hi_out, arb_hi_chosen, arb_lo_chosen] = src;
            return arb_hi_out.valid ? arb_hi_chosen : arb_lo_chosen;
        };
    }
};

// FastArb.scala 的 fastQosRRArb（rr=true → VipArbiter 子仲裁器）
template <class T, uint32_t N>
using QosRRArb = QosArb<T, N, VipArb<T, N>>;

// FastArb.scala 的 fastQosArb（rr=false → chisel3 Arbiter 子仲裁器）
template <class T, uint32_t N>
using QosFixedArb = QosArb<T, N, FixedArb<T, N>>;

// ---------------- Alloc：dongjiang Alloc（首个空闲项优先编码，组合） ----------------
// free_id = 最低 out_rdy 为高的索引；全忙时归 N-1（chisel PriorityMux 的
// 默认分支是末位——对拍实证：PriorityEncoder(全 0) = N-1，此时 out[N-1].valid
// 随 in.valid 拉高但不会 fire）；out[i].valid = in.valid && (free_id == i)；
// in_rdy = 存在空闲。

template <class T, uint32_t N>
class Alloc : public wolvicmod::Module {
public:
    static_assert(N >= 1);
    using ValidT = Valid<T>;
    using OutArr = std::array<ValidT, N>;  // 宏参数含逗号，先取别名
    using RdyArr = std::array<bool, N>;

    IN(bool, clk);  // 纯组合元件，clk 仅为接口统一保留
    IN(ValidT, in);
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

// ---------------- CondVipArb：xs-utils ConditionVipArbiter ----------------
// （arb/ConditionArbiter.scala + SelNto1）结构 = SelNto1 → selReg → VipArbiter：
//   sel_oh(i) = mvalid(i) 且 mvalid 中不存在 qos 更高者（全胜，可多热）；
//   mvalid(i) = in(i).valid && !本拍该路 fire（fire 当拍即从选择中剔除）
//   selReg ← sel_oh（en = 任一路原始 valid；无 valid 时保持）
//   内层 VipArb 的输入 valid = selReg(i)、bits 直通；in_rdy = 内层 in_rdy
//   （= out_rdy && selReg(i) && chosen==i，即 io.in(i).fire）。
// T 须带 .qos 字段（同 QosArb/EjectBuffer 的字段约定）。

template <class T, uint32_t N>
class CondVipArb : public wolvicmod::Module {
public:
    static_assert(N >= 1);
    using ValidT = Valid<T>;
    using InArr = std::array<ValidT, N>;  // 宏参数含逗号，先取别名
    using RdyArr = std::array<bool, N>;

    IN(bool, clk);
    IN(InArr, in);
    OUT(RdyArr, in_rdy);
    OUT(ValidT, out);
    IN(bool, out_rdy);
    OUT(uint32_t, chosen);

    REG(RdyArr, sel_reg);
    using Arb = VipArb<T, N>;  // 宏参数含逗号，先取别名
    MOD(Arb, arb);

    WIRE(RdyArr, w_sel_oh);
    WIRE(bool, w_any_vld);

    CondVipArb() {
        arb.clk = clk;
        // mvalid：剔除本拍 fire 的路（in_rdy 即 fire）
        w_sel_oh.assign().reads(in, sel_reg, chosen, out_rdy) = [](auto src) {
            auto [in, sel_reg, chosen, out_rdy] = src;
            bool mv[N];
            for (uint32_t i = 0; i < N; ++i)
                mv[i] = in[i].valid && !(out_rdy && sel_reg[i] && chosen == i);
            RdyArr oh{};
            for (uint32_t i = 0; i < N; ++i) {
                bool win = mv[i];
                for (uint32_t j = 0; j < N; ++j)
                    if (j != i && mv[j]) win = win && (in[i].bits.qos >= in[j].bits.qos);
                oh[i] = win;
            }
            return oh;
        };
        w_any_vld.assign().reads(in) = [](auto src) {
            auto [in] = src;
            for (uint32_t i = 0; i < N; ++i)
                if (in[i].valid) return true;
            return false;
        };
        sel_reg.update().on(posedge(clk)).en(w_any_vld).reads(w_sel_oh) = [](auto src) {
            auto [w_sel_oh] = src;
            return w_sel_oh;
        };
        arb.in.assign().reads(in, sel_reg) = [](auto src) {
            auto [in, sel_reg] = src;
            InArr d{};
            for (uint32_t i = 0; i < N; ++i) {
                d[i].valid = sel_reg[i];
                d[i].bits  = in[i].bits;
            }
            return d;
        };
        arb.out_rdy = out_rdy;
        out = arb.out;
        in_rdy = arb.in_rdy;
        chosen = arb.chosen;
    }
};

}  // namespace zj::prefab
