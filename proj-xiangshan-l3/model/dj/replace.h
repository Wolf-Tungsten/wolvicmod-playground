#pragma once

// ReplaceEntry / ReplaceCM：对齐 backend/ReplaceCM.scala（语义 §5.4）。
//   ReplaceEntry — 十八态 FSM（替换全流程）×64
//   ReplaceCM    — Alloc 池化 + reqPoS 按 (dirBank,posSet) 的 VipArbiter 矩阵 + 输出仲裁

#include <array>
#include <cstdint>

#include "model/dj/backend_types.h"
#include "model/dj/qosrr.h"
#include "prefab/xsarb.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::dj {

using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Valid;

// reqPoS 载荷：HnIndex + HasChiChannel
struct ReplReqPos {
    uint8_t hnIdx = 0;
    uint8_t channel = 0;

    bool operator==(const ReplReqPos&) const = default;
};

namespace replst {
constexpr uint8_t kFree = 0x0, kReqPos = 0x1, kWaitPos = 0x2, kWriDir = 0x3,
                  kWaitDir = 0x4, kUpdateId = 0x5, kRespCmt = 0x6, kReqDB = 0x7,
                  kWrite = 0x8, kSnoop = 0x9, kWaitRWri = 0xa, kWaitRSnp = 0xb,
                  kCopyId = 0xc, kSaveData = 0xd, kWaitResp = 0xe, kCleanPosT = 0xf,
                  kCleanPosR = 0x10, kWaitWriDir = 0x11;
}  // namespace replst

// ---------------- ReplaceCM（64 entry 拍平 + Alloc + reqPoS 矩阵 + 输出仲裁） ----------------
// 拍平建模：ReplaceEntry 不做子模块，64 项状态并为 REG(entries) 一条 update
// 循环；per-entry 输出为数组 wire 直喂仲裁器。语义与原版逐位等价。

class ReplaceCM : public wolvicmod::Module {
public:
    static constexpr uint32_t kEntries = 64;
    using ReqPosArr = std::array<std::array<Valid<ReplReqPos>, 4>, 2>;
    using PosRespArr = std::array<std::array<Valid<uint8_t>, 4>, 2>;

    struct ReplReg {  // 一个表项的全部状态（原 ReplaceEntry::ReplReg）
        DirMsg dir;
        uint8_t hnTxnID = 0;
        uint8_t qos = 0;
        bool wriSF = false, wriLLC = false, directAllocSF = false;
        // HasReplMes
        uint8_t state = replst::kFree;
        DsIdx ds;
        uint8_t replHnTxnID = 0;
        bool replToLan = false;
        bool needSnp = false, alrReplSF = false;

        bool operator==(const ReplReg&) const = default;
    };
    using EntryArr = std::array<ReplReg, kEntries>;
    using BoolArrN = std::array<bool, kEntries>;

    IN(bool, clk);
    IN(uint8_t, cfg_ci);
    IN(Valid<ReplTask>, task);
    OUT(bool, task_rdy);
    OUT(Valid<uint8_t>, resp);
    OUT(Valid<CMTask>, cm_task_snp);
    IN(bool, cm_task_snp_rdy);
    OUT(Valid<CMTask>, cm_task_wri);
    IN(bool, cm_task_wri_rdy);
    IN(Valid<CMResp>, cm_resp);
    OUT(ReqPosArr, req_pos_vec);
    IN(PosRespArr, pos_resp_vec);
    OUT(Valid<UpdPosTag>, upd_pos_tag);
    OUT(Valid<PosClean>, clean_pos);
    IN(bool, clean_pos_rdy);
    OUT(Valid<DirWrBoth>, write_dir);
    IN(bool, write_dir_rdy);
    IN(Valid<uint8_t>, write_dir_done);
    IN(Valid<DirResp>, resp_dir_llc);
    IN(Valid<DirResp>, resp_dir_sf);
    OUT(Valid<ReqDB>, req_db);
    IN(bool, req_db_rdy);
    OUT(Valid<UpdHnTxnID>, upd_hn_txn_id);
    OUT(Valid<DataTask>, data_task);
    IN(bool, data_task_rdy);
    IN(Valid<uint8_t>, data_resp);

