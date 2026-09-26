// Backend 对拍 harness：wolvicmod Backend vs refgenDj RTL Backend
// （dongjiang/backend 真实源码，kunminghu-v3 单核 32MB 配置）。
// 激励：HN 环境全模拟——
//   请求生成器：按 frontend/decode 表合法生成 CommitTask（ci/state 随机，
//     task=getTaskCode；task 无效时 cmt=getCommitCode(0,0)），PoS 槽位全生命
//     周期跟踪（cleanPoS 释放）；
//   响应器：CHI（读 CompData/写 DBIDResp+Comp+RN 写数据/snoop 按 opcode 取表内
//     合法响应变体/CompAck）、目录 wResp（替换写随机 victim）、dataResp、
//     posResp（free way 分配）、getAddrVec 地址表（updPosTag 跟写）。
// 每拍比对全部输出端口。

#include <array>
#include <cstdint>
#include <deque>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "VBackend.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include "model/dj/backend.h"
#include "model/dj/dj_decode.h"

using namespace wolvicmod;
using namespace zj::dj;

namespace dc = dectab;

namespace {

constexpr uint8_t kSnpResp = 0x1, kCompAck = 0x2, kComp = 0x4, kCompDBIDResp = 0x5,
                  kDBIDResp = 0x6, kSnpRespFwded = 0x9;
constexpr uint8_t kSnpRespData = 0x1, kCopyBackWriteData = 0x2, kNonCopyBackWriteData = 0x3,
                  kCompData = 0x4, kSnpRespDataFwded = 0x6, kNCBWrDataCompAck = 0xc;

uint64_t randAddr(std::mt19937& rng) { return catAddr(0, rng() % 24, rng() % 4, 13, rng() & 1); }

// ---------------- 槽位/地址/way 跟踪 ----------------
struct Slot {
    bool active = false;
    uint64_t addr = 0;
    bool expCompAck = false;
    uint8_t wrDataOp = 0;   // 写数据 opcode（0 表示不需要）
    int wrDataAt = -1;
    int compAckAt = -1;
};

struct WayPool {
    std::array<std::array<std::array<bool, 16>, 4>, 2> used{};
    int alloc(uint32_t b, uint32_t s, std::mt19937& rng) {
        for (uint32_t w = 14; w < 16; ++w)
            if (!used[b][s][w]) {
                used[b][s][w] = true;
                return static_cast<int>(w);
            }
        std::vector<int> fr;
        for (uint32_t w = 0; w < 14; ++w)
            if (!used[b][s][w]) fr.push_back(w);
        if (fr.empty()) return -1;
        const int w = fr[rng() % fr.size()];
        used[b][s][w] = true;
        return w;
    }
    void free(uint8_t hnIdx) {
        used[hnIdxDirBank(hnIdx)][hnIdxPosSet(hnIdx)][hnIdxPosWay(hnIdx)] = false;
    }
};

// ---------------- 事件队列 ----------------
struct EvRsp { uint64_t at; RespFlit f; };
struct EvDat { uint64_t at; DataFlit f; };
struct EvDirW { uint64_t at; bool sf; DirResp r; };
struct EvDataR { uint64_t at; uint8_t txn; };
struct EvPosR { uint64_t at; uint8_t b, s, way; };

struct SnoopPlan {
    bool fwd = false;
    uint8_t resp = 0;
    uint8_t fwdSt = 0;
    bool useData = false;
};

SnoopPlan planSnoop(std::mt19937& rng, uint8_t op) {
    Resp:
    SnoopPlan p;
    const uint32_t roll = rng() % 100;
    switch (op) {
        case 0x13:
            if (roll < 40) p = {true, 0, 0, false};
            else if (roll < 55) p = {true, 1, 0, false};
            else if (roll < 65) p = {true, 2, 0, false};
            else if (roll < 75) p = {true, 3, 0, false};
            else if (roll < 85) p = {true, 5, 0, true};
            else if (roll < 93) p = {true, 4, 0, true};
            else p = {false, static_cast<uint8_t>(roll & 1), 0, false};
            break;
        case 0x14:
            if (roll < 30) p = {true, 1, 1, false};
            else if (roll < 50) p = {true, 0, 1, false};
            else if (roll < 65) p = {true, 1, 1, true};
            else if (roll < 75) p = {true, 0, 1, true};
            else if (roll < 85) p = {true, 5, 1, true};
            else if (roll < 93) p = {true, 4, 1, true};
            else p = {false, 0, 0, false};
            break;
        case 0x17:
            if (roll < 40) p = {true, 0, 2, false};
            else if (roll < 60) p = {true, 0, 6, false};
            else if (roll < 80) p = {false, 4, 0, true};
            else p = {false, 0, 0, false};
            break;
        case 0x07:
            if (roll < 40) p = {false, 4, 0, true};
            else if (roll < 65) p = {false, 0, 0, true};
            else p = {false, 0, 0, false};
            break;
        case 0x08:
            if (roll < 30) p = {false, 0, 0, false};
            else if (roll < 45) p = {false, 2, 0, false};
            else if (roll < 55) p = {false, 1, 0, false};
            else if (roll < 75) p = {false, 6, 0, true};
            else if (roll < 90) p = {false, 5, 0, true};
            else p = {false, 4, 0, true};
            break;
        case 0x09:
            if (roll < 50) p = {false, 0, 0, false};
            else p = {false, 4, 0, true};
            break;
        default: p = {false, 0, 0, false}; break;
    }
    return p;
}

}  // namespace
namespace {

// ---------------- RTL 输入驱动 ----------------
void setRxRsp(VBackend& ref, const Valid<RespFlit>& v) {
    ref.io_rxRsp_valid = v.valid;
    ref.io_rxRsp_bits_DBID = v.bits.dbid;
    ref.io_rxRsp_bits_CBusy = v.bits.c_busy;
    ref.io_rxRsp_bits_FwdState = v.bits.fwd_state;
    ref.io_rxRsp_bits_Resp = v.bits.resp;
    ref.io_rxRsp_bits_RespErr = v.bits.resp_err;
    ref.io_rxRsp_bits_Opcode = v.bits.opcode;
    ref.io_rxRsp_bits_TxnID = v.bits.txn_id;
    ref.io_rxRsp_bits_SrcID = v.bits.src_id;
    ref.io_rxRsp_bits_TgtID = v.bits.tgt_id;
    ref.io_rxRsp_bits_QoS = v.bits.qos;
}
void setRxDat(VBackend& ref, const Valid<DataFlit>& v) {
    ref.io_rxDat_valid = v.valid;
    for (int w = 0; w < 4; ++w) {
        ref.io_rxDat_bits_Data[2 * w] = static_cast<uint32_t>(v.bits.data[w]);
        ref.io_rxDat_bits_Data[2 * w + 1] = static_cast<uint32_t>(v.bits.data[w] >> 32);
    }
    ref.io_rxDat_bits_BE = static_cast<uint32_t>(v.bits.be);
    ref.io_rxDat_bits_DataID = v.bits.data_id;
    ref.io_rxDat_bits_DBID = v.bits.dbid;
    ref.io_rxDat_bits_CBusy = v.bits.c_busy;
    ref.io_rxDat_bits_DataSource = v.bits.data_source;
    ref.io_rxDat_bits_Resp = v.bits.resp;
    ref.io_rxDat_bits_RespErr = v.bits.resp_err;
    ref.io_rxDat_bits_Opcode = v.bits.opcode;
    ref.io_rxDat_bits_HomeNID = v.bits.home_nid;
    ref.io_rxDat_bits_TxnID = v.bits.txn_id;
    ref.io_rxDat_bits_SrcID = v.bits.src_id;
    ref.io_rxDat_bits_TgtID = v.bits.tgt_id;
    ref.io_rxDat_bits_QoS = v.bits.qos;
}
void setRespDir(VBackend& ref, bool sf, const Valid<DirResp>& v) {
    if (sf) {
        ref.io_respDir_sf_valid = v.valid;
        ref.io_respDir_sf_bits_addr = v.bits.addr;
        ref.io_respDir_sf_bits_wayOH = v.bits.wayOH;
        ref.io_respDir_sf_bits_hit = v.bits.hit;
        ref.io_respDir_sf_bits_metaVec_0_state = v.bits.meta;
        ref.io_respDir_sf_bits_hnTxnID = v.bits.hnTxnID;
    } else {
        ref.io_respDir_llc_valid = v.valid;
        ref.io_respDir_llc_bits_addr = v.bits.addr;
        ref.io_respDir_llc_bits_wayOH = v.bits.wayOH;
        ref.io_respDir_llc_bits_hit = v.bits.hit;
        ref.io_respDir_llc_bits_metaVec_0_state = v.bits.meta;
        ref.io_respDir_llc_bits_hnTxnID = v.bits.hnTxnID;
    }
}
void setFastResp(VBackend& ref, const Valid<RespFlit>& v) {
    ref.io_fastResp_valid = v.valid;
    ref.io_fastResp_bits_DBID = v.bits.dbid;
    ref.io_fastResp_bits_CBusy = v.bits.c_busy;
    ref.io_fastResp_bits_FwdState = v.bits.fwd_state;
    ref.io_fastResp_bits_Resp = v.bits.resp;
    ref.io_fastResp_bits_RespErr = v.bits.resp_err;
    ref.io_fastResp_bits_Opcode = v.bits.opcode;
    ref.io_fastResp_bits_TxnID = v.bits.txn_id;
    ref.io_fastResp_bits_SrcID = v.bits.src_id;
    ref.io_fastResp_bits_TgtID = v.bits.tgt_id;
    ref.io_fastResp_bits_QoS = v.bits.qos;
}

#define CMT_FIELDS(P, t)                                                                          \
    P##_bits_hnTxnID = t.hnTxnID;                                                                 \
    P##_bits_qos = t.qos;                                                                         \
    P##_bits_chi_channel = t.chi.channel;                                                         \
    P##_bits_chi_nodeId = t.chi.nodeId;                                                           \
    P##_bits_chi_opcode = t.chi.opcode;                                                           \
    P##_bits_chi_order = t.chi.order;                                                             \
    P##_bits_chi_expCompAck = t.chi.expCompAck;                                                   \
    P##_bits_chi_snpAttr = t.chi.snpAttr;                                                         \
    P##_bits_chi_snoopMe = t.chi.snoopMe;                                                         \
    P##_bits_chi_dataVec_0 = t.chi.dataVec & 1;                                                   \
    P##_bits_chi_dataVec_1 = (t.chi.dataVec >> 1) & 1;                                            \
    P##_bits_chi_txnID = t.chi.txnID;                                                             \
    P##_bits_chi_memAttr_allocate = t.chi.memAllocate();                                          \
    P##_bits_chi_memAttr_cacheable = t.chi.memCacheable();                                        \
    P##_bits_chi_memAttr_device = t.chi.memDevice();                                              \
    P##_bits_chi_memAttr_ewa = t.chi.memEwa();                                                    \
    P##_bits_chi_size = t.chi.size;                                                               \
    P##_bits_chi_fwdNID = t.chi.fwdNID;                                                           \
    P##_bits_chi_fwdTxnID = t.chi.fwdTxnID;                                                       \
    P##_bits_chi_retToSrc = t.chi.retToSrc;                                                       \
    P##_bits_chi_toLAN = t.chi.toLAN;                                                             \
    P##_bits_chi_fromLAN = 1;                                                                     \
    P##_bits_dir_llc_hit = t.dir.llc.hit;                                                         \
    P##_bits_dir_llc_wayOH = t.dir.llc.wayOH;                                                     \
    P##_bits_dir_llc_metaVec_0_state = t.dir.llc.meta;                                            \
    P##_bits_dir_sf_hit = t.dir.sf.hit;                                                           \
    P##_bits_dir_sf_wayOH = t.dir.sf.wayOH;                                                       \
    P##_bits_dir_sf_metaVec_0_state = t.dir.sf.meta;                                              \
    P##_bits_alr_reqDB = t.alr.reqDB;                                                             \
    P##_bits_alr_sData = t.alr.sData;                                                             \
    P##_bits_alr_sDBID = t.alr.sDBID;                                                             \
    P##_bits_ds_bank = t.ds.bank;                                                                 \
    P##_bits_ds_idx = t.ds.idx;                                                                   \
    P##_bits_decList_0 = t.decList[0];                                                            \
    P##_bits_decList_1 = t.decList[1];                                                            \
    P##_bits_decList_2 = t.decList[2];                                                            \
    P##_bits_task_snoop = dc::tcSnoop(t.task);                                                    \
    P##_bits_task_read = dc::tcRead(t.task);                                                      \
    P##_bits_task_dataless = dc::tcDataless(t.task);                                              \
    P##_bits_task_write = dc::tcWrite(t.task);                                                    \
    P##_bits_task_dataOp_repl = dc::tcOpRepl(t.task);                                             \
    P##_bits_task_dataOp_read = dc::tcOpRead(t.task);                                             \
    P##_bits_task_dataOp_send = dc::tcOpSend(t.task);                                             \
    P##_bits_task_dataOp_save = dc::tcOpSave(t.task);                                             \
    P##_bits_task_dataOp_merge = dc::tcOpMerge(t.task);                                           \
    P##_bits_task_opcode = dc::tcOpcode(t.task);                                                  \
    P##_bits_task_needDB = dc::tcNeedDB(t.task);                                                  \
    P##_bits_task_returnDBID = dc::tcReturnDBID(t.task);                                          \
    P##_bits_task_expCompAck = dc::tcExpCompAck(t.task);                                          \
    P##_bits_task_doDMT = dc::tcDoDMT(t.task);                                                    \
    P##_bits_task_retToSrc = dc::tcRetToSrc(t.task);                                              \
    P##_bits_task_snpTgt = dc::tcSnpTgt(t.task);                                                  \
    P##_bits_task_fullSize = dc::tcFullSize(t.task);                                              \
    P##_bits_cmt_wriSRC = dc::ccWriSRC(t.cmt);                                                    \
    P##_bits_cmt_wriSNP = dc::ccWriSNP(t.cmt);                                                    \
    P##_bits_cmt_wriLLC = dc::ccWriLLC(t.cmt);                                                    \
    P##_bits_cmt_srcValid = dc::ccSrcValid(t.cmt);                                                \
    P##_bits_cmt_snpValid = dc::ccSnpValid(t.cmt);                                                \
    P##_bits_cmt_llcState = dc::ccLlcState(t.cmt);                                                \
    P##_bits_cmt_dataOp_repl = dc::ccOpRepl(t.cmt);                                               \
    P##_bits_cmt_dataOp_read = dc::ccOpRead(t.cmt);                                               \
    P##_bits_cmt_dataOp_send = dc::ccOpSend(t.cmt);                                               \
    P##_bits_cmt_dataOp_save = dc::ccOpSave(t.cmt);                                               \
    P##_bits_cmt_dataOp_merge = dc::ccOpMerge(t.cmt);                                             \
    P##_bits_cmt_waitSecDone = dc::ccWaitSecDone(t.cmt);                                          \
    P##_bits_cmt_sendResp = dc::ccSendResp(t.cmt);                                                \
    P##_bits_cmt_sendfwdResp = dc::ccSendFwdResp(t.cmt);                                          \
    P##_bits_cmt_channel = dc::ccChannel(t.cmt);                                                  \
    P##_bits_cmt_opcode = dc::ccOpcode(t.cmt);                                                    \
    P##_bits_cmt_resp = dc::ccResp(t.cmt);                                                        \
    P##_bits_cmt_fwdResp = dc::ccFwdResp(t.cmt);                                                  \
    P##_bits_cmt_fullSize = dc::ccFullSize(t.cmt);

void setCmtTask(VBackend& ref, int bank, const Valid<CommitTask>& v) {
    if (bank == 0) {
        ref.io_cmtTaskVec_0_valid = v.valid;
        CMT_FIELDS(ref.io_cmtTaskVec_0, v.bits)
    } else {
        ref.io_cmtTaskVec_1_valid = v.valid;
        CMT_FIELDS(ref.io_cmtTaskVec_1, v.bits)
    }
}

}  // namespace
namespace {

// ---------------- 请求生成 ----------------
struct Gen {
    std::mt19937& rng;
    WayPool& pool;
    std::array<Slot, 128>& slots;
    std::array<uint64_t, 128>& addrTab;

