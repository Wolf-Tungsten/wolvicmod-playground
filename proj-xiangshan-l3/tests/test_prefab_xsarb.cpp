// 项目侧 XiangShan 生态仲裁器单测：VipArb（xs-utils VipArbiter）/
// QosRRArb / QosFixedArb（dongjiang FastArb）/ Alloc（dongjiang Alloc）。
// 覆盖：VipArb 的 vip 粘性与让位；QoS 高优先抢占与高低组轮转；Alloc 首个空闲。
// 阵列端口：整路 in.set(arr)，断言 in_rdy.get()[i] / out.get()[i]。

#include <array>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <prefab/prefab.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace zj::prefab;
using namespace prefabtest;

namespace {

using InArr4 = std::array<Dec<uint32_t>, 4>;

TEST_CASE("zj VipArb: 连续 fire 时指针正向轮转（跳过无效路）") {
    VipArb<uint32_t, 4> top;
    top.elaborate();
    InArr4 ins{};
    for (uint32_t i = 0; i < 4; ++i) ins[i] = {true, 400 + i};
    top.in.set(ins);
    top.out_rdy.set(true);

    // vip 初值 0：vip 获胜并让位到"之上最低 valid"——全 valid 连续 fire 时
    // 正向轮转 0,1,2,3,0,1…（VipArbiter.scala：highValidMask 优先、绕回 low）
    for (uint32_t expect : {0u, 1u, 2u, 3u, 0u, 1u, 2u, 3u}) {
        comb(top);
        CHECK(top.chosen.get() == expect);
        CHECK(top.out.get().valid == true);
        CHECK(top.out.get().bits == 400 + expect);
        CHECK(top.in_rdy.get()[expect] == true);
        edge(top);
    }

    // 只留 in0/in2：轮转跳过无效的 in1/in3 → 0,2,0,2…
    ins[1].valid = false;
    ins[3].valid = false;
    top.in.set(ins);
    for (uint32_t expect : {0u, 2u, 0u, 2u}) {
        comb(top);
        CHECK(top.chosen.get() == expect);
        CHECK(top.out.get().bits == 400 + expect);
        edge(top);
    }
}

TEST_CASE("zj VipArb: vip 胜过更低索引（与固定优先级的区别点）") {
    VipArb<uint32_t, 4> top;
    top.elaborate();
    InArr4 ins{};
    for (uint32_t i = 0; i < 4; ++i) ins[i] = {true, 500 + i};
    top.in.set(ins);
    top.out_rdy.set(true);
    // 先把 vip 推进到 1（chosen=0 fire 一拍）
    comb(top);
    CHECK(top.chosen.get() == 0);
    edge(top);
    // 只留 in[0]/in[1]：vip=1 时 in[0] 索引更低但 vip 获胜
    ins[2].valid = false;
    ins[3].valid = false;
    top.in.set(ins);
    comb(top);
    CHECK(top.chosen.get() == 1);   // ★ FixedArb 此处会选 0
    CHECK(top.out.get().bits == 501);
    edge(top);
    // fire 后 vip 让位到 0
    comb(top);
    CHECK(top.chosen.get() == 0);
    CHECK(top.out.get().bits == 500);
    edge(top);
}

TEST_CASE("zj VipArb: vip 不请求时指针无条件移向最低 valid") {
    VipArb<uint32_t, 4> top;
    top.elaborate();
    InArr4 ins{};   // 全无效
    top.in.set(ins);
    top.out_rdy.set(false);   // 不 fire
    ins[2] = {true, 52};
    ins[3] = {true, 53};
    top.in.set(ins);

    // vip=0 未请求：组合授权给最低 valid（2），且即使不 fire 指针也移动
    comb(top);
    CHECK(top.chosen.get() == 2);
    CHECK(top.out.get().valid == true);
    CHECK(top.in_rdy.get()[2] == false);  // out_rdy=0
    edge(top);
    // 下一拍 vip=2：vip 自己就是最低 valid，chosen 仍为 2
    comb(top);
    CHECK(top.chosen.get() == 2);
    edge(top);
    // in[2] 撤请求、in[3] 留下：vip=2 未请求 → 组合给 3，指针移向 3
    ins[2].valid = false;
    top.in.set(ins);
    comb(top);
    CHECK(top.chosen.get() == 3);
    edge(top);
    // vip=3：反压下 chosen 保持 3（vip 请求但不 fire → 指针不动）
    for (int c = 0; c < 2; ++c) {
        comb(top);
        CHECK(top.chosen.get() == 3);
        edge(top);
    }
}

struct QosBits {
    uint32_t qos = 0;
    uint32_t payload = 0;

