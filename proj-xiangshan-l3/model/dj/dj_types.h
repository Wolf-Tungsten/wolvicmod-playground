#pragma once

// DongJiang 公共类型与纯函数（kunminghu-v3 DefaultConfig + LLC=ZhuJiang 单核
// 真实配置，推导见 docs/wolvicmod-zhujiang-model.md §5.1）：
//   - 地址切片：useAddr = addr[47:13]++addr[11:6]（剔除 bank 位 addr[12]），
//     dirBank/llcSet/sfSet/tag 均从 useAddr 切
//   - HnIdx：7bit {dirBank[6], posSet[5:4], posWay[3:0]}（= hnTxnID）
//   - PLRU：rocket-chip PseudoLRU（util/Replacement.scala）同构 constexpr 实现
//   - 目录读写请求/响应载荷

#include <array>
#include <cstdint>

namespace zj::dj {

// ---------------- 地址切片（DJParameters.scala:192-205） ----------------

constexpr uint32_t kAddrBits = 48;
constexpr uint32_t kBankOff = 12;    // hnxBankOff
constexpr uint32_t kOffsetBits = 6;  // 64B cache line
constexpr uint32_t kDirBankBits = 1;
constexpr uint32_t kWays = 16;       // llc/sf 同 16 路
constexpr uint32_t kPosSets = 4;     // 每 dirBank
constexpr uint32_t kPosWays = 16;

// useAddr(40:0) = addr[47:13] ++ addr[11:6]
constexpr uint64_t useAddr(uint64_t a) { return ((a >> 13) << 6) | ((a >> 6) & 0x3FULL); }

constexpr uint32_t uaDirBank(uint64_t ua) { return static_cast<uint32_t>(ua) & 1u; }

// 由 {bankId, tag, set, dirBank} 重组全地址（catByX，offset=0 —— victim 地址）：
// useAddr_ = Cat(tag, set, dirBank)；addr = useAddr_[40:6]++bank++useAddr_[5:0]++0(6)
constexpr uint64_t catAddr(uint32_t bankId, uint64_t tag, uint32_t set, uint32_t setBits,
                           uint32_t dirBank) {
    const uint64_t ua = (tag << (setBits + kDirBankBits)) | (set << kDirBankBits) | dirBank;
    return ((ua >> 6) << 13) | (static_cast<uint64_t>(bankId) << 12) | ((ua & 0x3FULL) << 6);
}

// ---------------- HnIdx / hnTxnID（DJBundles.scala:74-113） ----------------
// 7bit：{dirBank[6], posSet[5:4], posWay[3:0]}

constexpr uint8_t hnIdxOf(uint32_t dirBank, uint32_t posSet, uint32_t posWay) {
    return static_cast<uint8_t>((dirBank << 6) | (posSet << 4) | posWay);
}
constexpr uint32_t hnIdxDirBank(uint8_t h) { return h >> 6; }
constexpr uint32_t hnIdxPosSet(uint8_t h) { return (h >> 4) & 3u; }
constexpr uint32_t hnIdxPosWay(uint8_t h) { return h & 0xFu; }

// ci = addr[47:44]
constexpr uint32_t ciOf(uint64_t a) { return static_cast<uint32_t>(a >> 44); }

// 一位热 → 二进制（chisel OHToUInt；0 输入 → 0）
constexpr uint8_t ohToUInt(uint32_t oh) {
    if (oh == 0) return 0;
    uint8_t i = 0;
    while (((oh >> i) & 1u) == 0) ++i;
    return i;
}

// ---------------- PLRU（rocket-chip PseudoLRU 同构） ----------------
// state 15bit（16 路二叉树）：state(tree-2)=节点位（1 ⇒ 左子树更老），
// 左子树状态 = state(tree-3, right-1)，右子树 = state(right-2, 0)。

constexpr uint32_t plruLog2(uint32_t n) { return n <= 1 ? 0 : 1 + plruLog2((n + 1) / 2); }

// state 低 treeWays-1 位有效；返回 way（log2ceil(treeWays) 位）
constexpr uint32_t plruReplaceWay(uint32_t state, uint32_t treeWays) {
    if (treeWays > 2) {
        const uint32_t right = 1u << (plruLog2(treeWays) - 1);
        const uint32_t left = treeWays - right;
        const uint32_t nodeBit = (state >> (treeWays - 2)) & 1u;
        const uint32_t leftState = (state >> (right - 1)) & ((1u << (left - 1)) - 1u);
        const uint32_t rightState = state & ((1u << (right - 1)) - 1u);
        const uint32_t sub =
            nodeBit ? plruReplaceWay(leftState, left) : plruReplaceWay(rightState, right);
        return (nodeBit << plruLog2(right)) | sub;
    }
    return state & 1u;  // treeWays == 2（treeWays<=1 本配置用不到）
}

// touchWay 为子树内编码（log2ceil(treeWays) 位）
constexpr uint32_t plruNextState(uint32_t state, uint32_t touchWay, uint32_t treeWays) {
    if (treeWays > 2) {
        const uint32_t right = 1u << (plruLog2(treeWays) - 1);
        const uint32_t left = treeWays - right;
        const uint32_t setLeftOlder = ((touchWay >> (plruLog2(treeWays) - 1)) & 1u) ^ 1u;
        const uint32_t leftState = (state >> (right - 1)) & ((1u << (left - 1)) - 1u);
        const uint32_t rightState = state & ((1u << (right - 1)) - 1u);
        const uint32_t newLeft =
            setLeftOlder ? leftState
                         : plruNextState(leftState, touchWay & ((1u << plruLog2(left)) - 1u), left);
        const uint32_t newRight =
            setLeftOlder
                ? plruNextState(rightState, touchWay & ((1u << plruLog2(right)) - 1u), right)
                : rightState;
        return (setLeftOlder << (treeWays - 2)) | (newLeft << (right - 1)) | newRight;
    }
    return (touchWay & 1u) ^ 1u;  // treeWays == 2
}

// ---------------- 目录请求/响应载荷（directory/Bundle.scala） ----------------

// ChiState：llc 2bit {I=0,SC=1,UD=2,UC=3}；sf 1bit。两者 isValid 均为 state != 0。
using DirMeta = uint8_t;

struct DirRdReq {  // Addr + HasPackHnIdx
    uint64_t addr = 0;  // 48bit
    uint8_t hnIdx = 0;

