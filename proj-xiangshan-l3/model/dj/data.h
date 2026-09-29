#pragma once

// DataBlock 模型：对齐 dongjiang/data/*.scala（语义 docs/dongjiang-semantics.md §4）。
//   BeatStorage   — HomeDatRam（SpSram 2,2,outreg，5 拍）+ 5 拍移位 + respPipe
//   DBIDPool      — 双 FastQueue(64) dbid 池（预充/长短平衡）
//   DBIDCtrl      — dbid 分配/释放组合逻辑
//   DataBuffer    — datBuf(DpSram 字节阵列) + mask/repl 跟踪 + toCHIQ/toDSQ
//   DataCM        — 八态控制 FSM ×64（拍平：REG 数组 + 一条 update）+
//                   仲裁（repl 捆绑 / critical / QoS-RR）
//   DataBlock     — 顶层组装（txDat 二选一、DS 交叉分发）

#include <array>
#include <cstdint>

#include "model/dj/data_types.h"
#include "prefab/fastq.h"
#include "prefab/sram.h"
#include "prefab/xsarb.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/arb.h"
#include "wolvicmod/prefab/pipe.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::dj {

using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Valid;
using wolvicmod::prefab::ValidPipe;
using zj::prefab::DpSram;
using zj::prefab::FastQueue;
using zj::prefab::SpSram;
using zj::prefab::VipArb;
using wolvicmod::prefab::FixedArb;

// 字节行 ↔ Beat 互转（datBuf 按字节 lane 存储）
inline std::array<uint8_t, kBeatByte> beatToBytes(const Beat& b) {
    std::array<uint8_t, kBeatByte> r{};
    for (uint32_t i = 0; i < kBeatByte; ++i) r[i] = static_cast<uint8_t>(b[i / 8] >> (8 * (i % 8)));
    return r;
}
inline Beat bytesToBeat(const std::array<uint8_t, kBeatByte>& bs) {
    Beat r{};
    for (uint32_t i = 0; i < kBeatByte; ++i)
        r[i / 8] |= static_cast<uint64_t>(bs[i]) << (8 * (i % 8));
    return r;
}

// ---------------- BeatStorage ----------------

class BeatStorage : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(bool, clk_en);  // 门控时钟使能（横扫冻结），透传 DatRam
    IN(Valid<ReadDS>, read);
    OUT(bool, read_rdy);
    IN(Valid<WriteDS>, write);
    OUT(bool, write_rdy);
    OUT(Valid<DsResp>, resp);

    struct RespInfo {
        uint8_t dcid = 0, dbid = 0, beatNum = 0;
        bool toCHI = false;

        bool operator==(const RespInfo&) const = default;
    };
    using DatRam = SpSram<Beat, kNrDsSet, 1, 2, 2, false, true, false>;
    using RespPipeT = ValidPipe<RespInfo, kReadDsLatency>;
    MOD(DatRam, array);
    MOD(RespPipeT, resp_pipe);

    REG(uint8_t, sft_read);   // 5bit，d1=bit4 … d5=bit0
    REG(uint8_t, sft_write);
    REG(bool, rst_done);
    WIRE(bool, w_req_ready);
    WIRE(bool, w_read_fire);
    WIRE(bool, w_write_fire);

    BeatStorage();
};

// ---------------- DBIDPool ----------------

class DBIDPool : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(bool, clk_en);  // 门控时钟使能（预充冻结，见 prefab/sram.h）
    IN(Valid<uint8_t>, enq0);
    IN(Valid<uint8_t>, enq1);
    OUT(Valid<uint8_t>, deq0);
    IN(bool, deq0_rdy);
    OUT(Valid<uint8_t>, deq1);
    IN(bool, deq1_rdy);

    using IdQ = FastQueue<uint8_t, kNrDataBuf / 2, false>;
    MOD(IdQ, q0);
    MOD(IdQ, q1);

    REG(uint8_t, rst_cnt);  // Counter(64)，到 63 后 rst_done 锁存
    REG(bool, rst_done);
    WIRE(bool, w_enq_one);
    WIRE(bool, w_enq_two);
    WIRE(bool, w_enq_sel_q0);
    WIRE(bool, w_deq_one);
    WIRE(bool, w_deq_two);
    WIRE(bool, w_deq_sel_q0);

    DBIDPool();
};

