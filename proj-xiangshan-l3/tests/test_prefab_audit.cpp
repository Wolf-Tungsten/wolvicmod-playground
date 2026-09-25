// 项目侧 prefab 读集对账（§6.4）：auditOn 下驱动各元件若干拍，任何 lambda
// 经框架读路径访问未声明实体都会立刻报错；同时打开 assertUpdateMutexOn
// （预制菜每个状态只挂一条 Update，不应出现同 round 多激活）。

#include <array>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <prefab/prefab.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace zj::prefab;
using namespace prefabtest;

namespace {

struct QosBits {
    uint32_t qos = 0;
    uint32_t payload = 0;

    bool operator==(const QosBits&) const = default;
};

TEST_CASE("zj prefab: auditOn + assertUpdateMutexOn 全元件巡查") {
    {
        FastQueue<uint32_t, 3> top;
        top.elaborate();
        top.auditOn();
        top.assertUpdateMutexOn();
        for (uint32_t c = 0; c < 24; ++c) {
            top.enq.set({(c & 3u) != 0, c});
            top.deq_rdy.set((c & 1u) != 0);
            cycle(top);
        }
    }
    {
        VipArb<uint32_t, 4> top;
        top.elaborate();
        top.auditOn();
        top.assertUpdateMutexOn();
        std::array<Dec<uint32_t>, 4> ins{};
        for (uint32_t c = 0; c < 24; ++c) {
            for (uint32_t i = 0; i < 4; ++i) ins[i] = {((c >> i) & 1u) != 0, c * 10 + i};
            top.in.set(ins);
            top.out_rdy.set((c & 1u) != 0);
            cycle(top);
        }
    }
    {
        QosRRArb<QosBits, 4> top;
        top.elaborate();
        top.auditOn();
        top.assertUpdateMutexOn();
        std::array<Dec<QosBits>, 4> ins{};
        for (uint32_t c = 0; c < 24; ++c) {
            for (uint32_t i = 0; i < 4; ++i)
                ins[i] = {((c >> i) & 1u) != 0, {(i == c % 4) ? 0xfu : i, c * 10 + i}};
            top.in.set(ins);
            top.out_rdy.set((c & 1u) != 0);
            cycle(top);
        }
    }
    {
        Alloc<uint32_t, 3> top;
        top.elaborate();
        top.auditOn();
        top.assertUpdateMutexOn();
        std::array<bool, 3> rdy{};
        for (uint32_t c = 0; c < 24; ++c) {
            top.in.set({(c & 1u) != 0, c});
            for (uint32_t i = 0; i < 3; ++i) rdy[i] = ((c >> i) & 1u) != 0;
            top.out_rdy.set(rdy);
            cycle(top);
        }
    }
    {
        SpSram<uint32_t, 16, 2, 2, 2, false, true, true> top;
        top.elaborate();
        top.auditOn();
        top.assertUpdateMutexOn();
        for (uint32_t c = 0; c < 24; ++c) {
            const bool wr = (c & 1u) != 0;
            top.req.set({(c & 3u) != 0, {wr, c % 16, 0b11, {c, c + 1}}});
            cycle(top);
        }
    }
    {
        DpSram<uint32_t, 16, 1, true, 1, 1, false, true, true> top;
        top.elaborate();
        top.auditOn();
        top.assertUpdateMutexOn();
        for (uint32_t c = 0; c < 24; ++c) {
            top.wreq.set({(c & 1u) != 0, {true, c % 16, 0, {c}}});
            top.rreq.set({(c & 3u) != 0, c % 16});
            cycle(top);
        }
    }
}

}  // namespace
