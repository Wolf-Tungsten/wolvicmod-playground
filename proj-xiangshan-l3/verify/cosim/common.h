#pragma once

// cosim 基础设施：拍协议、失配报告（含前 16 拍激励回放）、密度激励工具。
//
// 拍协议（两侧严格同步）：
//   每拍 = 驱动本拍输入 → clk=0 eval（组合稳态）→ 采样比对全部输出 →
//          clk=1 eval（提交本拍）。
// 参考侧（Verilator）先施加复位：reset 拉高 4 拍（输入清零）再撤；
// wolvicmod 侧初始态即复位后态，撤复位后的第 0 拍开始比对。

#include <cstdint>
#include <deque>
#include <iostream>
#include <random>
#include <string>

namespace cosim {

struct Replay {
    std::deque<std::string> lines;

    void push(std::string s) {
        lines.push_back(std::move(s));
        if (lines.size() > 16) lines.pop_front();
    }
};

struct Stats {
    uint64_t checks = 0;
    uint64_t mismatches = 0;
};

inline void check(Stats& st, const char* comp, const char* cfg, uint32_t seed, uint64_t cyc,
                  const char* port, uint64_t refV, uint64_t dutV, const Replay& rp) {
    ++st.checks;
    if (refV == dutV) return;
    ++st.mismatches;
    if (st.mismatches > 5) return;  // 每 run 只报前 5 条，计数不停
    std::cout << "MISMATCH comp=" << comp << " cfg=" << cfg << " seed=" << seed
              << " cyc=" << cyc << " port=" << port << " ref=0x" << std::hex << refV
              << " dut=0x" << dutV << std::dec << "\n";
    std::cout << "  last-16 stimulus replay:\n";
    for (const auto& s : rp.lines) std::cout << "    " << s << "\n";
}

// 带 lane 的端口名（out_valid[3] 等）
inline std::string lanePort(const char* port, uint32_t lane) {
    return std::string(port) + "[" + std::to_string(lane) + "]";
}

// 拍协议两段（Verilator 模型字段直写；wolvicmod 模型 set/eval）
template <class VRef, class Dut>
void phaseLow(VRef& ref, Dut& dut) {
    ref.clock = 0;
    ref.eval();
    dut.clk.set(0);
    dut.eval();
}

template <class VRef, class Dut>
void phaseHigh(VRef& ref, Dut& dut) {
    ref.clock = 1;
    ref.eval();
    dut.clk.set(1);
    dut.eval();
}

// 参考侧复位 4 拍（输入清零），撤复位后落在 clk=0 稳态
template <class VRef, class ZeroFn>
void resetRef(VRef& ref, ZeroFn zeroInputs) {
    zeroInputs();
    ref.reset = 1;
    for (int i = 0; i < 4; ++i) {
        ref.clock = 0;
        ref.eval();
        ref.clock = 1;
        ref.eval();
    }
    ref.reset = 0;
    ref.clock = 0;
    ref.eval();
}

// 密度分段：返回本拍占空比（%），按段 100/50/10 轮换
inline uint32_t densityAt(uint64_t cyc, uint64_t cycles) {
    const uint64_t seg = cycles / 3;
    if (cyc < seg) return 100;
    if (cyc < 2 * seg) return 50;
    return 10;
}

inline bool roll(std::mt19937& rng, uint32_t pct) {
    return static_cast<uint32_t>(rng() % 100) < pct;
}

}  // namespace cosim