// ---------------- DBIDCtrl ----------------

class DBIDCtrl : public wolvicmod::Module {
public:
    using RespArr = std::array<uint8_t, kNrBeat>;

    IN(bool, clk);
    IN(bool, clk_en);  // 门控时钟使能，透传 DBIDPool
    IN(Valid<uint8_t>, req);  // Vec(nrBeat, Bool)：各 beat 是否要 dbid
    OUT(bool, req_rdy);
    OUT(RespArr, resp);       // Vec（组合直连，无 valid）
    IN(Valid<DBIDVecC>, release);

    MOD(DBIDPool, pool);
    WIRE(bool, w_has_two);

    DBIDCtrl();
};

// ---------------- DataBuffer ----------------

class DataBuffer : public wolvicmod::Module {
public:
    using ByteRow = std::array<uint8_t, kBeatByte>;
    using MaskArr = std::array<uint32_t, kNrDataBuf>;
    using ReplArr = std::array<bool, kNrDataBuf>;

    IN(bool, clk);
    IN(bool, clk_en);  // 门控时钟使能，透传 DatBuf
    IN(Valid<ReadDB>, read_to_chi);
    OUT(bool, read_to_chi_rdy);
    IN(Valid<ReadDB>, read_to_ds);
    OUT(bool, read_to_ds_rdy);
    IN(Valid<DBIDVecC>, clean);
    IN(Valid<DsResp>, ds_resp);
    IN(Valid<FromCHI>, from_chi);
    OUT(bool, from_chi_rdy);
    OUT(Valid<WriteDS>, write_ds);
    IN(bool, write_ds_rdy);
    OUT(Valid<ToCHIEntry>, to_chi);
    IN(bool, to_chi_rdy);

    using DatBuf = DpSram<uint8_t, kNrDataBuf, kBeatByte, false, 1, 1, false, false, false>;
    using ToDSQ = FastQueue<WriteDS, 2, false>;
    using ToCHIQ = FastQueue<ToCHIEntry, 2, false>;
    MOD(DatBuf, dat_buf);
    MOD(ToDSQ, to_ds_q);
    MOD(ToCHIQ, to_chi_q);

    REG(MaskArr, mask_vec);  // 每 dbid 已收字节掩码
    REG(ReplArr, repl_vec);
    REG(uint8_t, r_chi_sft);  // 2bit 读请求移位
    REG(uint8_t, r_ds_sft);
    // 写口寄存（valid+1 提交）
    REG(bool, wval_reg);
    REG(bool, ds_wri_reg);
    REG(bool, repl_reg);
    REG(uint32_t, mask_reg);
    REG(uint64_t, be_reg);
    REG(bool, read_or_snp_reg);
    REG(uint8_t, waddr_reg);
    REG(ByteRow, wdata_reg);
    // 读口两级信息链（fire → +1 rreq → +2 enq）
    REG(uint8_t, chi_dbid_d1);
    REG(uint8_t, chi_dbid_d2);
    REG(uint8_t, chi_beat_d1);
    REG(uint8_t, chi_beat_d2);
    REG(uint8_t, chi_dcid_d1);
    REG(uint8_t, chi_dcid_d2);
    REG(uint8_t, ds_dcid_d1);
    REG(uint8_t, ds_dcid_d2);
    REG(uint8_t, ds_beat_d1);
    REG(uint8_t, ds_beat_d2);
    REG(DsIdx, ds_ds_d1);
    REG(DsIdx, ds_ds_d2);
    REG(bool, rreq_val_reg);
    REG(uint8_t, rreq_addr_reg);

    WIRE(bool, w_rd_chi_fire);
    WIRE(bool, w_rd_ds_fire);
    WIRE(bool, w_has_free_chi);
    WIRE(bool, w_has_free_ds);
    WIRE(bool, w_wri_val);
    WIRE(uint8_t, w_wri_dbid);
    WIRE(bool, w_read_or_snp);

