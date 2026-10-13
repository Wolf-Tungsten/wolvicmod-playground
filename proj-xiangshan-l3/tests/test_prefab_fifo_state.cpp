// FifoState 值类型对 wolvicmod::prefab::Queue 模块的随机对拍：
// 同一随机激励（enq.valid/bits、deq_rdy）逐拍驱动两侧，comb 相位比对
// enq_rdy / deq.valid / deq.bits / count，edge 相位 FifoState 走纯函数 next。
// 覆盖 N=1/2/3/4 与 Flow/Pipe 组合，外加定向灌满/排空/空闲段。

#include <cstdint>
#include <random>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/queue.h>
#include <prefab/fifo_state.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace prefabtest;

namespace {

template <uint32_t N, bool Flow, bool Pipe>
void cosimRun(uint64_t seed) {
    using Q = prefab::Queue<uint32_t, N, Flow, Pipe>;
    Q q;
    q.elaborate();
    zj::prefab::FifoState<uint32_t, N, Flow, Pipe> fs{};

    std::mt19937                    rng(seed);
    std::uniform_int_distribution<> coin(0, 1);

    auto step = [&](bool enq_v, uint32_t bits, bool deq_r) {
        prefab::Valid<uint32_t> enq{enq_v, bits};
        q.enq.set(enq);
        q.deq_rdy.set(deq_r);
        comb(q);
        // 组合输出三比对
        CHECK(q.enq_rdy.get() == zj::prefab::fifoEnqRdy(fs, deq_r));
        const auto fd = zj::prefab::fifoDeq(fs, enq);
        CHECK(q.deq.get().valid == fd.valid);
        if (fd.valid) CHECK(q.deq.get().bits == fd.bits);
        CHECK(q.count.get() == zj::prefab::fifoCount(fs));
        edge(q);
        fs = zj::prefab::fifoNext(fs, bits, enq_v, deq_r);
    };

    // 定向：空闲 5 拍
    for (int t = 0; t < 5; ++t) step(false, 0, coin(rng));
    // 定向：灌满（enq 恒 valid,deq 恒 backpressure 到满）
    for (uint32_t t = 0; t < N + 2; ++t) step(true, 0xA0 + t, false);
    // 定向：满时 pipe 放行角落（deq_rdy 拉高）
    for (uint32_t t = 0; t < 3; ++t) step(true, 0xB0 + t, true);
    // 定向：排空
    for (uint32_t t = 0; t < N + 2; ++t) step(false, 0, true);
    // 随机 2000 拍
    uint32_t bits = 0;
    for (int t = 0; t < 2000; ++t) {
        bits += 0x11;
        step(coin(rng), bits, coin(rng));
    }
    // 定向：burst 反压锯齿（两拍进一拍出的密度扫掠）
    for (int t = 0; t < 600; ++t) {
        bits += 0x7;
        step(t % 3 != 2, bits, t % 2 == 0);
    }
}

}  // namespace

TEST_CASE("FifoState 对拍 Queue：N=1 非 pipe") { cosimRun<1, false, false>(1); }
TEST_CASE("FifoState 对拍 Queue：N=1 pipe(EjectBuffer ipipe 形态)") { cosimRun<1, false, true>(2); }
TEST_CASE("FifoState 对拍 Queue：N=2(InjQueue 形态)") { cosimRun<2, false, false>(3); }
TEST_CASE("FifoState 对拍 Queue：N=2 pipe") { cosimRun<2, false, true>(4); }
TEST_CASE("FifoState 对拍 Queue：N=3") { cosimRun<3, false, false>(5); }
TEST_CASE("FifoState 对拍 Queue：N=4") { cosimRun<4, false, false>(6); }
TEST_CASE("FifoState 对拍 Queue：N=2 flow") { cosimRun<2, true, false>(7); }
TEST_CASE("FifoState 对拍 Queue：N=2 flow+pipe") { cosimRun<2, true, true>(8); }
