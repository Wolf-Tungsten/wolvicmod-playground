#pragma once

// DataBlock 公共类型与常量（kunminghu-v3 单核配置，语义 docs/dongjiang-semantics.md §4）。
// 维度：Beat 32B、每 cacheline 2 beat；DS 4 bank × 2 beat 的 BeatStorage；
// dbid 7bit（128 项 DataBuffer）；dcid 6bit（64 项 DataCtrlEntry）。

#include <array>
#include <cstdint>

#include "model/dj/dj_types.h"
#include "model/flit/zj_flit.h"

namespace zj::dj {

using zj::chi::DataFlit;

constexpr uint32_t kNrDSBank = 4;
constexpr uint32_t kNrBeat = 2;
constexpr uint32_t kBeatByte = 32;
constexpr uint32_t kNrDsSet = 65536;  // llcSets(16384) × 16way / 4bank
constexpr uint32_t kDsIdxBits = 16;
constexpr uint32_t kNrDataBuf = 128;  // dbid 空间
constexpr uint32_t kDbIdBits = 7;
constexpr uint32_t kNrDataCM = 64;  // dcid 空间
constexpr uint32_t kDcIdBits = 6;
constexpr uint32_t kReadDsLatency = 5;

using Beat = std::array<uint64_t, kBeatByte / 8>;

// DatOpcode（zhujiang/chi/Opcode.scala）
namespace dat_op {
constexpr uint8_t kSnpRespData = 0x1;
constexpr uint8_t kCopyBackWriteData = 0x2;
constexpr uint8_t kNonCopyBackWriteData = 0x3;
constexpr uint8_t kCompData = 0x4;
constexpr uint8_t kSnpRespDataFwded = 0x6;
constexpr uint8_t kNCBWrDataCompAck = 0xc;
}  // namespace dat_op

// ---------------- 操作与索引 ----------------

struct DataOp {  // HasDataOp（data/Bundle.scala:15）
    bool repl = false;
    bool read = false;
    bool send = false;
    bool save = false;
    bool merge = false;

    bool readToDB() const { return repl || read; }
    bool readToDS() const { return repl || save; }
    bool readToCHI() const { return repl || send; }
    bool isValid() const { return repl || read || send || save; }
    bool operator==(const DataOp&) const = default;
};

struct DsIdx {  // data/Bundle.scala:37
    uint8_t bank = 0;   // 2bit
    uint32_t idx = 0;   // 16bit

    // temp = Cat(llcSet(13b), way(4b), dirBank(1b))；bank=temp[1:0]，idx=temp[17:2]
    void set(uint64_t addr, uint32_t way) {
        const uint32_t llcSet = static_cast<uint32_t>((useAddr(addr) >> kDirBankBits) & 0x1FFFu);
        const uint32_t temp = (llcSet << 5) | (way << 1) | uaDirBank(useAddr(addr));
        bank = temp & 3u;
        idx = (temp >> 2) & 0xFFFFu;
    }
    bool operator==(const DsIdx&) const = default;
};

// ---------------- 通道载荷 ----------------

struct ReadDB {  // buf 读请求（→ DataBuffer）
    DsIdx ds;
    uint8_t dcid = 0;
    uint8_t dbid = 0;
    uint8_t beatNum = 0;  // 1bit
    uint8_t qos = 0;      // 4bit
    bool critical = false;
    bool repl = false;

    bool operator==(const ReadDB&) const = default;
};

struct ReadDS {  // DS 读请求（→ BeatStorage）
    DsIdx ds;
    uint8_t dcid = 0;
    uint8_t dbid = 0;
    uint8_t beatNum = 0;
    uint8_t qos = 0;
    bool critical = false;
    bool toCHI = false;

    bool operator==(const ReadDS&) const = default;
};

struct WriteDS {  // DS 写请求
    DsIdx ds;
    uint8_t dcid = 0;
    uint8_t beatNum = 0;
    Beat beat{};

    bool operator==(const WriteDS&) const = default;
};

struct DsResp {  // BeatStorage 读响应
    uint8_t dbid = 0;
    uint8_t dcid = 0;
    uint8_t beatNum = 0;
    Beat beat{};
    bool toCHI = false;

    bool operator==(const DsResp&) const = default;
};

struct DBIDVecC {  // DBIDVec + HasDataVec（release / clean 载荷）
    std::array<uint8_t, kNrBeat> dbidVec{};
    uint8_t dataVec = 0;  // 2bit

    bool operator==(const DBIDVecC&) const = default;
};

struct UpdHnTxnID {
    uint8_t before = 0;  // 7bit
    uint8_t next = 0;

    bool operator==(const UpdHnTxnID&) const = default;
};

struct ReqDB {  // HnTxnID + HasDataVec
    uint8_t hnTxnID = 0;
    uint8_t dataVec = 0;  // 2bit，非零

    bool operator==(const ReqDB&) const = default;
};

struct DataTask {  // data/Bundle.scala:56
    uint8_t hnTxnID = 0;
    DataOp dataOp;
    DsIdx ds;
    uint8_t dataVec = 0;  // 2bit，非零
    uint8_t qos = 0;      // 4bit
    DataFlit txDat;

    bool operator==(const DataTask&) const = default;
};

struct DcidBeat {  // DCID + HasBeatNum（完成通知）
    uint8_t dcid = 0;
    uint8_t beatNum = 0;

    bool operator==(const DcidBeat&) const = default;
};

struct FromCHI {  // PackDataFilt + HasDBID（rxDat 入 buf）
    DataFlit dat;
    uint8_t dbid = 0;

    bool operator==(const FromCHI&) const = default;
};

struct ToCHIEntry {  // PackDataFilt + HasDCID + HasBeatNum（buf → txDat）
    DataFlit dat;
    uint8_t dcid = 0;
    uint8_t beatNum = 0;

    bool operator==(const ToCHIEntry&) const = default;
};

// alloc 载荷：HnTxnID + HasDataVec + HasDBIDVec
struct AllocBits {
    uint8_t hnTxnID = 0;
    uint8_t dataVec = 0;
    std::array<uint8_t, kNrBeat> dbidVec{};

    bool operator==(const AllocBits&) const = default;
};

// clean 载荷：HnTxnID + HasDataVec
struct CleanBits {
    uint8_t hnTxnID = 0;
    uint8_t dataVec = 0;

    bool operator==(const CleanBits&) const = default;
};

// entry 状态输出（dbgVec / getDBID 用）：HnTxnID + HasDataVec + HasDBIDVec
struct EntryState {
    uint8_t hnTxnID = 0;
    uint8_t dataVec = 0;
    std::array<uint8_t, kNrBeat> dbidVec{};

    bool operator==(const EntryState&) const = default;
};

}  // namespace zj::dj