    DataBuffer();
};

// ---------------- DataCM 表项状态 ----------------

namespace ctrl {
constexpr uint8_t kFree = 0, kAlloc = 1, kRepl = 2, kRead = 3, kSend = 4, kSave = 5,
                  kResp = 6, kClean = 7;
}  // namespace ctrl

// ---------------- DataCM ----------------
// 拍平建模：RTL 的 DataCtrlEntry 子模块阵列（×64）在 C 模型里只是 for 循环，
// 不再做子模块。64 项控制状态是一个 REG 数组（一条 update 循环算全数组 next）；
// per-entry 组合输出为 std::array 值的单条 assign 直喂仲裁器；resp/release/三读
// 通道仲裁器（VipArb）保留为子模块。对外端口与原层次版逐位等价。

class DataCM : public wolvicmod::Module {
public:
    using DbRespArr = std::array<uint8_t, kNrBeat>;

    IN(bool, clk);
    IN(Valid<UpdHnTxnID>, upd_hn_txn_id);
    IN(Valid<ReqDB>, req_db_in);
    OUT(bool, req_db_in_rdy);
    IN(Valid<DataTask>, task);
    OUT(Valid<uint8_t>, resp);  // HnTxnID
    IN(Valid<CleanBits>, clean);
    OUT(Valid<ReadDS>, read_to_db);
    IN(bool, read_to_db_rdy);
    OUT(Valid<ReadDB>, read_to_ds);
    IN(bool, read_to_ds_rdy);
    OUT(Valid<ReadDB>, read_to_chi);
    IN(bool, read_to_chi_rdy);
    OUT(Valid<uint8_t>, req_db_out);  // Vec(nrBeat, Bool)
    IN(bool, req_db_out_rdy);
    IN(DbRespArr, dbid_resp);
    // getChiDat / getDBID（DataBlock 顶层的 DataFlit/dbid 查询）
    IN(bool, get_chi_dat_valid);
    IN(uint8_t, get_chi_dat_dcid);
    OUT(DataFlit, get_chi_dat_bits);
    IN(bool, get_dbid_valid);
    IN(uint16_t, get_dbid_txn_id);
    IN(uint8_t, get_dbid_data_id);
    OUT(uint8_t, get_dbid_dbid);
    OUT(Valid<DBIDVecC>, release);
    IN(Valid<DcidBeat>, ds_wri_db);
    IN(Valid<DcidBeat>, tx_dat_fire);
    IN(Valid<DcidBeat>, db_wri_ds);

    struct DataCtrlV {  // 一个 DataCtrlEntry 的全部寄存器状态（原 EntryReg）
        uint8_t state = ctrl::kFree;
        bool critical = false;
        uint8_t s_read = 0, s_send = 0, s_save = 0;  // 待发 beat 位图（2bit）
        uint8_t w_read = 0, w_send = 0, w_save = 0;  // 待完成 beat 位图
        DataTask task;
        uint8_t dataVec = 0;
        std::array<uint8_t, kNrBeat> dbidVec{};

        bool operator==(const DataCtrlV&) const = default;
    };
    using CtrlArr = std::array<DataCtrlV, kNrDataCM>;
    REG(CtrlArr, entries);

    REG(bool, task_fire_reg);
    REG(DataTask, task_reg);

    // 仲裁：resp/release 各一路 RR（VipArb）；三读通道各两层（QoS 高优 + 普通）
    using RespArbT = VipArb<uint8_t, kNrDataCM>;
    using RelArbT = VipArb<DBIDVecC, kNrDataCM>;
    using DbArbT = VipArb<ReadDS, kNrDataCM>;
    using DsArbT = VipArb<ReadDB, kNrDataCM>;
    using ChiArbT = VipArb<ReadDB, kNrDataCM>;
    MOD(RespArbT, resp_arb);
    MOD(RelArbT, rel_arb);
    MOD(DbArbT, db_hi_arb);
    MOD(DbArbT, db_lo_arb);
    MOD(DsArbT, ds_hi_arb);
    MOD(DsArbT, ds_lo_arb);
    MOD(ChiArbT, chi_hi_arb);
    MOD(ChiArbT, chi_lo_arb);

