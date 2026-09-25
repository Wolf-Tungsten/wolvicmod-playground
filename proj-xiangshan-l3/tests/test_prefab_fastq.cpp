// 项目侧 FastQueue<T, N> 单测：xs-utils FastQueue 拍级对齐。
// 覆盖：灌满→排空占位数扫描；同拍 enq+deq；**满 → deq 后下一拍 enq_rdy 才
// 恢复**的寄存 ready 气泡（与 chisel pipe Queue 的本质差异点）；长程随机
// 反压保序。

#include <deque>
#include <random>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <prefab/prefab.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace zj::prefab;
using namespace prefabtest;

namespace {

using FQ3 = FastQueue<uint32_t, 3>;

TEST_CASE("zj FastQueue: 灌满→排空，count/free_num 逐拍扫描") {
    FQ3 top;
    top.elaborate();
    top.enq.set({false, 0});
    top.deq_rdy.set(false);

    // cycle 0：空，enq_rdy 初值 true（RegInit(true.B)）
    comb(top);
    CHECK(top.count.get() == 0);
    CHECK(top.free_num.get() == 3);
    CHECK(top.enq_rdy.get() == true);
    CHECK(top.deq.get().valid == false);
    edge(top);

    // 灌 10：cycle 1
    top.enq.set({true, 10});
    comb(top);
    CHECK(top.count.get() == 0);
    CHECK(top.enq_rdy.get() == true);
    edge(top);
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.count.get() == 1);
    CHECK(top.free_num.get() == 2);
    CHECK(top.deq.get().valid == true);
    CHECK(top.deq.get().bits == 10);
    edge(top);

    // 灌 11：cycle 2（当前 count=1 < N-1 → 下一拍 ready 保持）
    top.enq.set({true, 11});
    comb(top);
    CHECK(top.enq_rdy.get() == true);
    edge(top);
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.count.get() == 2);
    CHECK(top.deq.get().bits == 10);
    edge(top);

    // 灌 12：当前 count=2 = N-1 → 仅 enq 后 ready 下一拍拉低
    top.enq.set({true, 12});
    comb(top);
    CHECK(top.enq_rdy.get() == true);   // 本拍仍可灌（灌入后即满）
    edge(top);
    // cycle 4：满
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.count.get() == 3);
    CHECK(top.free_num.get() == 0);
    CHECK(top.enq_rdy.get() == false);
    CHECK(top.deq.get().valid == true);
    CHECK(top.deq.get().bits == 10);
    edge(top);

    // 排空 10, 11, 12
    top.enq.set({false, 0});
    top.deq_rdy.set(true);
    for (uint32_t expect : {10u, 11u, 12u}) {
        comb(top);
        CHECK(top.deq.get().valid == true);
        CHECK(top.deq.get().bits == expect);
        edge(top);
    }
    comb(top);
    CHECK(top.count.get() == 0);
    CHECK(top.free_num.get() == 3);
    CHECK(top.deq.get().valid == false);
    CHECK(top.enq_rdy.get() == true);
    edge(top);
}

TEST_CASE("zj FastQueue: 满→deq 后下一拍 enq_rdy 才恢复（气泡断言）") {
    FQ3 top;
    top.elaborate();
    top.enq.set({false, 0});
    top.deq_rdy.set(false);
    comb(top);
    edge(top);

    // 灌满 [10, 11, 12]
    for (uint32_t v : {10u, 11u, 12u}) {
        top.enq.set({true, v});
        comb(top);
        edge(top);
    }
    // cycle 3：满。同拍 deq_rdy=1 + enq.valid=1——与 chisel pipe Queue 的本质
    // 差异：enq_rdy 是寄存的，本拍仍然为低，enq 进不来（一拍气泡）
    top.enq.set({true, 13});
    top.deq_rdy.set(true);
    comb(top);
    CHECK(top.enq_rdy.get() == false);  // ★ pipe Queue 此处会为 true
    CHECK(top.deq.get().valid == true);
    CHECK(top.deq.get().bits == 10);
    edge(top);
    // cycle 4：10 已弹出、13 未入队（count 3→2），ready 本拍恢复；
    // enq(13) 在 ready 恢复的同拍即可灌入（deq 同拍弹 11）→ [12, 13]
    comb(top);
    CHECK(top.count.get() == 2);
    CHECK(top.enq_rdy.get() == true);   // ★ 下一拍才恢复
    CHECK(top.deq.get().bits == 11);
    edge(top);
    // cycle 5：13 落在队尾 [12, 13]
    top.enq.set({false, 0});
    top.deq_rdy.set(false);
    comb(top);
    CHECK(top.count.get() == 2);
    CHECK(top.deq.get().bits == 12);
    edge(top);
    top.deq_rdy.set(true);
    comb(top);
    CHECK(top.deq.get().bits == 12);
    edge(top);
    comb(top);
    CHECK(top.deq.get().valid == true);
    CHECK(top.deq.get().bits == 13);
    edge(top);
    comb(top);
    CHECK(top.deq.get().valid == false);
    CHECK(top.count.get() == 0);
    edge(top);
}