    bool operator==(const DirRdReq&) const = default;
};

struct DirWrReq {  // DirEntry + HasPackHnIdx + HasDirectAlloc
    uint64_t addr = 0;      // 48bit
    uint16_t wayOH = 0;     // 16bit 一位热
    DirMeta meta = 0;       // nrMetas=1：单 meta
    uint8_t hnIdx = 0;
    bool hit = false;
    bool directAlloc = false;

    bool operator==(const DirWrReq&) const = default;
};

struct DirResp {  // DirEntry + HasHnTxnID + toRepl
    uint64_t addr = 0;      // victim 地址（offset=0）
    uint16_t wayOH = 0;
    DirMeta meta = 0;
    uint8_t hnTxnID = 0;    // 7bit
    bool hit = false;
    bool toRepl = false;

    bool operator==(const DirResp&) const = default;
};

// Directory 顶层写通道：llc/sf 各自 Valid（bundle 内嵌 Valid 语义）
struct DirWrBoth {
    DirWrReq llc;
    DirWrReq sf;
    bool llcValid = false;
    bool sfValid = false;

    bool operator==(const DirWrBoth&) const = default;
};

// rRespVec 载荷（DirMsg：只有 wayOH/hit/metaVec，无 addr/hnTxnID）
struct DirMsgHalf {
    uint16_t wayOH = 0;
    DirMeta meta = 0;
    bool hit = false;

    bool operator==(const DirMsgHalf&) const = default;
};

struct DirMsg {
    DirMsgHalf llc;
    DirMsgHalf sf;

    bool operator==(const DirMsg&) const = default;
};

}  // namespace zj::dj