    // 端口阵列汇集线（条目派生输出 → 仲裁输入；拍平后为单条数组 assign）
    using RespInArr = std::array<Valid<uint8_t>, kNrDataCM>;
    using RelInArr = std::array<Valid<DBIDVecC>, kNrDataCM>;
    using DbInArr = std::array<Valid<ReadDS>, kNrDataCM>;
    using DsInArr = std::array<Valid<ReadDB>, kNrDataCM>;
    using RdyArr64 = std::array<bool, kNrDataCM>;
    using TxBitsArr = std::array<DataFlit, kNrDataCM>;
    using StateArr = std::array<Valid<EntryState>, kNrDataCM>;
    WIRE(RespInArr, w_resp_in);
    WIRE(RelInArr, w_rel_in);
    WIRE(DbInArr, w_db_in);
    WIRE(DsInArr, w_ds_in);
    WIRE(DsInArr, w_chi_in);
    WIRE(RdyArr64, w_alloc_rdy_all);
    WIRE(RdyArr64, w_read_for_repl_all);
    WIRE(TxBitsArr, w_tx_dat_bits);
    WIRE(StateArr, w_states);
    // 条目 read 通道 rdy 回接（原 entries[i].read_to_*_rdy 输入）
    WIRE(RdyArr64, w_db_rdys);
    WIRE(RdyArr64, w_ds_rdys);
    WIRE(RdyArr64, w_chi_rdys);
    // 仲裁结果与选择
    WIRE(bool, w_has_repl);
    WIRE(uint8_t, w_repl_dcid);
    WIRE(uint8_t, w_free_dcid);
    WIRE(bool, w_has_free_dc);

    DataCM();
};

// ---------------- DataBlock ----------------

class DataBlock : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(bool, clk_en);  // 门控时钟使能（横扫/预充冻结），透传 BeatStorage/DBIDCtrl/DataBuffer
    OUT(Valid<DataFlit>, tx_dat);
    IN(bool, tx_dat_rdy);
    IN(Valid<DataFlit>, rx_dat);
    OUT(bool, rx_dat_rdy);
    IN(Valid<UpdHnTxnID>, upd_hn_txn_id);
    IN(Valid<ReqDB>, req_db);
    OUT(bool, req_db_rdy);
    IN(Valid<DataTask>, task);
    OUT(Valid<uint8_t>, resp);  // HnTxnID
    IN(Valid<CleanBits>, clean_db);

    MOD_ARRAY(BeatStorage, kNrDSBank * kNrBeat, beat_storages);  // [bank*2+beatNum]
    MOD(DataCM, data_cm);
    MOD(DBIDCtrl, dbid_ctrl);
    MOD(DataBuffer, dat_buf);

    // dsResp 两级 fastArb：每 bank 2 合 1（固定优先）→ Pipe(1) → 4 合 1
    using DsBankArbT = FixedArb<DsResp, kNrBeat>;
    using DsRespPipeT = ValidPipe<DsResp, 1>;
    using DsTopArbT = FixedArb<DsResp, kNrDSBank>;
    MOD_ARRAY(DsBankArbT, kNrDSBank, ds_bank_arbs);
    MOD_ARRAY(DsRespPipeT, kNrDSBank, ds_resp_pipes);
    MOD(DsTopArbT, ds_top_arb);

    // DS 交叉分发线
    using BsRdyArr = std::array<bool, kNrDSBank * kNrBeat>;
    using DsPipeArr = std::array<Valid<DsResp>, kNrDSBank>;
    WIRE(BsRdyArr, w_bs_read_rdy);
    WIRE(BsRdyArr, w_bs_write_rdy);
    WIRE(DsPipeArr, w_ds_pipe_deq);
    WIRE(Valid<DsResp>, w_ds_resp_arbed);
    WIRE(bool, w_db_to_chi);
    WIRE(bool, w_ds_to_chi);

    DataBlock();
};

}  // namespace zj::dj
