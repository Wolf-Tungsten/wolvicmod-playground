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

    // 合并拍内状态（perf §20：原 3 个独立 reg + 2 条 fire 中转 wire）。
    // sftRead/sftWrite 同沿每拍移位、rstDone 同沿锁存，三者并为一个 struct +
    // 一条 update；read/write fire 语义同源（valid&&rdy）、消费方高度重叠
    // （st update 与 resp_pipe.enq），并为一条 struct wire。
    struct St {
        uint8_t sftRead = 0;  // 5bit，d1=bit4 … d5=bit0
        uint8_t sftWrite = 0;
        bool rstDone = false;

        bool operator==(const St&) const = default;
    };
    struct Fires {
        bool rd = false, wr = false;

        bool operator==(const Fires&) const = default;
    };
    REG(St, st);
    WIRE(Fires, w_fires);
    WIRE(bool, w_req_ready);

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

    // 预充状态（原 rst_cnt/rst_done）：同沿同使能（clk_en 门控）的计数器与
    // 完成锁存，并为一个 struct + 一条 update。
    struct Rst {
        uint8_t cnt = 0;  // Counter(64)，到 63 后 done 锁存
        bool done = false;

        bool operator==(const Rst&) const = default;
    };
    // enq/deq 的"来一个/来两个"判定（原 w_enq_one/two、w_deq_one/two）：各自
    // 读集完全相同（两端口 valid / 两端口 rdy），各并为一个 struct。
    struct Cnt {
        bool one = false, two = false;

        bool operator==(const Cnt&) const = default;
    };
    REG(Rst, rst);
    WIRE(Cnt, w_enq_cnt);
    WIRE(bool, w_enq_sel_q0);
    WIRE(Cnt, w_deq_cnt);
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
    // 读侧控制（原 r_chi_sft/r_ds_sft/rreq_val_reg/rreq_addr_reg）：两条 2bit 读
    // 请求移位与 datBuf 读口请求寄存同沿更新、使能同源（读 fire），并为一个
    // struct + 一条 update。
    struct RdCtl {
        uint8_t chiSft = 0, dsSft = 0;  // 2bit 读请求移位
        bool rreqVal = false;           // datBuf 读口：RegNext(chiFire||dsFire)
        uint8_t rreqAddr = 0;           // RegEnable(dbid, fires)（DS 优先）

        bool operator==(const RdCtl&) const = default;
    };
    // 写口提交寄存组（原 wval/ds_wri/repl/mask/be/read_or_snp/waddr/wdata_reg）：
    // 全部同沿采样 dsResp/fromCHI 侧输入（valid+1 提交），并为一个 struct +
    // 一条 update，各字段保持原 RegNext/RegEnable 语义；w_wri_val/w_wri_dbid/
    // w_read_or_snp 三条中转线唯一消费方即本组，已内联进 update lambda。
    struct WrReg {
        bool wval = false, dsWri = false, repl = false, readOrSnp = false;
        uint32_t mask = 0;
        uint64_t be = 0;
        uint8_t waddr = 0;
        ByteRow wdata{};

        bool operator==(const WrReg&) const = default;
    };
    // 读数据回送信息链（fire → +1 → +2 两级，对齐 rresp）：chi/ds 各自的 d1/d2
    // 寄存器同沿、使能分别同源（chiFire / enD1(chiSft)），各并为一个 struct。
    struct ChiPipe {
        uint8_t dbidD1 = 0, dbidD2 = 0, beatD1 = 0, beatD2 = 0, dcidD1 = 0, dcidD2 = 0;

        bool operator==(const ChiPipe&) const = default;
    };
    struct DsPipe {
        uint8_t dcidD1 = 0, dcidD2 = 0, beatD1 = 0, beatD2 = 0;
        DsIdx dsD1{}, dsD2{};

        bool operator==(const DsPipe&) const = default;
    };
    REG(RdCtl, rd_ctl);
    REG(WrReg, wr);
    REG(ChiPipe, chi_pipe);
    REG(DsPipe, ds_pipe);

    WIRE(bool, w_rd_chi_fire);
    WIRE(bool, w_rd_ds_fire);
    // w_has_free_chi/w_has_free_ds 为单消费方中转线，已内联进 read_to_*_rdy。

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

    // task 输入延迟一拍（原 task_fire_reg/task_reg）：RegNext(valid) 与
    // RegEnable(bits, valid) 同沿同源，并为一个 struct + 一条 update。
    struct TaskD {
        bool fire = false;
        DataTask task;

        bool operator==(const TaskD&) const = default;
    };
    REG(TaskD, task_d);

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
    // 条目派生标志（原 w_alloc_rdy_all/w_read_for_repl_all）：同读 entries 的
    // 两个 64 位标志数组，并为一条 struct wire。
    struct Flags {
        RdyArr64 allocRdy{}, readForRepl{};

        bool operator==(const Flags&) const = default;
    };
    // 条目查询视图（原 w_states/w_tx_dat_bits）：同读 entries 的 getDBID /
    // getChiDat 查询载荷，并为一条 struct wire。
    struct Views {
        StateArr states;
        TxBitsArr txBits;

        bool operator==(const Views&) const = default;
    };
    WIRE(Flags, w_flags);
    WIRE(Views, w_views);
    // 空闲项 / repl 项选择（原 w_has_free_dc+w_free_dcid、w_has_repl+
    // w_repl_dcid）：has 与 dcid 同读集同消费方，各并为一个 struct。
    struct Sel {
        bool has = false;
        uint8_t dcid = 0;

        bool operator==(const Sel&) const = default;
    };
    WIRE(Sel, w_free_sel);
    WIRE(Sel, w_repl_sel);
    // 整条 update 的静止门（perf-breakdown §21）：无非空闲项且无 alloc 时
    // compute/commit 全跳过。
    WIRE(bool, w_any);
    // 条目 read 通道 rdy 回接（原 w_db_rdys/w_ds_rdys/w_chi_rdys 三条数组
    // assign）：唯一消费方是 entries update，已内联进该 lambda。

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
