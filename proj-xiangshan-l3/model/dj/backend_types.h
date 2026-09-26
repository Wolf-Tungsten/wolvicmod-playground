#pragma once

// Backend 共享载荷类型（backend/Bundle.scala + dongjiang/bundle/*）。
// TaskCode(24b)/CommitCode(29b)/TaskInst(19b) 保持打包位图，位段见 dj_decode.h；
// 其余按运行期结构体。语义 docs/dongjiang-semantics.md §5。

#include <array>
#include <cstdint>

#include "model/dj/data_types.h"
#include "model/dj/dj_decode.h"
#include "model/dj/dj_types.h"
#include "model/flit/zj_flit.h"

namespace zj::dj {

using zj::chi::DataFlit;
using zj::chi::HReqFlit;
using zj::chi::ReqFlitT;
using zj::chi::RespFlit;
using zj::chi::SnoopFlit;

// ---------------- HasChi（DJBundles.scala:127） ----------------

struct Chi {  // HasNodeId + channel + op + order/expCA + snpField + dataVec + HasChi
    uint16_t nodeId = 0;    // 11bit
    uint8_t channel = 0;    // 2bit：REQ=0 DAT=1 RSP=2 SNP=3
    uint8_t opcode = 0;     // ≤7bit
    uint8_t order = 0;      // 2bit
    bool expCompAck = false;
    bool snpAttr = false;
    bool snoopMe = false;
    uint8_t dataVec = 0;    // 2bit
    uint16_t txnID = 0;     // 12bit
    uint8_t memAttr = 0;    // 4bit：allocate(3) cacheable(2) device(1) ewa(0)（先声明 MSB）
    uint8_t size = 0;       // 3bit
    uint16_t fwdNID = 0;    // 11bit
    uint16_t fwdTxnID = 0;  // 12bit
    bool retToSrc = false;
    bool toLAN = false;

    bool toBBN() const { return !toLAN; }
    uint16_t getNoC() const { return toLAN ? 0 : 1; }  // LAN=0, BBN=1
    bool memAllocate() const { return (memAttr >> 3) & 1; }
    bool memCacheable() const { return (memAttr >> 2) & 1; }
    bool memDevice() const { return (memAttr >> 1) & 1; }
    bool memEwa() const { return memAttr & 1; }
    bool isCopyBackWrite() const {
        // CopyBack 写族：WriteUnique*/WriteBack*/WriteClean*/WriteEvict*（Opcode 高位段）
        // 见 ReqOpcode：0x17/0x18/0x19/0x1a/0x1b/0x42(WriteEvictOrEvict)/0x20/0x21
        return opcode == 0x17 || opcode == 0x18 || opcode == 0x19 || opcode == 0x1a ||
               opcode == 0x1b || opcode == 0x42 || opcode == 0x20 || opcode == 0x21;
    }
    bool isImmediateWrite() const { return opcode == 0x1c || opcode == 0x1d; }  // WriteNoSnp*
    bool operator==(const Chi&) const = default;
};

// ---------------- CommitTask（frontend → Commit） ----------------

struct Already {
    bool reqDB = false;
    bool sData = false;
    bool sDBID = false;

    bool operator==(const Already&) const = default;
};

struct CommitTask {
    Chi chi;
    DirMsg dir;           // HasPackDirMsg
    Already alr;          // HasAlready
    DsIdx ds;             // HasDsIdx
    std::array<uint8_t, 4> decList{};  // (ci, si, ti, sti)
    uint32_t task = 0;    // TaskCode 打包（dectab 位段）
    uint32_t cmt = 0;     // CommitCode 打包
    uint8_t qos = 0;      // 4bit
    uint8_t hnTxnID = 0;  // 7bit（CommitTask with HasHnTxnID 外层字段）

    // isReplLLC = cmt.wriLLC & !dir.llc.hit
    bool isReplLLC() const { return dectab::ccWriLLC(cmt) && !dir.llc.hit; }
    bool operator==(const CommitTask&) const = default;
};

// ---------------- CMTask / CMResp / ReplTask ----------------

struct CMTask {
    uint8_t hnTxnID = 0;  // 7bit
    Chi chi;
    DataOp dataOp;
    DsIdx ds;
    bool fromRepl = false;
    uint8_t snpVec = 0;   // nrSfMetas=1
    uint8_t cbResp = 0;   // ChiResp 3bit
    bool doDMT = false;
    uint8_t qos = 0;

    bool operator==(const CMTask&) const = default;
};

struct CMResp {
    uint8_t hnTxnID = 0;
    uint32_t taskInst = 0;  // TaskInst 打包
    bool toRepl = false;
    uint8_t qos = 0;
    uint8_t respErr = 0;  // 2bit

    bool operator==(const CMResp&) const = default;
};

struct ReplTask {
    uint8_t hnTxnID = 0;
    DirMsg dir;
    uint8_t qos = 0;
    bool wriSF = false;
    bool wriLLC = false;
    bool directAllocSF = false;

    bool isDirectAllocSF() const { return wriSF && !dir.sf.hit && directAllocSF; }
    bool isReplSF() const { return wriSF && !dir.sf.hit && !directAllocSF; }
    bool isReplLLC() const { return wriLLC && !dir.llc.hit; }
    bool isReplDIR() const { return isReplSF() || isReplLLC(); }
    bool operator==(const ReplTask&) const = default;
};

// ---------------- PosClean（frontend/PosClean，backend 内传播） ----------------

struct PosClean {
    uint8_t hnIdx = 0;    // 7bit
    uint8_t channel = 0;  // 2bit
    uint8_t qos = 0;

    bool operator==(const PosClean&) const = default;
};

// reqPosVec2 元素（ChiChnlBundle：仅 channel）
struct ReqPos {
    uint8_t channel = 0;

    bool operator==(const ReqPos&) const = default;
};

// updPosTag：Addr + addrVal + hnIdx
struct UpdPosTag {
    uint64_t addr = 0;
    bool addrVal = false;
    uint8_t hnIdx = 0;

    bool operator==(const UpdPosTag&) const = default;
};

// reqDB（Commit/Replace 侧）：HnTxnID + HasDataVec + HasQoS
struct ReqDBQos {
    uint8_t hnTxnID = 0;
    uint8_t dataVec = 0;
    uint8_t qos = 0;

    bool operator==(const ReqDBQos&) const = default;
};

}  // namespace zj::dj