TEST_CASE("zj FastQueue: 同拍 enq+deq（count 不变、新项落尾）") {
    FQ3 top;
    top.elaborate();
    top.enq.set({false, 0});
    top.deq_rdy.set(false);
    comb(top);
    edge(top);

    // 灌 1 项后同拍推拉：count=1 不变，内容 [10] → [20]
    top.enq.set({true, 10});
    comb(top);
    edge(top);
    top.enq.set({true, 20});
    top.deq_rdy.set(true);
    comb(top);
    CHECK(top.count.get() == 1);
    CHECK(top.deq.get().valid == true);
    CHECK(top.deq.get().bits == 10);
    CHECK(top.enq_rdy.get() == true);
    edge(top);
    top.enq.set({false, 0});
    top.deq_rdy.set(false);
    comb(top);
    CHECK(top.count.get() == 1);
    CHECK(top.deq.get().valid == true);
    CHECK(top.deq.get().bits == 20);   // 新项已落到 array[0]
    edge(top);

    // 灌到 2 项 [20, 21]，同拍推拉：count=2 不变，[20,21] → [21,22]
    top.enq.set({true, 21});
    comb(top);
    edge(top);
    top.enq.set({true, 22});
    top.deq_rdy.set(true);
    comb(top);
    CHECK(top.count.get() == 2);
    CHECK(top.deq.get().bits == 20);
    edge(top);
    top.enq.set({false, 0});
    top.deq_rdy.set(false);
    comb(top);
    CHECK(top.count.get() == 2);
    CHECK(top.deq.get().bits == 21);
    edge(top);
    // 排空校验 21, 22
    top.deq_rdy.set(true);
    for (uint32_t expect : {21u, 22u}) {
        comb(top);
        CHECK(top.deq.get().valid == true);
        CHECK(top.deq.get().bits == expect);
        edge(top);
    }
    comb(top);
    CHECK(top.deq.get().valid == false);
    edge(top);
}

TEST_CASE("zj FastQueue: 随机反压长程保序（寄存 ready 模型对拍 400 拍）") {
    FQ3 top;
    top.elaborate();
    std::mt19937 rng(12345);
    std::deque<uint32_t> model;
    bool rdyReg = true;   // 模型侧的 enqRdyReg
    uint32_t pushed = 0, popped = 0;
    top.enq.set({false, 0});
    top.deq_rdy.set(false);
    comb(top);
    edge(top);
    for (int c = 0; c < 400; ++c) {
        const bool ev = (rng() & 3u) != 0;
        const bool dr = (rng() & 1u) != 0;
        top.enq.set({ev, pushed});
        top.deq_rdy.set(dr);
        comb(top);
        // 组合输出对拍
        CHECK(top.count.get() == model.size());
        CHECK(top.free_num.get() == 3 - model.size());
        CHECK(top.enq_rdy.get() == rdyReg);
        CHECK(top.deq.get().valid == !model.empty());
        if (!model.empty()) CHECK(top.deq.get().bits == model.front());
        const bool enqF = ev && rdyReg;
        const bool deqF = !model.empty() && dr;
        edge(top);
        if (deqF) {
            CHECK(model.front() == popped);
            model.pop_front();
            ++popped;
        }
        if (enqF) model.push_back(pushed++);
        // enqRdyReg 次态（读当前 count，即更新前的 model.size()）
        const size_t curCount = model.size() + (deqF ? 1 : 0) - (enqF ? 1 : 0);
        if (enqF && !deqF) rdyReg = curCount < 3 - 1;
        else if (!enqF && deqF) rdyReg = true;
    }
    CHECK(popped > 50);
}

TEST_CASE("zj FastQueue<2>: ZhuJiang 常用深度的满/气泡行为") {
    FastQueue<uint32_t, 2> top;
    top.elaborate();
    top.enq.set({false, 0});
    top.deq_rdy.set(false);
    comb(top);
    CHECK(top.enq_rdy.get() == true);
    edge(top);

    // 灌满 [1, 2]：count=1 时仅 enq → ready 下一拍拉低
    top.enq.set({true, 1});
    comb(top);
    edge(top);
    top.enq.set({true, 2});
    comb(top);
    CHECK(top.count.get() == 1);
    CHECK(top.enq_rdy.get() == true);   // 灌第 2 项的本拍仍 ready
    edge(top);
    top.enq.set({true, 3});
    comb(top);
    CHECK(top.count.get() == 2);
    CHECK(top.enq_rdy.get() == false);  // 满：3 进不来
    edge(top);
    // 满 + deq_rdy：本拍 ready 仍低（气泡），下一拍恢复
    top.deq_rdy.set(true);
    comb(top);
    CHECK(top.enq_rdy.get() == false);
    CHECK(top.deq.get().bits == 1);
    edge(top);
    comb(top);
    CHECK(top.count.get() == 1);
    CHECK(top.enq_rdy.get() == true);   // 恢复，3 本拍可入
    CHECK(top.deq.get().bits == 2);
    edge(top);                          // 入 3、出 2 → [3]
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.count.get() == 1);
    CHECK(top.deq.get().bits == 3);
    edge(top);
    comb(top);
    CHECK(top.deq.get().valid == false);
    edge(top);
}

TEST_CASE("zj FastQueue: NoX 变体 deq 无效时 bits 为零") {
    FastQueue<uint32_t, 2, true> top;
    top.elaborate();
    top.enq.set({false, 0});
    top.deq_rdy.set(false);
    comb(top);
    CHECK(top.deq.get().valid == false);
    CHECK(top.deq.get().bits == 0);
    edge(top);
    top.enq.set({true, 55});
    comb(top);
    edge(top);
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.deq.get().valid == true);
    CHECK(top.deq.get().bits == 55);
    edge(top);
    top.deq_rdy.set(true);
    comb(top);
    edge(top);
    comb(top);
    CHECK(top.deq.get().valid == false);
    CHECK(top.deq.get().bits == 0);   // NoX：弹出后 bits 归零而非陈旧 55
    edge(top);
}

}  // namespace