    // family 参数包
    struct Fam {
        uint8_t opcode;       // ReqOpcode
        uint8_t kind;         // 0=Read 1=Write 2=Dataless
        bool expCompAck;
        bool allocate, ewa, fullSize;
        uint8_t order;        // 0=None 1=RA 2=RO/OWO 3=EO
        bool useOWO;          // order=OWO(2) 且 expCompAck（isOWO 定义）
    };

    explicit Gen(std::mt19937& r, WayPool& p, std::array<Slot, 128>& s,
                 std::array<uint64_t, 128>& a)
        : rng(r), pool(p), slots(s), addrTab(a) {}

    Fam randFam() {
        const uint32_t k = rng() % 14;
        switch (k) {
            case 0: return {0x04, 0, false, false, false, false, 3, false};  // readNoSnp noECA_EO
            case 1: return {0x04, 0, true, false, false, false, 3, false};   // readNoSnp ECA_EO
            case 2: return {0x04, 0, true, false, true, false, 3, false};    // readNoSnp ECA_EO_ewa
            case 3: return {0x03, 0, true, false, false, false, 3, false};   // readOnce noAlloc
            case 4: return {0x03, 0, true, true, false, false, 3, false};    // readOnce alloc
            case 5: return {0x03, 0, true, true, true, true, 3, false};      // readOnce alloc ewa full
            case 6: return {0x26, 0, true, true, true, true, 0, false};      // readNotSharedDirty
            case 7: return {0x07, 0, true, true, true, true, 0, false};      // readUnique
            case 8: return {0x22, 0, false, true, true, true, 0, false};     // stashOnceShared
            case 9: return {0x1c, 1, false, false, false, false, 3, false};  // writeNoSnpPtl noEWA_EO
            case 10: return {0x1c, 1, true, false, false, false, 2, true};   // writeNoSnpPtl OWO
            case 11: return {0x18, 1, true, false, false, false, 2, true};   // writeUniquePtl noEWA
            case 12: return {0x18, 1, true, true, true, false, 2, true};     // writeUniquePtl ewa alloc
            default: {
                const uint32_t d = rng() % 5;
                switch (d) {
                    case 0: return {0x0c, 2, true, false, false, false, 0, false};   // makeUnique
                    case 1: return {0x0d, 2, false, false, false, false, 0, false};  // evict
                    case 2: return {0x08, 2, false, false, false, false, 0, false};  // cleanShared
                    case 3: return {0x09, 2, false, false, false, false, 0, false};  // cleanInvalid
                    default: return {0x0a, 2, false, false, false, false, 0, false}; // makeInvalid
                }
            }
        }
    }

