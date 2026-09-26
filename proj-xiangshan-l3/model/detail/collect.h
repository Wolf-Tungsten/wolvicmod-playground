#pragma once

// collectPorts：把 N 个同类型子模块端口汇聚成一路 Wire<std::array<V,N>>。
// 框架的 reads(...) 是可变参 API，子模块以指针数组（createChildModule 循环
// 建）持有时无法逐一枚举，这里用 std::apply 一次性展开；仅此一处泛型
// 设施，桥级模块（64 CM 的 wakeup/info/alloc/W valid 广播）共用。
// get 为访问子（Cm& -> Out<V>&），调用点以 lambda 给出（端口是引用成员，
// 无法取成员指针）。

#include <array>
#include <cstdint>
#include <tuple>

#include "wolvicmod/core/module.h"

namespace zj::detail {

// dst[i] = get(*cms[i]) 的当前值。
template <class V, size_t N, class CmArr, class Get>
void collectPorts(wolvicmod::Wire<std::array<V, N>>& dst, CmArr& cms, Get get) {
    std::apply(
        [&](auto*... cm) {
            dst.assign().reads(get(*cm)...) = [](auto src) {
                std::array<V, N> a{};
                uint32_t i = 0;
                std::apply([&](const auto&... v) { ((a[i++] = v), ...); }, src);
                return a;
            };
        },
        cms);
}

}  // namespace zj::detail
