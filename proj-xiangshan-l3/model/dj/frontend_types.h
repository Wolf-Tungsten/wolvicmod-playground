#pragma once

// Frontend 共享载荷类型（frontend/* 语见 docs/wolvicmod-zhujiang-model.md §5.7）。

#include <array>
#include <cstdint>

#include "model/dj/backend_types.h"

namespace zj::dj {

// PackChi with HasAddr with HasQoS（TaskBuffer/Block 间传递的任务）
struct ChiTask {
    Chi chi;
    uint64_t addr = 0;  // 48bit
    uint8_t qos = 0;

    bool operator==(const ChiTask&) const = default;
};

// task_s1/task_s2：PackChi + addr + hnIdx + alr + qos
struct TaskS1 {
    Chi chi;
    uint64_t addr = 0;
    uint8_t hnIdx = 0;
    Already alr;
    uint8_t qos = 0;

    bool operator==(const TaskS1&) const = default;
};

// PosEntry 状态
struct PosState {
    bool req = false;
    bool snp = false;
    bool tagVal = false;
    uint64_t tag = 0;    // posTagBits=38
    uint8_t offset = 0;  // 6bit

    bool one() const { return req != snp; }
    bool two() const { return req && snp; }
    bool valid() const { return req || snp; }
    bool operator==(const PosState&) const = default;
};

// TaskEntry 状态（one-hot）
namespace taskst {
constexpr uint8_t kFree = 0x1, kSend = 0x2, kWait = 0x4, kSleep = 0x8;
}

// posTag/posSet/posTagBits（DJParameters：posTagBits=38, posSet=useAddr[2:1]）
constexpr uint64_t posTagOf(uint64_t a) { return useAddr(a) >> 3; }
constexpr uint32_t posSetOf(uint64_t a) { return static_cast<uint32_t>((useAddr(a) >> 1) & 3); }
// catPoS：由 {bankId, tag, set, dirBank} 重组地址（offset 可选）
constexpr uint64_t catPosAddr(uint32_t bankId, uint64_t tag, uint32_t set, uint32_t dirBank,
                              uint32_t offset = 0) {
    return catAddr(bankId, tag, set, 2, dirBank) | offset;
}

}  // namespace zj::dj
