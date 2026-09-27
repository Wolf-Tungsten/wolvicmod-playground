#pragma once

// Commit / CommitEntry / BackendDecode：对齐 backend/{Commit,Decode}.scala
// （语义 docs/dongjiang-semantics.md §5.2/§5.3）。
//   BackendDecode — Third/Fourth 译码（2 拍流水 + dj_decode 查表）
//   CommitEntry   — 五态 FSM + 双层 flag（entry=112：2 dirBank × posSets4 × way0-13）
//   Commit        — entry 阵列 + 两级译码 + 输出仲裁

#include <array>
#include <cstdint>

#include "model/dj/backend_types.h"
#include "model/dj/qosrr.h"
#include "prefab/fastq.h"
#include "prefab/xsarb.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::dj {

using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Valid;
using zj::prefab::VipArb;

// ---------------- 译码请求/结果载荷 ----------------

struct DecMes {  // taskInst + decList + hnTxnID
    uint32_t taskInst = 0;  // TaskInst 打包（19b）
    std::array<uint8_t, 4> decList{};
    uint8_t hnTxnID = 0;

    bool operator==(const DecMes&) const = default;
};

// ---------------- BackendDecode（Decode("Third"/"Fourth")） ----------------

template <bool Third>
class BackendDecode : public wolvicmod::Module {
public:
    using DecListArr = std::array<uint8_t, 4>;

    IN(bool, clk);
    IN(Valid<DecMes>, dec_mes_in);
    OUT(Valid<uint8_t>, hn_txn_id_out);
    OUT(DecListArr, dec_list_out);  // Output（非 Valid，Out 侧有效时取值）
    OUT(uint32_t, task_code_out);
    OUT(uint32_t, cmt_code_out);

    REG(bool, dec_val_reg);
    REG(bool, hn_id_val_reg);
    REG(uint8_t, hn_id_reg);
    REG(DecMes, dec_mes_reg);
    REG(DecListArr, dec_list_reg);
    REG(uint32_t, task_code_reg);
    REG(uint32_t, cmt_code_reg);
    WIRE(uint32_t, w_task_inst);
    WIRE(DecListArr, w_dec_list);
    WIRE(uint32_t, w_task_code);
    WIRE(uint32_t, w_cmt_code);

    BackendDecode();
};

using BackendDecodeThird = BackendDecode<true>;
using BackendDecodeFourth = BackendDecode<false>;

// ---------------- CommitEntry ----------------

class CommitEntry : public wolvicmod::Module {
public:
    using DecListArr = std::array<uint8_t, 4>;

    struct Flag {
        // intl.s / intl.w
        bool sDecode = false, sReqDB = false, sCmTask = false, sDataTask = false,
             sWriDir = false;
        bool wCmResp = false, wReplResp = false, wDataResp = false;
        // chi.s / chi.w
        bool sDbid = false, sResp = false;
        bool wXCB0 = false, wXCB1 = false, wCompAck = false;

        bool operator==(const Flag&) const = default;
    };
    struct AlrGet {
        bool compAck = false, ncbWrD0 = false, ncbWrD1 = false;

        bool operator==(const AlrGet&) const = default;
    };

    static constexpr uint8_t kFree = 0, kFstTask = 1, kSecTask = 2, kCommit = 3, kClean = 4;

    IN(bool, clk);
    IN(uint8_t, cfg_ci);  // 本配置行为未用（保持接口一致）
    IN(uint8_t, cfg_bank_id);
    IN(uint8_t, hn_txn_id);  // 7bit，elab 常量
    IN(uint8_t, hn_idx);     // 7bit，elab 常量
    IN(Valid<CommitTask>, alloc);  // HnTxnID 藏在 task 里？——RTL 为 CommitTask with HasHnTxnID
    OUT(Valid<DecMes>, trd_dec_out);
    IN(bool, trd_dec_out_rdy);
    OUT(Valid<DecMes>, fth_dec_out);
    IN(bool, fth_dec_out_rdy);
    IN(Valid<DecListArr>, dec_list_in);
    IN(uint32_t, task_code_in);
    IN(uint32_t, cmt_code_in);
    OUT(Valid<RespFlit>, tx_rsp);
    IN(bool, tx_rsp_rdy);
    IN(Valid<RespFlit>, rx_rsp);
    IN(Valid<DataFlit>, rx_dat);
    OUT(Valid<CMTask>, cm_task_snp);
    IN(bool, cm_task_snp_rdy);
    OUT(Valid<CMTask>, cm_task_wri);
    IN(bool, cm_task_wri_rdy);
    OUT(Valid<CMTask>, cm_task_read);
    IN(bool, cm_task_read_rdy);
    IN(Valid<CMResp>, cm_resp);
    OUT(Valid<ReplTask>, repl_task);
    IN(bool, repl_task_rdy);
    IN(Valid<uint8_t>, repl_resp);
    OUT(Valid<ReqDBQos>, req_db);
    IN(bool, req_db_rdy);
    OUT(Valid<DataTask>, data_task);
    IN(bool, data_task_rdy);
    IN(Valid<uint8_t>, data_resp);
    OUT(Valid<PosClean>, clean_pos);
    IN(bool, clean_pos_rdy);
    OUT(uint8_t, state_out);