    REG(EntryArr, entries);
    using AllocT = zj::prefab::Alloc<ReplTask, kEntries>;
    MOD(AllocT, alloc_arb);
    // reqPoS 矩阵：每 (dirBank × posSet) 一个 VipArbiter(64)
    using ReqPosArbT = zj::prefab::VipArb<ReplReqPos, kEntries>;
    MOD_ARRAY(ReqPosArbT, 8, req_pos_arbs);  // [bank*4+set]
    // 输出仲裁
    using RespArbT = zj::prefab::VipArb<uint8_t, kEntries>;         // fastRRArb.validOut
    using UpdIdArbT = zj::prefab::VipArb<UpdHnTxnID, kEntries>;     // fastRRArb.validOut
    using UpdTagArbT = wolvicmod::prefab::FixedArb<UpdPosTag, kEntries>;  // fastArb
    using CleanArbT = QosRRArb<PosClean, kEntries>;
    using DataTaskArbT = QosRRArb<DataTask, kEntries>;
    using WdirArbT = zj::prefab::VipArb<DirWrBoth, kEntries>;       // fastRRArb（带 rdy）
    using CmTaskArbT = QosRRArb<CMTask, kEntries>;
    using ReqDbArbT = QosRRArb<ReqDBQos, kEntries>;
    MOD(RespArbT, resp_arb);
    MOD(UpdIdArbT, upd_id_arb);
    MOD(UpdTagArbT, upd_tag_arb);
    MOD(CleanArbT, clean_arb);
    MOD(DataTaskArbT, data_task_arb);
    MOD(WdirArbT, wdir_arb);
    MOD(CmTaskArbT, snp_arb);
    MOD(CmTaskArbT, wri_arb);
    MOD(ReqDbArbT, req_db_arb);

    // 汇集线
    using RespInArr = std::array<Valid<uint8_t>, kEntries>;
    using UpdIdInArr = std::array<Valid<UpdHnTxnID>, kEntries>;
    using UpdTagInArr = std::array<Valid<UpdPosTag>, kEntries>;
    using CleanInArr = std::array<Valid<PosClean>, kEntries>;
    using DataTaskInArr = std::array<Valid<DataTask>, kEntries>;
    using WdirInArr = std::array<Valid<DirWrBoth>, kEntries>;
    using CmTaskInArr = std::array<Valid<CMTask>, kEntries>;
    using ReqDbInArr = std::array<Valid<ReqDBQos>, kEntries>;
    using ReqPosInArr = std::array<Valid<ReplReqPos>, kEntries>;
    WIRE(RespInArr, w_resp_in);
    WIRE(UpdIdInArr, w_upd_id_in);
    WIRE(UpdTagInArr, w_upd_tag_in);
    WIRE(CleanInArr, w_clean_in);
    WIRE(DataTaskInArr, w_data_task_in);
    WIRE(WdirInArr, w_wdir_in);
    WIRE(CmTaskInArr, w_snp_in);
    WIRE(CmTaskInArr, w_wri_in);
    WIRE(ReqDbInArr, w_req_db_in);
    using RdyArrN = std::array<bool, kEntries>;
    WIRE(RdyArrN, w_alloc_rdy_all);
    WIRE(RdyArrN, w_req_pos_rdy_all);  // 8 个 reqPos 仲裁器按 (bank,set) 选择后的 per-entry rdy
    using TxnIdArr = std::array<uint8_t, kEntries>;

    // 合并线（perf-breakdown §20：行为语义相同、读集相同/高度重叠的离散小信号
    // 并为 struct，省一条 assign 的派发+读集打包成本）：
    // dir 响应命中——原 w_sf_resp_hit / w_llc_resp_hit，同做 per-entry 命中检测、
    // 读集同为 entries+一路 dirResp，且同为 upd_pos_tag 输出与次态所共用。
    struct DirHits {
        RdyArrN sf{};
        RdyArrN llc{};

        bool operator==(const DirHits&) const = default;
    };
    WIRE(DirHits, w_dir_hits);
    // 整条 update 的静止门（perf-breakdown §21）：无非空闲项且无 alloc 时
    // compute/commit 全跳过。
    WIRE(bool, w_any);
    // reqPoS 矩阵输入——原 w_hn_txn_ids / w_req_pos_in，同为 reads(entries) 的
    // per-entry 提取，且共同只喂 reqPoS 矩阵与 rdy 选择这一条组合链。
    struct ReqPosFeed {
        TxnIdArr ids{};
        ReqPosInArr in{};

        bool operator==(const ReqPosFeed&) const = default;
    };
    WIRE(ReqPosFeed, w_req_pos_feed);

    ReplaceCM();
};

}  // namespace zj::dj