    // 生成一个 CommitTask；返回 hnIdx（<0 表示无槽）
    int make(uint64_t cyc, Valid<CommitTask>& out) {
        const Fam f = randFam();
        const uint64_t addr = randAddr(rng);
        const uint32_t b = uaDirBank(useAddr(addr));
        const uint32_t s = (useAddr(addr) >> 1) & 3;
        const int way = pool.alloc(b, s, rng);
        if (way < 0 || way > 13) {  // commit 槽只用 way0-13
            if (way >= 14) pool.free(hnIdxOf(b, s, way));
            return -1;
        }
        const uint8_t hnIdx = hnIdxOf(b, s, way);
        // state：llcState(DecodeCHI 值) 与 src/oth
        const uint32_t stateRoll = rng() % 100;
        bool srcHit = false, othHit = false;
        uint32_t llcDec;  // DecodeCHI：I=0 SC=1 UC=2 UD=3
        if (stateRoll < 55) {
            llcDec = 0;  // llc miss
        } else if (stateRoll < 70) {
            llcDec = 1 + rng() % 3;  // SC/UC/UD，sfMiss
        } else {
            llcDec = 0;
            const uint32_t h = rng() % 3;
            srcHit = (h == 1 || h == 2);
            othHit = (h == 0 || h == 2);
        }
        CommitTask t;
        t.hnTxnID = hnIdx;
        t.qos = (rng() % 6 == 0) ? 0xF : (rng() & 7);
        t.chi.channel = 0;
        t.chi.nodeId = 0x09;
        t.chi.opcode = f.opcode;
        t.chi.order = f.order;
        t.chi.expCompAck = f.expCompAck;
        t.chi.snpAttr = true;
        t.chi.snoopMe = false;
        t.chi.dataVec = f.fullSize ? 3 : (1u << (rng() & 1));
        t.chi.txnID = rng() & 0xFFF;
        t.chi.memAttr = (f.allocate << 3) | (1u << 2) | (0u << 1) | f.ewa;
        t.chi.size = f.fullSize ? 6 : 5;
        t.chi.fwdNID = rng() & 0x7FF;
        t.chi.fwdTxnID = rng() & 0xFFF;
        t.chi.retToSrc = false;
        t.chi.toLAN = true;
        t.dir.llc.hit = llcDec != 0;
        t.dir.llc.wayOH = static_cast<uint16_t>(1u << (rng() % 16));
        // DecodeCHI → ChiState：I=0 SC=1 UC=3 UD=2
        t.dir.llc.meta = llcDec == 0 ? 0 : (llcDec == 1 ? 1 : (llcDec == 2 ? 3 : 2));
        t.dir.sf.hit = srcHit || othHit;
        t.dir.sf.wayOH = static_cast<uint16_t>(1u << (rng() % 16));
        t.dir.sf.meta = t.dir.sf.hit ? 1 : 0;
        t.alr = {};
        t.ds.set(addr, ohToUInt(t.dir.llc.wayOH));
        // chiInst / stateInst / decList
        const uint32_t chiInst = packChi(1, 0, 1, 1, f.opcode, f.expCompAck, f.allocate,
                                         f.ewa, f.order, f.fullSize);
        const uint32_t ci = dc::decChi(chiInst);
        const uint32_t si = dc::decState(ci, packSi(1, srcHit, othHit, llcDec));
        t.decList = {static_cast<uint8_t>(ci), static_cast<uint8_t>(si), 0, 0};
        t.task = dc::getTaskCode(ci, si);
        t.cmt = dc::tcIsValid(t.task) ? 0 : dc::getCommitCode(ci, si, 0, 0);

        // 槽位登记
        auto& sl = slots[hnIdx];
        sl.active = true;
        sl.addr = addr;
        sl.expCompAck = f.expCompAck;
        sl.wrDataOp = 0;
        sl.wrDataAt = -1;
        sl.compAckAt = f.expCompAck ? cyc + 30 + rng() % 40 : -1;
        if (f.kind == 1) {  // write 系：RN 写数据
            const uint32_t r = rng() % 100;
            sl.wrDataOp = r < 50 ? kNCBWrDataCompAck
                          : r < 80 ? kNonCopyBackWriteData
                                   : kCopyBackWriteData;
            sl.wrDataAt = cyc + 2 + rng() % 25;
        }
        addrTab[hnIdx] = addr;
        out = {true, t};
        return hnIdx;
    }