    bool operator==(const QosBits&) const = default;
};

using QosArr4 = std::array<Dec<QosBits>, 4>;

TEST_CASE("zj QosFixedArb: qos==0xf 高优先抢占") {
    QosFixedArb<QosBits, 4> top;
    top.elaborate();
    QosArr4 ins{};
    top.in.set(ins);
    top.out_rdy.set(true);

    // in[0] 低 qos、in[1] 高 qos、in[2] 低 qos：hasHigh → 高组获胜
    ins[0] = {true, {1, 1000}};
    ins[1] = {true, {0xf, 1001}};
    ins[2] = {true, {2, 1002}};
    top.in.set(ins);
    comb(top);
    CHECK(top.out.get().valid == true);
    CHECK(top.chosen.get() == 1);   // 高组固定优先级：in[1]
    CHECK(top.out.get().bits.payload == 1001);
    // chisel Arbiter 的 ready 不门控自身 valid：in[0] 低 qos、未被高组授予，
    // 但其高组输入无效且前面无高组 valid → ready=1（不会因此 fire——它不在
    // 高组里；这正是 FastArb 源码 PopCount(fire)===out.fire 断言盯的角落）
    CHECK(top.in_rdy.get()[0] == true);
    CHECK(top.in_rdy.get()[1] == true);
    CHECK(top.in_rdy.get()[2] == false);
    edge(top);

    // 两个高 qos：高组内固定优先级取低索引
    ins[3] = {true, {0xf, 1003}};
    top.in.set(ins);
    comb(top);
    CHECK(top.chosen.get() == 1);
    edge(top);
    ins[1].valid = false;
    top.in.set(ins);
    comb(top);
    CHECK(top.chosen.get() == 3);
    CHECK(top.out.get().bits.payload == 1003);
    edge(top);

    // 没有高 qos：低组（= 全部输入）按固定优先级
    ins[3].valid = false;
    top.in.set(ins);
    comb(top);
    CHECK(top.chosen.get() == 0);   // in[0]/in[2] 低 qos 竞争，in[0] 赢
    CHECK(top.out.get().bits.payload == 1000);
    CHECK(top.in_rdy.get()[0] == true);
    edge(top);
    ins[0].valid = false;
    top.in.set(ins);
    comb(top);
    CHECK(top.chosen.get() == 2);
    edge(top);
}

TEST_CASE("zj QosRRArb: 高组内按 VipArb 轮转（FastArb 的 RR=VipArbiter）") {
    QosRRArb<QosBits, 4> top;
    top.elaborate();
    QosArr4 ins{};
    top.in.set(ins);
    top.out_rdy.set(true);
    // 高组 {1, 3}，持续请求 + 连续 fire
    ins[1] = {true, {0xf, 11}};
    ins[3] = {true, {0xf, 33}};
    top.in.set(ins);

    // 高组子仲裁器是 VipArbiter（vip 初值 0）：
    // 拍0 vip=0 未请求 → 授权最低 valid=1，指针移向 1；
    // 拍1 vip=1 获胜 fire → 让位给 3；拍2 vip=3 fire → 让位给 1；之后 1,3 轮转
    for (uint32_t expect : {1u, 1u, 3u, 1u, 3u, 1u, 3u}) {
        comb(top);
        CHECK(top.chosen.get() == expect);
        CHECK(top.out.get().valid == true);
        CHECK(top.out.get().bits.payload == expect * 11);
        CHECK(top.in_rdy.get()[expect] == true);
        edge(top);
    }
    // 高组撤出后低组（全部输入）按 VipArb 授权。
    // 注意：高组阶段低组子仲裁器一直看到 in[1]/in[3] 的 valid（low 组 = 全部
    // 输入），其 vip 已移至 1 并驻留——撤出后首个有效授权从 vip=1 的视角给出。
    ins[1].valid = false;
    ins[3].valid = false;
    ins[0] = {true, {0, 70}};
    ins[2] = {true, {0, 72}};
    top.in.set(ins);
    // 拍0：vip=1 未请求 → 授权最低 valid=0，指针移向"之上最低 valid"=2；
    // 拍1：vip=2 获胜 fire → 绕回让位给 0；之后 0,2 轮转
    for (uint32_t expect : {0u, 2u, 0u, 2u}) {
        comb(top);
        CHECK(top.chosen.get() == expect);
        CHECK(top.out.get().bits.payload == 70 + expect);
        edge(top);
    }
}

TEST_CASE("zj Alloc: 首个空闲项优先编码") {
    Alloc<uint32_t, 3> top;
    top.elaborate();
    top.in.set({false, 0});
    std::array<bool, 3> rdy{false, false, false};
    top.out_rdy.set(rdy);

    // 全忙：in_rdy=0。注意 free_id 此时归末位 N-1=2（chisel PriorityEncoder
    // 空输入归末位，PriorityMux 默认分支），out[N-1].valid 随 in.valid 拉高
    // ——但 in_rdy=0 不会 fire，与 Alloc.scala（o.valid = in.valid & freeId===i）
    // 行为一致
    top.in.set({true, 42});
    comb(top);
    CHECK(top.in_rdy.get() == false);
    CHECK(top.free_id.get() == 2);
    CHECK(top.out.get()[2].valid == true);   // 末位拉高但不会 fire
    CHECK(top.out.get()[0].valid == false);
    edge(top);

    // {0,1,1} 空闲：free_id=1，in 路由到 out[1]
    rdy[1] = true;
    rdy[2] = true;
    top.out_rdy.set(rdy);
    comb(top);
    CHECK(top.free_id.get() == 1);
    CHECK(top.in_rdy.get() == true);
    CHECK(top.out.get()[0].valid == false);
    CHECK(top.out.get()[1].valid == true);
    CHECK(top.out.get()[1].bits == 42);
    CHECK(top.out.get()[2].valid == false);
    edge(top);

    // 全空闲：free_id=0
    rdy[0] = true;
    top.out_rdy.set(rdy);
    comb(top);
    CHECK(top.free_id.get() == 0);
    CHECK(top.out.get()[0].valid == true);
    CHECK(top.out.get()[0].bits == 42);
    edge(top);

    // in 无效：无 out 有效
    top.in.set({false, 0});
    comb(top);
    for (uint32_t i = 0; i < 3; ++i) CHECK(top.out.get()[i].valid == false);
    CHECK(top.in_rdy.get() == true);  // ready 只看空闲
    edge(top);
}

}  // namespace
