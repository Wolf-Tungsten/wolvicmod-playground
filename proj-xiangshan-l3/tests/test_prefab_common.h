#pragma once

// 项目侧 prefab 测试公共驱动惯例：每拍 = set 输入 → comb()（clk 0 eval，
// 此时组合输出即"本拍"取值，逐拍采样比对）→ edge()（clk 1 eval，NBA 提交）。

#include <wolvicmod/wolvicmod.h>

namespace prefabtest {

template <class M>
void comb(M& top) {
    top.clk.set(0);
    top.eval();
}

template <class M>
void edge(M& top) {
    top.clk.set(1);
    top.eval();
}

// 无采样的一整拍
template <class M>
void cycle(M& top) {
    comb(top);
    edge(top);
}

}  // namespace prefabtest