    static constexpr uint32_t packChi(bool valid, uint32_t channel, bool fromLAN, bool toLAN,
                                      uint32_t opcode, bool eca, bool alloc, bool ewa,
                                      uint32_t order, bool full) {
        return (valid << 17) | (channel << 15) | (fromLAN << 14) | (toLAN << 13) |
               (opcode << 6) | (eca << 5) | (alloc << 4) | (ewa << 3) | (order << 1) | full;
    }
    static constexpr uint32_t packSi(bool valid, bool src, bool oth, uint32_t llc) {
        return (valid << 4) | (src << 3) | (oth << 2) | llc;
    }
};

}  // namespace
namespace {

void CHECK_ALL(VBackend& ref, Backend& dut, cosim::Stats& st, uint32_t seed, uint64_t c,
               cosim::Replay& rp);
void respond(VBackend& ref, Backend& dut, uint64_t c, std::mt19937& rng,
             std::deque<EvRsp>& evRsp, std::deque<EvDat>& evDat,
             std::deque<EvDirW>& evDirW, std::deque<EvDataR>& evDataR,
             std::deque<EvPosR>& evPosR, WayPool& pool, std::array<Slot, 128>& slots,
             std::array<uint64_t, 128>& addrTab);

uint64_t cosimBackend(uint32_t seed, uint64_t cycles) {
    VBackend ref;
    Backend dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;

    cosim::resetRef(ref, [&] {
        ref.io_txReq_ready = 0;
        ref.io_txSnp_ready = 0;
        ref.io_txRsp_ready = 0;
        setRxRsp(ref, {false, {}});
        setRxDat(ref, {false, {}});
        ref.io_writeDir_ready = 0;
        setRespDir(ref, false, {false, {}});
        setRespDir(ref, true, {false, {}});
        for (int b = 0; b < 2; ++b)
            for (int s = 0; s < 4; ++s) {
                if (b == 0) {
                    ref.io_posRespVec2_0_0_valid = 0;
                    ref.io_posRespVec2_0_0_bits = 0;
                }
            }
        ref.io_cmtTaskVec_0_valid = 0;
        ref.io_cmtTaskVec_1_valid = 0;
        setCmtTask(ref, 0, {false, {}});
        setCmtTask(ref, 1, {false, {}});
        setFastResp(ref, {false, {}});
        ref.io_reqDB_ready = 0;
        ref.io_dataTask_ready = 0;
        ref.io_dataResp_valid = 0;
        ref.io_dataResp_bits_hnTxnID = 0;
        ref.io_cleanDB_ready = 0;
        ref.io_getAddrVec_0_result_addr = 0;
        ref.io_getAddrVec_1_result_addr = 0;
        ref.io_getAddrVec_2_result_addr = 0;
        ref.io_config_ci = 0;
        ref.io_config_closeLLC = 0;
        ref.io_config_bankId = 0;
        for (int s = 0; s < 4; ++s) {
            ref.io_posRespVec2_0_0_valid = 0;
        }
    });
    dut.cfg_ci.set(0);
    dut.cfg_bank_id.set(0);
    dut.tx_req_rdy.set(false);
    dut.tx_snp_rdy.set(false);
    dut.tx_rsp_rdy.set(false);
    dut.rx_rsp.set({false, {}});
    dut.rx_dat.set({false, {}});
    dut.write_dir_rdy.set(false);
    dut.resp_dir_llc.set({false, {}});
    dut.resp_dir_sf.set({false, {}});
    dut.pos_resp_vec.set({});
    dut.cmt_task_0.set({false, {}});
    dut.cmt_task_1.set({false, {}});
    dut.fast_resp.set({false, {}});
    dut.req_db_rdy.set(false);
    dut.data_task_rdy.set(false);
    dut.data_resp.set({false, 0});
    dut.clean_db_rdy.set(false);
    dut.get_addr_0_result.set(0);
    dut.get_addr_1_result.set(0);
    dut.get_addr_2_result.set(0);
    dut.clk.set(0);
    dut.eval();

    WayPool pool;
    std::array<Slot, 128> slots{};
    std::array<uint64_t, 128> addrTab{};
    Gen gen(rng, pool, slots, addrTab);
    std::deque<EvRsp> evRsp;
    std::deque<EvDat> evDat;
    std::deque<EvDirW> evDirW;
    std::deque<EvDataR> evDataR;
    std::deque<EvPosR> evPosR;
    Valid<CommitTask> pendCmt0{false, {}}, pendCmt1{false, {}};

    auto posRespSet = [&](VBackend& r, uint8_t b, uint8_t s, bool valid, uint8_t way) {
        // posRespVec2 端口按 (b,s) 命名
        switch (b * 4 + s) {
            case 0: r.io_posRespVec2_0_0_valid = valid; r.io_posRespVec2_0_0_bits = way; break;
            case 1: r.io_posRespVec2_0_1_valid = valid; r.io_posRespVec2_0_1_bits = way; break;
            case 2: r.io_posRespVec2_0_2_valid = valid; r.io_posRespVec2_0_2_bits = way; break;
            case 3: r.io_posRespVec2_0_3_valid = valid; r.io_posRespVec2_0_3_bits = way; break;
            case 4: r.io_posRespVec2_1_0_valid = valid; r.io_posRespVec2_1_0_bits = way; break;
            case 5: r.io_posRespVec2_1_1_valid = valid; r.io_posRespVec2_1_1_bits = way; break;
            case 6: r.io_posRespVec2_1_2_valid = valid; r.io_posRespVec2_1_2_bits = way; break;
            default: r.io_posRespVec2_1_3_valid = valid; r.io_posRespVec2_1_3_bits = way; break;
        }
    };

    for (uint64_t c = 0; c < cycles; ++c) {
        const uint32_t pct = cosim::densityAt(c, cycles);

        // ---- 新请求计划 ----
        if (!pendCmt0.valid && cosim::roll(rng, pct / 6)) {
            Valid<CommitTask> v;
            if (gen.make(c, v) >= 0) {
                if (uaDirBank(useAddr(v.bits.ds.idx == 0 ? 0 : 0)) == 0) {}
                pendCmt0 = v;
            }
        }
        if (!pendCmt1.valid && cosim::roll(rng, pct / 6)) {
            Valid<CommitTask> v;
            if (gen.make(c, v) >= 0) pendCmt1 = v;
        }

        // ---- 事件出队 ----
        Valid<RespFlit> rspV{false, {}};
        Valid<DataFlit> datV{false, {}};
        Valid<DirResp> dirLV{false, {}}, dirSV{false, {}};
        Valid<uint8_t> dataRV{false, 0};
        if (!evRsp.empty() && evRsp.front().at <= c) {
            rspV = {true, evRsp.front().f};
            evRsp.pop_front();
        }
        if (!evDat.empty() && evDat.front().at <= c) {
            datV = {true, evDat.front().f};
            evDat.pop_front();
        }
        if (!evDirW.empty() && evDirW.front().at <= c) {
            const auto& e = evDirW.front();
            if (e.sf) dirSV = {true, e.r};
            else dirLV = {true, e.r};
            evDirW.pop_front();
        }
        if (!evDataR.empty() && evDataR.front().at <= c) {
            dataRV = {true, evDataR.front().txn};
            evDataR.pop_front();
        }
        bool posRValid[2][4] = {};
        uint8_t posRWay[2][4] = {};
        while (!evPosR.empty() && evPosR.front().at <= c) {
            const auto e = evPosR.front();
            posRValid[e.b][e.s] = true;
            posRWay[e.b][e.s] = e.way;
            evPosR.pop_front();
        }

        // ---- 驱动两侧 ----
        const bool txReqRdy = cosim::roll(rng, 70), txSnpRdy = cosim::roll(rng, 70),
                   txRspRdy = cosim::roll(rng, 70), wdirRdy = cosim::roll(rng, 70),
                   reqDbRdy = cosim::roll(rng, 70), dtaskRdy = cosim::roll(rng, 70),
                   cdbRdy = cosim::roll(rng, 70);
        dut.tx_req_rdy.set(txReqRdy);
        ref.io_txReq_ready = txReqRdy;
        dut.tx_snp_rdy.set(txSnpRdy);
        ref.io_txSnp_ready = txSnpRdy;
        dut.tx_rsp_rdy.set(txRspRdy);
        ref.io_txRsp_ready = txRspRdy;
        dut.write_dir_rdy.set(wdirRdy);
        ref.io_writeDir_ready = wdirRdy;
        dut.req_db_rdy.set(reqDbRdy);
        ref.io_reqDB_ready = reqDbRdy;
        dut.data_task_rdy.set(dtaskRdy);
        ref.io_dataTask_ready = dtaskRdy;
        dut.clean_db_rdy.set(cdbRdy);
        ref.io_cleanDB_ready = cdbRdy;

        // cmtTaskVec 按 dirBank 分发
        Valid<CommitTask> cmt0{false, {}}, cmt1{false, {}};
        if (pendCmt0.valid) {
            const uint32_t b = uaDirBank(useAddr(slots[pendCmt0.bits.hnTxnID].addr));
            if (b == 0) {
                cmt0 = pendCmt0;
                pendCmt0 = {false, {}};
            }
        }
        if (pendCmt1.valid) {
            const uint32_t b = uaDirBank(useAddr(slots[pendCmt1.bits.hnTxnID].addr));
            if (b == 1) {
                cmt1 = pendCmt1;
                pendCmt1 = {false, {}};
            }
        }
        // pendCmt 的 bank 不匹配的留到另一边空时再发（简单起见：各自只收匹配 bank 的）
        dut.cmt_task_0.set(cmt0);
        dut.cmt_task_1.set(cmt1);
        setCmtTask(ref, 0, cmt0);
        setCmtTask(ref, 1, cmt1);

        dut.rx_rsp.set(rspV);
        setRxRsp(ref, rspV);
        dut.rx_dat.set(datV);
        setRxDat(ref, datV);
        dut.resp_dir_llc.set(dirLV);
        setRespDir(ref, false, dirLV);
        dut.resp_dir_sf.set(dirSV);
        setRespDir(ref, true, dirSV);
        dut.data_resp.set(dataRV);
        ref.io_dataResp_valid = dataRV.valid;
        ref.io_dataResp_bits_hnTxnID = dataRV.bits;
        typename Backend::PosRespArr pr{};
        for (int b = 0; b < 2; ++b)
            for (int s = 0; s < 4; ++s) {
                pr[b][s] = {posRValid[b][s], posRWay[b][s]};
                posRespSet(ref, b, s, posRValid[b][s], posRWay[b][s]);
            }
        dut.pos_resp_vec.set(pr);

        // fastResp：随机直通 flit
        Valid<RespFlit> frV{false, {}};
        if (cosim::roll(rng, 15)) {
            RespFlit f{};
            f.txn_id = rng() & 0xFFF;
            f.dbid = rng() & 0xFFF;
            f.src_id = rng() & 0x7FF;
            f.tgt_id = rng() & 0x7FF;
            f.opcode = rng() & 0x1F;
            f.resp = rng() & 7;
            f.qos = rng() & 0xF;
            frV = {true, f};
        }
        dut.fast_resp.set(frV);
        setFastResp(ref, frV);

        // getAddrVec 地址表
        dut.get_addr_0_result.set(addrTab[dut.get_addr_0_hnidx.get()]);
        dut.get_addr_1_result.set(addrTab[dut.get_addr_1_hnidx.get()]);
        dut.get_addr_2_result.set(addrTab[dut.get_addr_2_hnidx.get()]);
        ref.io_getAddrVec_0_result_addr = addrTab[dut.get_addr_0_hnidx.get()];
        ref.io_getAddrVec_1_result_addr = addrTab[dut.get_addr_1_hnidx.get()];
        ref.io_getAddrVec_2_result_addr = addrTab[dut.get_addr_2_hnidx.get()];

        rp.push("cyc=" + std::to_string(c));

        cosim::phaseLow(ref, dut);

        // ---- 比对（核心通道；字段展开见 CHECK_* 宏） ----
        CHECK_ALL(ref, dut, st, seed, c, rp);

        // ---- fire 采样 + 环境响应调度 ----
        respond(ref, dut, c, rng, evRsp, evDat, evDirW, evDataR, evPosR, pool, slots,
                addrTab);

        cosim::phaseHigh(ref, dut);

        // clean_pos 释放槽位（dut 侧 clean_pos fire）
        if (dut.clean_pos.get().valid && dut.clean_db_rdy.get()) {
            const uint8_t hnIdx = dut.clean_pos.get().bits.hnIdx;
            pool.free(hnIdx);
            slots[hnIdx].active = false;
        }
    }

    std::cout << (st.mismatches == 0 ? "PASS" : "FAIL") << " backend Backend seed=" << seed
              << " cycles=" << cycles << " checks=" << st.checks
              << " mismatches=" << st.mismatches << "\n";
    return st.mismatches;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {1u, 2u, 3u}) bad += cosimBackend(seed, 150000);
    return bad == 0 ? 0 : 1;
}
namespace {

void checkHnIdx(cosim::Stats& st, uint32_t seed, uint64_t c, const char* p, uint32_t rdb,
                uint32_t rps, uint32_t rpw, uint8_t d, cosim::Replay& rp) {
    cosim::check(st, "be", "Backend", seed, c, (std::string(p) + "_dirBank").c_str(), rdb,
                 hnIdxDirBank(d), rp);
    cosim::check(st, "be", "Backend", seed, c, (std::string(p) + "_posSet").c_str(), rps,
                 hnIdxPosSet(d), rp);
    cosim::check(st, "be", "Backend", seed, c, (std::string(p) + "_posWay").c_str(), rpw,
                 hnIdxPosWay(d), rp);
}

void CHECK_ALL(VBackend& ref, Backend& dut, cosim::Stats& st, uint32_t seed, uint64_t c,
               cosim::Replay& rp) {
    cosim::check(st, "be", "Backend", seed, c, "rx_rsp_rdy", ref.io_rxRsp_ready,
                 dut.rx_rsp_rdy.get(), rp);
    // tx_req
    cosim::check(st, "be", "Backend", seed, c, "txreq_valid", ref.io_txReq_valid,
                 dut.tx_req.get().valid, rp);
    if (ref.io_txReq_valid && dut.tx_req.get().valid) {
        const auto& b = dut.tx_req.get().bits;
        cosim::check(st, "be", "Backend", seed, c, "txreq_eca", ref.io_txReq_bits_ExpCompAck,
                     b.exp_comp_ack, rp);
        cosim::check(st, "be", "Backend", seed, c, "txreq_excl", ref.io_txReq_bits_Excl,
                     b.excl, rp);
        cosim::check(st, "be", "Backend", seed, c, "txreq_snpattr",
                     ref.io_txReq_bits_SnpAttr, b.snp_attr, rp);
        cosim::check(st, "be", "Backend", seed, c, "txreq_memattr",
                     ref.io_txReq_bits_MemAttr, b.mem_attr, rp);
        cosim::check(st, "be", "Backend", seed, c, "txreq_order", ref.io_txReq_bits_Order,
                     b.order, rp);
        cosim::check(st, "be", "Backend", seed, c, "txreq_addr", ref.io_txReq_bits_Addr,
                     b.addr, rp);
        cosim::check(st, "be", "Backend", seed, c, "txreq_size", ref.io_txReq_bits_Size,
                     b.size, rp);
        cosim::check(st, "be", "Backend", seed, c, "txreq_opcode",
                     ref.io_txReq_bits_Opcode, b.opcode, rp);
        cosim::check(st, "be", "Backend", seed, c, "txreq_rtxn",
                     ref.io_txReq_bits_ReturnTxnID, b.return_txn_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "txreq_rnid",
                     ref.io_txReq_bits_ReturnNID, b.return_nid, rp);
        cosim::check(st, "be", "Backend", seed, c, "txreq_txnid", ref.io_txReq_bits_TxnID,
                     b.txn_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "txreq_srcid", ref.io_txReq_bits_SrcID,
                     b.src_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "txreq_tgtid", ref.io_txReq_bits_TgtID,
                     b.tgt_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "txreq_qos", ref.io_txReq_bits_QoS,
                     b.qos, rp);
    }
    // tx_snp
    cosim::check(st, "be", "Backend", seed, c, "txsnp_valid", ref.io_txSnp_valid,
                 dut.tx_snp.get().valid, rp);
    if (ref.io_txSnp_valid && dut.tx_snp.get().valid) {
        const auto& b = dut.tx_snp.get().bits;
        cosim::check(st, "be", "Backend", seed, c, "txsnp_r2s", ref.io_txSnp_bits_RetToSrc,
                     b.ret_to_src, rp);
        cosim::check(st, "be", "Backend", seed, c, "txsnp_dngsd",
                     ref.io_txSnp_bits_DoNotGoToSD, b.do_not_go_to_sd, rp);
        cosim::check(st, "be", "Backend", seed, c, "txsnp_addr", ref.io_txSnp_bits_Addr,
                     b.addr, rp);
        cosim::check(st, "be", "Backend", seed, c, "txsnp_opcode",
                     ref.io_txSnp_bits_Opcode, b.opcode, rp);
        cosim::check(st, "be", "Backend", seed, c, "txsnp_fwdtxn",
                     ref.io_txSnp_bits_FwdTxnID, b.fwd_txn_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "txsnp_fwdnid",
                     ref.io_txSnp_bits_FwdNID, b.fwd_nid, rp);
        cosim::check(st, "be", "Backend", seed, c, "txsnp_txnid", ref.io_txSnp_bits_TxnID,
                     b.txn_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "txsnp_srcid", ref.io_txSnp_bits_SrcID,
                     b.src_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "txsnp_tgtid", ref.io_txSnp_bits_TgtID,
                     b.tgt_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "txsnp_qos", ref.io_txSnp_bits_QoS,
                     b.qos, rp);
    }
    // tx_rsp
    cosim::check(st, "be", "Backend", seed, c, "txrsp_valid", ref.io_txRsp_valid,
                 dut.tx_rsp.get().valid, rp);
    if (ref.io_txRsp_valid && dut.tx_rsp.get().valid) {
        const auto& b = dut.tx_rsp.get().bits;
        cosim::check(st, "be", "Backend", seed, c, "txrsp_dbid", ref.io_txRsp_bits_DBID,
                     b.dbid, rp);
        cosim::check(st, "be", "Backend", seed, c, "txrsp_cbusy", ref.io_txRsp_bits_CBusy,
                     b.c_busy, rp);
        cosim::check(st, "be", "Backend", seed, c, "txrsp_fwdst",
                     ref.io_txRsp_bits_FwdState, b.fwd_state, rp);
        cosim::check(st, "be", "Backend", seed, c, "txrsp_resp", ref.io_txRsp_bits_Resp,
                     b.resp, rp);
        cosim::check(st, "be", "Backend", seed, c, "txrsp_resperr",
                     ref.io_txRsp_bits_RespErr, b.resp_err, rp);
        cosim::check(st, "be", "Backend", seed, c, "txrsp_opcode",
                     ref.io_txRsp_bits_Opcode, b.opcode, rp);
        cosim::check(st, "be", "Backend", seed, c, "txrsp_txnid", ref.io_txRsp_bits_TxnID,
                     b.txn_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "txrsp_srcid", ref.io_txRsp_bits_SrcID,
                     b.src_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "txrsp_tgtid", ref.io_txRsp_bits_TgtID,
                     b.tgt_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "txrsp_qos", ref.io_txRsp_bits_QoS,
                     b.qos, rp);
    }
    // write_dir
    cosim::check(st, "be", "Backend", seed, c, "wdir_valid", ref.io_writeDir_valid,
                 dut.write_dir.get().valid, rp);
    if (ref.io_writeDir_valid && dut.write_dir.get().valid) {
        const auto& b = dut.write_dir.get().bits;
        cosim::check(st, "be", "Backend", seed, c, "wdir_llc_v", ref.io_writeDir_bits_llc_valid,
                     b.llcValid, rp);
        if (ref.io_writeDir_bits_llc_valid && b.llcValid) {
            cosim::check(st, "be", "Backend", seed, c, "wdir_llc_addr",
                         ref.io_writeDir_bits_llc_bits_addr, b.llc.addr, rp);
            cosim::check(st, "be", "Backend", seed, c, "wdir_llc_way",
                         ref.io_writeDir_bits_llc_bits_wayOH, b.llc.wayOH, rp);
            cosim::check(st, "be", "Backend", seed, c, "wdir_llc_hit",
                         ref.io_writeDir_bits_llc_bits_hit, b.llc.hit, rp);
            cosim::check(st, "be", "Backend", seed, c, "wdir_llc_meta",
                         ref.io_writeDir_bits_llc_bits_metaVec_0_state, b.llc.meta, rp);
            checkHnIdx(st, seed, c, "wdir_llc_hnIdx",
                       ref.io_writeDir_bits_llc_bits_hnIdx_dirBank,
                       ref.io_writeDir_bits_llc_bits_hnIdx_pos_set,
                       ref.io_writeDir_bits_llc_bits_hnIdx_pos_way, b.llc.hnIdx, rp);
            cosim::check(st, "be", "Backend", seed, c, "wdir_llc_da",
                         ref.io_writeDir_bits_llc_bits_directAlloc, b.llc.directAlloc, rp);
        }
        cosim::check(st, "be", "Backend", seed, c, "wdir_sf_v", ref.io_writeDir_bits_sf_valid,
                     b.sfValid, rp);
        if (ref.io_writeDir_bits_sf_valid && b.sfValid) {
            cosim::check(st, "be", "Backend", seed, c, "wdir_sf_addr",
                         ref.io_writeDir_bits_sf_bits_addr, b.sf.addr, rp);
            cosim::check(st, "be", "Backend", seed, c, "wdir_sf_way",
                         ref.io_writeDir_bits_sf_bits_wayOH, b.sf.wayOH, rp);
            cosim::check(st, "be", "Backend", seed, c, "wdir_sf_hit",
                         ref.io_writeDir_bits_sf_bits_hit, b.sf.hit, rp);
            cosim::check(st, "be", "Backend", seed, c, "wdir_sf_meta",
                         ref.io_writeDir_bits_sf_bits_metaVec_0_state, b.sf.meta, rp);
            checkHnIdx(st, seed, c, "wdir_sf_hnIdx",
                       ref.io_writeDir_bits_sf_bits_hnIdx_dirBank,
                       ref.io_writeDir_bits_sf_bits_hnIdx_pos_set,
                       ref.io_writeDir_bits_sf_bits_hnIdx_pos_way, b.sf.hnIdx, rp);
            cosim::check(st, "be", "Backend", seed, c, "wdir_sf_da",
                         ref.io_writeDir_bits_sf_bits_directAlloc, b.sf.directAlloc, rp);
        }
    }
    // unlock
    cosim::check(st, "be", "Backend", seed, c, "unlock_v", ref.io_unlock_valid,
                 dut.unlock.get().valid, rp);
    if (ref.io_unlock_valid && dut.unlock.get().valid)
        checkHnIdx(st, seed, c, "unlock", ref.io_unlock_bits_hnIdx_dirBank,
                   ref.io_unlock_bits_hnIdx_pos_set, ref.io_unlock_bits_hnIdx_pos_way,
                   dut.unlock.get().bits, rp);
    // clean_pos
    cosim::check(st, "be", "Backend", seed, c, "cpos_v", ref.io_cleanPoS_valid,
                 dut.clean_pos.get().valid, rp);
    if (ref.io_cleanPoS_valid && dut.clean_pos.get().valid) {
        checkHnIdx(st, seed, c, "cpos", ref.io_cleanPoS_bits_hnIdx_dirBank,
                   ref.io_cleanPoS_bits_hnIdx_pos_set, ref.io_cleanPoS_bits_hnIdx_pos_way,
                   dut.clean_pos.get().bits.hnIdx, rp);
        cosim::check(st, "be", "Backend", seed, c, "cpos_ch", ref.io_cleanPoS_bits_channel,
                     dut.clean_pos.get().bits.channel, rp);
        cosim::check(st, "be", "Backend", seed, c, "cpos_qos", ref.io_cleanPoS_bits_qos,
                     dut.clean_pos.get().bits.qos, rp);
    }
    // upd_hn_txn_id
    cosim::check(st, "be", "Backend", seed, c, "updid_v", ref.io_updHnTxnID_valid,
                 dut.upd_hn_txn_id.get().valid, rp);
    if (ref.io_updHnTxnID_valid && dut.upd_hn_txn_id.get().valid) {
        cosim::check(st, "be", "Backend", seed, c, "updid_before",
                     ref.io_updHnTxnID_bits_before, dut.upd_hn_txn_id.get().bits.before, rp);
        cosim::check(st, "be", "Backend", seed, c, "updid_next",
                     ref.io_updHnTxnID_bits_next, dut.upd_hn_txn_id.get().bits.next, rp);
    }
    // req_db
    cosim::check(st, "be", "Backend", seed, c, "reqdb_v", ref.io_reqDB_valid,
                 dut.req_db.get().valid, rp);
    if (ref.io_reqDB_valid && dut.req_db.get().valid) {
        cosim::check(st, "be", "Backend", seed, c, "reqdb_txn", ref.io_reqDB_bits_hnTxnID,
                     dut.req_db.get().bits.hnTxnID, rp);
        cosim::check(st, "be", "Backend", seed, c, "reqdb_dv0", ref.io_reqDB_bits_dataVec_0,
                     dut.req_db.get().bits.dataVec & 1, rp);
        cosim::check(st, "be", "Backend", seed, c, "reqdb_dv1", ref.io_reqDB_bits_dataVec_1,
                     (dut.req_db.get().bits.dataVec >> 1) & 1, rp);
    }
    // data_task
    cosim::check(st, "be", "Backend", seed, c, "dtask_v", ref.io_dataTask_valid,
                 dut.data_task.get().valid, rp);
    if (ref.io_dataTask_valid && dut.data_task.get().valid) {
        const auto& b = dut.data_task.get().bits;
        cosim::check(st, "be", "Backend", seed, c, "dtask_repl",
                     ref.io_dataTask_bits_dataOp_repl, b.dataOp.repl, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_read",
                     ref.io_dataTask_bits_dataOp_read, b.dataOp.read, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_send",
                     ref.io_dataTask_bits_dataOp_send, b.dataOp.send, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_save",
                     ref.io_dataTask_bits_dataOp_save, b.dataOp.save, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_merge",
                     ref.io_dataTask_bits_dataOp_merge, b.dataOp.merge, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_bank",
                     ref.io_dataTask_bits_ds_bank, b.ds.bank, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_idx", ref.io_dataTask_bits_ds_idx,
                     b.ds.idx, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_dv0",
                     ref.io_dataTask_bits_dataVec_0, b.dataVec & 1, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_dv1",
                     ref.io_dataTask_bits_dataVec_1, (b.dataVec >> 1) & 1, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_txn",
                     ref.io_dataTask_bits_hnTxnID, b.hnTxnID, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_qos", ref.io_dataTask_bits_qos,
                     b.qos, rp);
        const auto& f = b.txDat;
        for (int w = 0; w < 4; ++w) {
            const uint64_t rv =
                (static_cast<uint64_t>(ref.io_dataTask_bits_txDat_Data[2 * w + 1]) << 32) |
                ref.io_dataTask_bits_txDat_Data[2 * w];
            cosim::check(st, "be", "Backend", seed, c,
                         (std::string("dtask_data") + std::to_string(w)).c_str(), rv,
                         f.data[w], rp);
        }
        cosim::check(st, "be", "Backend", seed, c, "dtask_be", ref.io_dataTask_bits_txDat_BE,
                     f.be, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_did",
                     ref.io_dataTask_bits_txDat_DataID, f.data_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_dbid",
                     ref.io_dataTask_bits_txDat_DBID, f.dbid, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_cbusy",
                     ref.io_dataTask_bits_txDat_CBusy, f.c_busy, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_ds",
                     ref.io_dataTask_bits_txDat_DataSource, f.data_source, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_resp",
                     ref.io_dataTask_bits_txDat_Resp, f.resp, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_resperr",
                     ref.io_dataTask_bits_txDat_RespErr, f.resp_err, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_opcode",
                     ref.io_dataTask_bits_txDat_Opcode, f.opcode, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_homenid",
                     ref.io_dataTask_bits_txDat_HomeNID, f.home_nid, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_txnid",
                     ref.io_dataTask_bits_txDat_TxnID, f.txn_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_srcid",
                     ref.io_dataTask_bits_txDat_SrcID, f.src_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_tgtid",
                     ref.io_dataTask_bits_txDat_TgtID, f.tgt_id, rp);
        cosim::check(st, "be", "Backend", seed, c, "dtask_fqos",
                     ref.io_dataTask_bits_txDat_QoS, f.qos, rp);
    }
    // clean_db
    cosim::check(st, "be", "Backend", seed, c, "cdb_v", ref.io_cleanDB_valid,
                 dut.clean_db.get().valid, rp);
    if (ref.io_cleanDB_valid && dut.clean_db.get().valid) {
        cosim::check(st, "be", "Backend", seed, c, "cdb_txn", ref.io_cleanDB_bits_hnTxnID,
                     dut.clean_db.get().bits.hnTxnID, rp);
        cosim::check(st, "be", "Backend", seed, c, "cdb_dv0", ref.io_cleanDB_bits_dataVec_0,
                     dut.clean_db.get().bits.dataVec & 1, rp);
        cosim::check(st, "be", "Backend", seed, c, "cdb_dv1", ref.io_cleanDB_bits_dataVec_1,
                     (dut.clean_db.get().bits.dataVec >> 1) & 1, rp);
    }
    // req_pos_vec
    for (int b = 0; b < 2; ++b)
        for (int s = 0; s < 4; ++s) {
            const bool rv = b == 0 ? (s == 0 ? ref.io_reqPosVec2_0_0_valid
                                     : s == 1 ? ref.io_reqPosVec2_0_1_valid
                                     : s == 2 ? ref.io_reqPosVec2_0_2_valid
                                              : ref.io_reqPosVec2_0_3_valid)
                                   : (s == 0 ? ref.io_reqPosVec2_1_0_valid
                                     : s == 1 ? ref.io_reqPosVec2_1_1_valid
                                     : s == 2 ? ref.io_reqPosVec2_1_2_valid
                                              : ref.io_reqPosVec2_1_3_valid);
            cosim::check(st, "be", "Backend", seed, c,
                         (std::string("reqpos_v") + std::to_string(b) + std::to_string(s))
                             .c_str(),
                         rv, dut.req_pos_vec.get()[b][s].valid, rp);
            if (rv && dut.req_pos_vec.get()[b][s].valid) {
                const uint32_t rch = b == 0 ? (s == 0 ? ref.io_reqPosVec2_0_0_bits_channel
                                              : s == 1 ? ref.io_reqPosVec2_0_1_bits_channel
                                              : s == 2 ? ref.io_reqPosVec2_0_2_bits_channel
                                                       : ref.io_reqPosVec2_0_3_bits_channel)
                                            : (s == 0 ? ref.io_reqPosVec2_1_0_bits_channel
                                              : s == 1 ? ref.io_reqPosVec2_1_1_bits_channel
                                              : s == 2 ? ref.io_reqPosVec2_1_2_bits_channel
                                                       : ref.io_reqPosVec2_1_3_bits_channel);
                cosim::check(st, "be", "Backend", seed, c,
                             (std::string("reqpos_ch") + std::to_string(b) +
                              std::to_string(s))
                                 .c_str(),
                             rch, dut.req_pos_vec.get()[b][s].bits.channel, rp);
            }
        }
    // upd_pos_tag
    cosim::check(st, "be", "Backend", seed, c, "uptag_v", ref.io_updPosTag_valid,
                 dut.upd_pos_tag.get().valid, rp);
    if (ref.io_updPosTag_valid && dut.upd_pos_tag.get().valid) {
        cosim::check(st, "be", "Backend", seed, c, "uptag_addr",
                     ref.io_updPosTag_bits_addr, dut.upd_pos_tag.get().bits.addr, rp);
        cosim::check(st, "be", "Backend", seed, c, "uptag_addrval",
                     ref.io_updPosTag_bits_addrVal, dut.upd_pos_tag.get().bits.addrVal, rp);
        checkHnIdx(st, seed, c, "uptag", ref.io_updPosTag_bits_hnIdx_dirBank,
                   ref.io_updPosTag_bits_hnIdx_pos_set, ref.io_updPosTag_bits_hnIdx_pos_way,
                   dut.upd_pos_tag.get().bits.hnIdx, rp);
    }
    // fast_resp_rdy
    cosim::check(st, "be", "Backend", seed, c, "fresp_rdy", ref.io_fastResp_ready,
                 dut.fast_resp_rdy.get(), rp);
    // get_addr hnidx
    checkHnIdx(st, seed, c, "gaddr0", ref.io_getAddrVec_0_hnIdx_dirBank,
               ref.io_getAddrVec_0_hnIdx_pos_set, ref.io_getAddrVec_0_hnIdx_pos_way,
               dut.get_addr_0_hnidx.get(), rp);
    checkHnIdx(st, seed, c, "gaddr1", ref.io_getAddrVec_1_hnIdx_dirBank,
               ref.io_getAddrVec_1_hnIdx_pos_set, ref.io_getAddrVec_1_hnIdx_pos_way,
               dut.get_addr_1_hnidx.get(), rp);
    checkHnIdx(st, seed, c, "gaddr2", ref.io_getAddrVec_2_hnIdx_dirBank,
               ref.io_getAddrVec_2_hnIdx_pos_set, ref.io_getAddrVec_2_hnIdx_pos_way,
               dut.get_addr_2_hnidx.get(), rp);
}

// ---------------- 环境响应调度 ----------------
void respond(VBackend& ref, Backend& dut, uint64_t c, std::mt19937& rng,
             std::deque<EvRsp>& evRsp, std::deque<EvDat>& evDat,
             std::deque<EvDirW>& evDirW, std::deque<EvDataR>& evDataR,
             std::deque<EvPosR>& evPosR, WayPool& pool, std::array<Slot, 128>& slots,
             std::array<uint64_t, 128>& addrTab) {
    auto insertRsp = [](std::deque<EvRsp>& q, EvRsp e) {
        auto it = q.begin();
        while (it != q.end() && it->at <= e.at) ++it;
        q.insert(it, e);
    };
    auto insertDat = [](std::deque<EvDat>& q, EvDat e) {
        auto it = q.begin();
        while (it != q.end() && it->at <= e.at) ++it;
        q.insert(it, e);
    };

    // tx_req fire → CHI 响应
    if (dut.tx_req.get().valid && dut.tx_req_rdy.get()) {
        const auto& b = dut.tx_req.get().bits;
        const uint8_t txn = static_cast<uint8_t>(b.txn_id & 0x7F);
        if (b.opcode >= 0x0c) {  // write 系：DBIDResp(/CompDBIDResp) + Comp
            const bool combo = cosim::roll(rng, 40);
            RespFlit f{};
            f.opcode = combo ? kCompDBIDResp : kDBIDResp;
            f.dbid = rng() & 0xFFF;
            f.txn_id = b.txn_id;
            f.src_id = 0x30;  // SN
            f.tgt_id = b.src_id;
            f.qos = b.qos;
            f.resp_err = cosim::roll(rng, 5) ? 2 : 0;
            evRsp.push_back({c + 3 + rng() % 12, f});
            if (!combo) {
                f.opcode = kComp;
                evRsp.push_back({c + 8 + rng() % 15, f});
            }
        } else {  // read 系：CompData
            const bool full = b.size == 6;
            const uint32_t beats = full ? 2 : 1;
            for (uint32_t i = 0; i < beats; ++i) {
                DataFlit f{};
                for (auto& w : f.data) w = (static_cast<uint64_t>(rng()) << 32) | rng();
                f.be = 0xFFFFFFFFull;
                f.data_id = full ? static_cast<uint8_t>(i * 2)
                                 : static_cast<uint8_t>((slots[txn].addr >> 5 & 1) * 2);
                f.dbid = rng() & 0xFFF;
                f.opcode = kCompData;
                f.resp = cosim::roll(rng, 80) ? 2 : 1;  // UC/SC
                f.resp_err = cosim::roll(rng, 5) ? 2 : 0;
                f.home_nid = 0x30;
                f.txn_id = b.txn_id;
                f.src_id = 0x30;
                f.tgt_id = b.src_id;
                f.qos = b.qos;
                insertDat(evDat, {c + 4 + rng() % 20 + i, f});
            }
        }
    }

    // tx_snp fire → snoop 响应
    if (dut.tx_snp.get().valid && dut.tx_snp_rdy.get()) {
        const auto& b = dut.tx_snp.get().bits;
        const SnoopPlan p = planSnoop(rng, b.opcode);
        if (p.useData) {
            for (uint32_t i = 0; i < 2; ++i) {
                DataFlit f{};
                for (auto& w : f.data) w = (static_cast<uint64_t>(rng()) << 32) | rng();
                f.be = 0xFFFFFFFFull;
                f.data_id = static_cast<uint8_t>(i * 2);
                f.opcode = p.fwd ? kSnpRespDataFwded : kSnpRespData;
                f.resp = p.resp;
                f.data_source = p.fwdSt;
                f.txn_id = b.txn_id;
                f.src_id = b.tgt_id;
                f.tgt_id = b.src_id;
                f.qos = b.qos;
                insertDat(evDat, {c + 4 + rng() % 16 + i, f});
            }
        } else {
            RespFlit f{};
            f.opcode = p.fwd ? kSnpRespFwded : kSnpResp;
            f.resp = p.resp;
            f.fwd_state = p.fwdSt;
            f.txn_id = b.txn_id;
            f.src_id = b.tgt_id;
            f.tgt_id = b.src_id;
            f.qos = b.qos;
            insertRsp(evRsp, {c + 4 + rng() % 16, f});
        }
    }

    // data_task fire → dataResp
    if (dut.data_task.get().valid && dut.data_task_rdy.get())
        evDataR.push_back({c + 2 + rng() % 10, dut.data_task.get().bits.hnTxnID});

    // write_dir fire（替换）→ respDir
    if (dut.write_dir.get().valid && dut.write_dir_rdy.get()) {
        const auto& w = dut.write_dir.get().bits;
        const bool replL = w.llcValid && !w.llc.hit && !w.llc.directAlloc;
        const bool replS = w.sfValid && !w.sf.hit && !w.sf.directAlloc;
        if (replL || replS) {
            DirResp r;
            r.addr = randAddr(rng) & ~0x3Full;
            const uint32_t m = rng() % 100;
            if (replL) {
                r.meta = m < 40 ? 2 : (m < 60 ? 3 : (m < 80 ? 1 : 0));  // UD/UC/SC/I
            } else {
                r.meta = m < 50 ? 1 : 0;
            }
            r.wayOH = static_cast<uint16_t>(1u << (rng() % 16));
            r.hit = false;
            r.hnTxnID = replL ? w.llc.hnIdx : w.sf.hnIdx;
            evDirW.push_back({c + 4, replS, r});
        }
    }

    // reqPosVec → posResp
    for (uint8_t b = 0; b < 2; ++b)
        for (uint8_t s = 0; s < 4; ++s)
            if (dut.req_pos_vec.get()[b][s].valid) {
                const int w = pool.alloc(b, s, rng);
                if (w >= 0) evPosR.push_back({c + 1 + rng() % 3, b, s,
                                              static_cast<uint8_t>(w)});
            }

    // upd_pos_tag → 地址表跟写
    if (dut.upd_pos_tag.get().valid) {
        const auto& u = dut.upd_pos_tag.get().bits;
        addrTab[u.hnIdx] = u.addr;
    }

    // compAck / 写数据到期注入（槽位计划表驱动）
    for (uint32_t id = 0; id < 128; ++id) {
        auto& sl = slots[id];
        if (!sl.active) continue;
        if (sl.compAckAt >= 0 && static_cast<uint64_t>(sl.compAckAt) <= c) {
            RespFlit f{};
            f.opcode = kCompAck;
            f.txn_id = id;
            f.src_id = 0x09;
            f.qos = 0;
            insertRsp(evRsp, {c, f});
            sl.compAckAt = -1;
        }
        if (sl.wrDataAt >= 0 && static_cast<uint64_t>(sl.wrDataAt) <= c) {
            const uint8_t dv = 3;
            for (uint32_t i = 0; i < 2; ++i) {
                if (!((dv >> i) & 1)) continue;
                DataFlit f{};
                for (auto& w : f.data) w = (static_cast<uint64_t>(rng()) << 32) | rng();
                f.be = sl.wrDataOp == kCopyBackWriteData ? 0xFFFFFFFFull : rng();
                f.data_id = static_cast<uint8_t>(i * 2);
                f.opcode = sl.wrDataOp;
                f.resp = 0;  // CB 数据 Resp=I（NCBWrData 语义）
                f.txn_id = id;
                f.src_id = 0x09;
                f.qos = 0;
                insertDat(evDat, {c + i, f});
            }
            sl.wrDataAt = -1;
        }
    }
}

}  // namespace