    REG(CommitTask, task_reg);
    REG(Flag, flag_reg);
    REG(uint8_t, state_reg);  // 3b
    REG(uint32_t, inst_reg);  // TaskInst 打包（19b）
    REG(AlrGet, alr_get_reg);
    REG(uint8_t, resp_err_reg);  // 2b

    // 派生线网（大量小 wire 跟随 RTL 结构）
    WIRE(bool, w_rx_rsp_hit);
    WIRE(bool, w_rx_dat_hit);
    WIRE(bool, w_comp_ack_hit);
    WIRE(bool, w_xcb_hit0);
    WIRE(bool, w_xcb_hit1);
    WIRE(bool, w_cm_resp_hit);
    WIRE(bool, w_alloc_hit);
    WIRE(bool, w_valid);
    WIRE(Flag, w_flag_next);
    WIRE(uint32_t, w_inst_next);
    WIRE(CommitTask, w_task_next);
    WIRE(uint8_t, w_state_next);
    WIRE(uint8_t, w_resp_err_next);
    WIRE(AlrGet, w_alr_get_next);
    WIRE(bool, w_set);

    CommitEntry();
};

// ---------------- Commit（顶层：112 entry + 两级译码 + 输出仲裁） ----------------

class Commit : public wolvicmod::Module {
public:
    static constexpr uint32_t kEntries = 112;  // 2 × 56

    IN(bool, clk);
    IN(uint8_t, cfg_ci);
    IN(uint8_t, cfg_bank_id);
    IN(Valid<CommitTask>, cmt_task_0);
    IN(Valid<CommitTask>, cmt_task_1);
    OUT(Valid<RespFlit>, tx_rsp);
    IN(bool, tx_rsp_rdy);
    IN(Valid<RespFlit>, rx_rsp);
    IN(Valid<DataFlit>, rx_dat);
    OUT(Valid<CMTask>, cm_task_snp);
    IN(bool, cm_task_snp_rdy);
    OUT(Valid<CMTask>, cm_task_wri);
    IN(bool, cm_task_wri_rdy);
    OUT(Valid<CMTask>, cm_task_read);
    IN(bool, cm_task_read_rdy);
    IN(Valid<CMResp>, cm_resp);
    OUT(Valid<ReqDB>, req_db);
    IN(bool, req_db_rdy);
    OUT(Valid<ReplTask>, repl_task);
    IN(bool, repl_task_rdy);
    IN(Valid<uint8_t>, repl_resp);
    OUT(Valid<DataTask>, data_task);
    IN(bool, data_task_rdy);
    IN(Valid<uint8_t>, data_resp);
    OUT(Valid<PosClean>, clean_pos);
    IN(bool, clean_pos_rdy);

    MOD_ARRAY(CommitEntry, kEntries, entries);
    MOD(BackendDecodeThird, trd_dec);
    MOD(BackendDecodeFourth, fth_dec);

    // 译码请求 RR 合流（validOnly）
    using DecArbT = VipArb<DecMes, kEntries>;
    MOD(DecArbT, trd_arb);
    MOD(DecArbT, fth_arb);
    // 输出 QosRR 仲裁
    using TxRspArbT = QosRRArb<RespFlit, kEntries>;
    using CmTaskArbT = QosRRArb<CMTask, kEntries>;
    using ReqDbArbT = QosRRArb<ReqDBQos, kEntries>;
    using ReplArbT = QosRRArb<ReplTask, kEntries>;
    using DataTaskArbT = QosRRArb<DataTask, kEntries>;
    using CleanArbT = QosRRArb<PosClean, kEntries>;
    MOD(TxRspArbT, tx_rsp_arb);
    MOD(CmTaskArbT, snp_arb);
    MOD(CmTaskArbT, wri_arb);
    MOD(CmTaskArbT, read_arb);
    MOD(ReqDbArbT, req_db_arb);
    MOD(ReplArbT, repl_arb);
    MOD(DataTaskArbT, data_task_arb);
    MOD(CleanArbT, clean_arb);

    // 汇集线
    using DecInArr = std::array<Valid<DecMes>, kEntries>;
    using TxRspInArr = std::array<Valid<RespFlit>, kEntries>;
    using CmTaskInArr = std::array<Valid<CMTask>, kEntries>;
    using ReqDbInArr = std::array<Valid<ReqDBQos>, kEntries>;
    using ReplInArr = std::array<Valid<ReplTask>, kEntries>;
    using DataTaskInArr = std::array<Valid<DataTask>, kEntries>;
    using CleanInArr = std::array<Valid<PosClean>, kEntries>;
    WIRE(DecInArr, w_trd_in);
    WIRE(DecInArr, w_fth_in);
    WIRE(TxRspInArr, w_tx_rsp_in);
    WIRE(CmTaskInArr, w_snp_in);
    WIRE(CmTaskInArr, w_wri_in);
    WIRE(CmTaskInArr, w_read_in);
    WIRE(ReqDbInArr, w_req_db_in);
    WIRE(ReplInArr, w_repl_in);
    WIRE(DataTaskInArr, w_data_task_in);
    WIRE(CleanInArr, w_clean_in);

    Commit();
};

}  // namespace zj::dj
