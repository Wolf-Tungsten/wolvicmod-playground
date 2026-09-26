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

class ReplaceEntry : public wolvicmod::Module {
public:
    struct ReplReg {
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
    using PosRespArr = std::array<std::array<Valid<uint8_t>, 4>, 2>;

    IN(bool, clk);
    IN(uint8_t, cfg_ci);
    IN(Valid<ReplTask>, alloc);
    OUT(bool, alloc_rdy);
    OUT(Valid<uint8_t>, resp);
    IN(bool, resp_rdy);
    OUT(Valid<CMTask>, cm_task_snp);
    IN(bool, cm_task_snp_rdy);
    OUT(Valid<CMTask>, cm_task_wri);
    IN(bool, cm_task_wri_rdy);
    IN(Valid<CMResp>, cm_resp);
    OUT(Valid<ReplReqPos>, req_pos);
    IN(bool, req_pos_rdy);
    IN(PosRespArr, pos_resp);
    OUT(Valid<UpdPosTag>, upd_pos_tag);
    OUT(Valid<PosClean>, clean_pos);
    IN(bool, clean_pos_rdy);
    OUT(Valid<DirWrBoth>, write_dir);
    IN(bool, write_dir_rdy);
    IN(Valid<uint8_t>, write_dir_done);
    IN(Valid<DirResp>, resp_dir_llc);
    IN(Valid<DirResp>, resp_dir_sf);
    OUT(Valid<ReqDBQos>, req_db);
    IN(bool, req_db_rdy);
    OUT(Valid<UpdHnTxnID>, upd_hn_txn_id);
    IN(bool, upd_hn_txn_id_rdy);
    OUT(Valid<DataTask>, data_task);
    IN(bool, data_task_rdy);
    IN(Valid<uint8_t>, data_resp);

    REG(ReplReg, reg);
    WIRE(ReplReg, w_next);
    WIRE(bool, w_set);
    WIRE(bool, w_pos_resp_hit);
    WIRE(bool, w_sf_resp_hit);
    WIRE(bool, w_llc_resp_hit);
    WIRE(bool, w_cm_resp_hit);
    WIRE(bool, w_data_resp_hit);
    WIRE(bool, w_wri_dir_done_hit);

    ReplaceEntry();
};

// ---------------- ReplaceCM（64 entry + Alloc + reqPoS 矩阵 + 输出仲裁） ----------------

class ReplaceCM : public wolvicmod::Module {
public:
    static constexpr uint32_t kEntries = 64;
    using ReqPosArr = std::array<std::array<Valid<ReplReqPos>, 4>, 2>;
    using PosRespArr = std::array<std::array<Valid<uint8_t>, 4>, 2>;

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

    MOD_ARRAY(ReplaceEntry, kEntries, entries);
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
    WIRE(ReqPosInArr, w_req_pos_in);
    using ReqPosOutArr = std::array<Valid<ReplReqPos>, 8>;
    WIRE(ReqPosOutArr, w_req_pos_out);

    ReplaceCM();
};

}  // namespace zj::dj
