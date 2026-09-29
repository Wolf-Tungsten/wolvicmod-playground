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

    // 合并流水状态（perf-breakdown §20：原 7 个独立 reg + 4 条中间 wire）。
    // stage1 寄存输入；stage2 以 stage1 valid 为使能寄存译码结果——两级同沿
    // 同拍更新，按行为语义并为一个 struct + 一条 update，译码查表内联。
    struct St {
        bool decVal = false;   // stage1: RegNext(dec_mes_in.valid)
        DecMes mes;            // stage1: RegEnable(dec_mes_in.bits, valid)
        bool hnIdVal = false;  // stage2: RegNext(decVal)
        uint8_t hnId = 0;      // stage2: RegEnable(mes.hnTxnID, decVal)
        DecListArr decList{};  // stage2: RegEnable(译码结果, decVal)
        uint32_t taskCode = 0;
        uint32_t cmtCode = 0;

        bool operator==(const St&) const = default;
    };
    REG(St, st);

    BackendDecode();
};

using BackendDecodeThird = BackendDecode<true>;
using BackendDecodeFourth = BackendDecode<false>;


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

    // ---- 拍平的表项（原 CommitEntry 子模块，112 实例并入数组） ----
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
    struct V {
        CommitTask task;
        Flag flag;
        AlrGet alrGet;
        uint32_t inst = 0;    // TaskInst 打包（19b）
        uint8_t state = 0;    // 3b
        uint8_t respErr = 0;  // 2b

        bool operator==(const V&) const = default;
    };
    static constexpr uint8_t kFree = 0, kFstTask = 1, kSecTask = 2, kCommit = 3, kClean = 4;
    // entry i 的 hnTxnID/hnIdx（同一数值）：bank*64 + set*16 + way
    static constexpr uint8_t hnIdOf(uint32_t i) {
        return static_cast<uint8_t>((i / 56) * 64 + ((i % 56) / 14) * 16 + (i % 14));
    }
    using EntryArr = std::array<V, kEntries>;
    using BoolArrN = std::array<bool, kEntries>;
    using U8ArrN = std::array<uint8_t, kEntries>;
    using U32ArrN = std::array<uint32_t, kEntries>;
    using TaskArrN = std::array<CommitTask, kEntries>;
    using FlagArrN = std::array<Flag, kEntries>;
    using AlrArrN = std::array<AlrGet, kEntries>;
    using DecListArr = std::array<uint8_t, 4>;
    using DecListInArr = std::array<Valid<DecListArr>, kEntries>;

    REG(EntryArr, entries);
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

    // 汇集线（拍平后由数组化 assign 驱动）
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

    // per-entry 派生/次态数组
    WIRE(BoolArrN, w_valid);
    WIRE(BoolArrN, w_alloc_hit);
    WIRE(BoolArrN, w_comp_ack_hit);
    WIRE(BoolArrN, w_xcb_hit0);
    WIRE(BoolArrN, w_xcb_hit1);
    WIRE(BoolArrN, w_cm_resp_hit);
    WIRE(DecListInArr, w_dec_list_in);
    WIRE(U32ArrN, w_task_code_in);
    WIRE(U32ArrN, w_cmt_code_in);
    WIRE(U8ArrN, w_state_next);
    WIRE(TaskArrN, w_task_next);
    WIRE(FlagArrN, w_flag_next);
    WIRE(U32ArrN, w_inst_next);
    WIRE(AlrArrN, w_alr_get_next);
    WIRE(U8ArrN, w_resp_err_next);
    WIRE(BoolArrN, w_set);

    Commit();
};

}  // namespace zj::dj
