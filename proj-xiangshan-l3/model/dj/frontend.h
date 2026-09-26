#pragma once

// Frontend 模型：对齐 frontend/{Frontend,ToChiTask,TaskBuffer,Block,PoS,Decode}.scala
// （语义 docs/dongjiang-semantics.md §7；本配置 hasHPR=false，HPR 通道空转，
//   但 TaskBuffer 的 sort/lock 机制全量建模）。

#include <array>
#include <cstdint>

#include "model/dj/frontend_types.h"
#include "model/dj/qosrr.h"
#include "model/dj/replace.h"
#include "prefab/fastq.h"
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

// ---------------- ReqToChiTask（纯组合） ----------------

class ReqToChiTask : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(uint8_t, cfg_ci);
    IN(Valid<HReqFlit>, rx_req);
    OUT(bool, rx_req_rdy);
    OUT(Valid<ChiTask>, chi_task);
    IN(bool, chi_task_rdy);

    ReqToChiTask();
};

// ---------------- TaskEntry / TaskBuffer ----------------

class TaskEntry : public wolvicmod::Module {
public:
    struct EntryReg {
        uint8_t state = taskst::kFree;  // one-hot
        Chi chi;
        uint64_t addr = 0;
        uint8_t qos = 0;

        bool operator==(const EntryReg&) const = default;
    };

    IN(bool, clk);
    IN(Valid<ChiTask>, chi_task_in);
    OUT(bool, chi_task_in_rdy);
    OUT(Valid<ChiTask>, chi_task_s0);
    IN(bool, chi_task_s0_rdy);
    IN(bool, retry_s1);
    IN(bool, sleep_s1);
    IN(Valid<uint64_t>, wakeup);
    IN(uint8_t, init_nid);  // sort：入队时同址在途数
    IN(bool, oth_rel);      // sort：同址有 release
    OUT(bool, st_valid);
    OUT(bool, st_release);
    OUT(uint64_t, st_addr);
    OUT(uint8_t, st_value);
    OUT(uint8_t, st_nid);
    OUT(bool, st_lock);

    REG(EntryReg, task_reg);
    REG(uint8_t, nid_reg);
    REG(uint8_t, retry_num_reg);
    REG(bool, timeout_reg);
    REG(bool, valid_d1);
    WIRE(bool, w_wakeup_hit);

    TaskEntry();
};

class TaskBuffer : public wolvicmod::Module {
public:
    static constexpr uint32_t kEntries = 16;  // nrReqTaskBuf（per dirBank）

    IN(bool, clk);
    IN(Valid<ChiTask>, chi_task_in);
    OUT(bool, chi_task_in_rdy);
    OUT(Valid<ChiTask>, chi_task_s0);
    IN(bool, chi_task_s0_rdy);
    OUT(bool, lock_task);
    IN(bool, retry_s1);
    IN(bool, sleep_s1);
    IN(Valid<uint64_t>, wakeup);
    OUT(bool, working);

    MOD_ARRAY(TaskEntry, kEntries, entries);
    using AllocT = zj::prefab::Alloc<ChiTask, kEntries>;
    MOD(AllocT, alloc_arb);
    using S0ArbT = zj::prefab::VipArb<ChiTask, kEntries>;
    MOD(S0ArbT, s0_arb);

    using S0InArr = std::array<Valid<ChiTask>, kEntries>;
    using BoolArr = std::array<bool, kEntries>;
    using U8Arr = std::array<uint8_t, kEntries>;
    using U64Arr = std::array<uint64_t, kEntries>;
    WIRE(S0InArr, w_s0_in);
    WIRE(BoolArr, w_alloc_rdy_all);
    WIRE(BoolArr, w_valid_all);
    WIRE(BoolArr, w_release_all);
    WIRE(BoolArr, w_lock_all);
    WIRE(U64Arr, w_addr_all);
    REG(bool, has_lock_reg);

    TaskBuffer();
};

// ---------------- Block ----------------

class Block : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(uint8_t, cfg_ci);
    IN(Valid<ChiTask>, chi_task_s0);
    OUT(Valid<TaskS1>, task_s1);
    IN(bool, pos_block_s1);
    IN(uint8_t, hn_idx_s1);
    OUT(bool, retry_s1);
    OUT(Valid<DirRdReq>, read_dir_s1);
    IN(bool, read_dir_s1_rdy);
    OUT(Valid<ReqDB>, req_db_s1);
    IN(bool, req_db_s1_rdy);
    OUT(Valid<RespFlit>, fast_resp_s1);
    IN(bool, fast_resp_s1_rdy);

    REG(bool, valid_reg_s1);
    REG(ChiTask, task_reg_s1);
    REG(bool, s_receipt_reg_s1);
    REG(bool, s_dbid_reg_s1);
    WIRE(bool, w_should_resp_s1);
    WIRE(bool, w_block_by_db_s1);
    WIRE(bool, w_block_pos);
    WIRE(bool, w_block_dir);
    WIRE(bool, w_block_resp);
    WIRE(bool, w_block_any);

    Block();
};


// ---------------- PosEntry ----------------

class PosEntry : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(uint8_t, cfg_bank_id);
    IN(uint8_t, hn_idx);     // 7bit，elab 常量
    IN(bool, alloc_valid);
    IN(uint64_t, alloc_addr);  // addrVal/tag/offset 由父模块拆好（PosSet 内）
    IN(bool, alloc_addr_val);
    IN(uint8_t, alloc_channel);
    IN(Valid<UpdPosTag>, upd_tag);
    IN(Valid<PosClean>, clean);
    OUT(Valid<uint64_t>, wakeup);
    OUT(PosState, state);
    OUT(uint64_t, state_addr);  // catPoS 重组地址（getAddrVec 用）

    REG(PosState, state_reg);
    REG(bool, wakeup_reg);
    REG(uint64_t, wakeup_addr_reg);

    PosEntry();
};

// ---------------- PosSet ----------------

class PosSet : public wolvicmod::Module {
public:
    using PosStateArr = std::array<PosState, 16>;
    using AddrArr = std::array<uint64_t, 16>;
    using BoolArr16 = std::array<bool, 16>;
    using U64Arr16 = std::array<uint64_t, 16>;
    struct AllocS1 {
        bool valid = false;
        uint64_t addr = 0;
        uint8_t channel = 0;

        bool operator==(const AllocS1&) const = default;
    };

    IN(bool, clk);
    IN(uint8_t, cfg_bank_id);
    IN(uint8_t, dir_bank);
    IN(uint8_t, pos_set);
    IN(bool, alloc_s0_valid);
    IN(uint64_t, alloc_s0_addr);
    IN(uint8_t, alloc_s0_channel);
    OUT(bool, sleep_s1);
    OUT(bool, block_s1);
    OUT(Valid<uint8_t>, hn_idx_s1);
    IN(bool, retry_s1);
    IN(bool, req_pos_valid);
    IN(uint8_t, req_pos_channel);
    OUT(Valid<uint8_t>, pos_resp);
    IN(Valid<UpdPosTag>, upd_tag);
    IN(Valid<PosClean>, clean);
    OUT(Valid<uint64_t>, wakeup);
    OUT(PosStateArr, state_vec);
    OUT(AddrArr, addr_vec);

    MOD_ARRAY(PosEntry, 16, entries);
    REG(bool, lock_reg);
    REG(AllocS1, alloc_reg_s1);
    REG(uint8_t, alloc_way_reg_s1);
    REG(bool, sleep_reg);
    REG(bool, block_reg);
    REG(bool, hn_idx_valid_reg);
    REG(bool, pos_resp_valid_reg);
    REG(uint8_t, pos_resp_way_reg);

    WIRE(PosStateArr, w_states);
    WIRE(AddrArr, w_addrs);
    using U64ArrW = std::array<Valid<uint64_t>, 16>;
    WIRE(uint32_t, w_mat_tag_vec);
    WIRE(uint32_t, w_free_vec);
    WIRE(bool, w_block_s0);
    WIRE(uint32_t, w_free_vec2);
    WIRE(uint8_t, w_repl_sel_way);
    WIRE(bool, w_req_pos_fire);
    WIRE(U64ArrW, w_wakeup_entries);

    PosSet();
};

// ---------------- PosTable ----------------

class PosTable : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(uint8_t, cfg_bank_id);
    IN(uint8_t, dir_bank);
    IN(bool, alloc_s0_valid);
    IN(uint64_t, alloc_s0_addr);
    IN(uint8_t, alloc_s0_channel);
    OUT(bool, sleep_s1);
    OUT(bool, block_s1);
    OUT(uint8_t, hn_idx_s1);
    OUT(bool, hn_idx_s1_valid);
    IN(bool, retry_s1);
    using ReqPosInArr4 = std::array<Valid<ReplReqPos>, 4>;
    using PosRespArr4 = std::array<Valid<uint8_t>, 4>;
    IN(ReqPosInArr4, req_pos_vec);
    OUT(PosRespArr4, pos_resp_vec);
    IN(Valid<UpdPosTag>, upd_tag);
    IN(Valid<PosClean>, clean);
    OUT(Valid<uint64_t>, wakeup);
    OUT(uint8_t, alr_use_pos);
    OUT(bool, working);
    using AddrVec2 = std::array<std::array<uint64_t, 16>, 4>;
    OUT(AddrVec2, addr_vec2);  // getAddrVec 用

    MOD_ARRAY(PosSet, 4, sets);
    using BoolArr4 = std::array<bool, 4>;
    using WakeArr = std::array<Valid<uint64_t>, 4>;
    WIRE(BoolArr4, w_sleep_all);
    WIRE(BoolArr4, w_block_all);
    using ValidU8Arr4 = std::array<Valid<uint8_t>, 4>;
    WIRE(ValidU8Arr4, w_hn_valid_all);
    WIRE(WakeArr, w_wakeup_all);
    WIRE(PosRespArr4, w_pos_resp_all);

    PosTable();
};

// ---------------- FrontendDecode（s2=fstDec；s3=SecDec+GetDecRes+组装） ----------------

class FrontendDecode : public wolvicmod::Module {
public:
    using DecList4 = std::array<uint8_t, 4>;
    IN(bool, clk);
    IN(uint8_t, cfg_ci);
    IN(Valid<TaskS1>, task_s2);
    IN(Valid<DirMsg>, resp_dir_s3);
    OUT(Valid<CommitTask>, cmt_task_s3);
    OUT(Valid<ReqDB>, req_db_s3);
    IN(bool, req_db_s3_rdy);
    OUT(Valid<DataTask>, fast_data_s3);
    IN(bool, fast_data_s3_rdy);
    OUT(Valid<ReqDB>, clean_db_s3);

    REG(bool, valid_reg_s3);
    REG(TaskS1, task_reg_s3);
    REG(DecList4, dec_list_reg_s3);
    WIRE(uint32_t, w_chi_inst_s2);
    WIRE(DecList4, w_dec_list_s2);
    WIRE(uint32_t, w_state_inst_s3);
    WIRE(DecList4, w_dec_list_s3);
    WIRE(uint32_t, w_task_code_s3);
    WIRE(uint32_t, w_cmt_code_s3);
    WIRE(bool, w_resp_comp_data_s3);
    WIRE(bool, w_clean_unuse_db_s3);

    FrontendDecode();
};

// ---------------- Frontend（顶层组装，单 dirBank 实例） ----------------

class Frontend : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(uint8_t, cfg_ci);
    IN(uint8_t, cfg_bank_id);
    IN(uint8_t, dir_bank);
    IN(Valid<HReqFlit>, rx_req);
    OUT(bool, rx_req_rdy);
    OUT(Valid<ReqDB>, req_db_s1);
    IN(bool, req_db_s1_rdy);
    OUT(Valid<ReqDB>, req_db_s3);
    IN(bool, req_db_s3_rdy);
    OUT(Valid<DataTask>, fast_data_s3);
    IN(bool, fast_data_s3_rdy);
    OUT(Valid<ReqDB>, clean_db_s3);
    OUT(Valid<DirRdReq>, read_dir);
    IN(bool, read_dir_rdy);
    IN(Valid<DirMsg>, resp_dir);
    OUT(Valid<CommitTask>, cmt_task);
    using U8x3 = std::array<uint8_t, 3>;
    using U64x3 = std::array<uint64_t, 3>;
    IN(U8x3, get_addr_hnidx);   // getAddrVec 三路 hnIdx 输入（Backend 驱动）
    OUT(U64x3, get_addr_result);
    using ReqPosInArr4 = std::array<Valid<ReplReqPos>, 4>;
    using PosRespArr4 = std::array<Valid<uint8_t>, 4>;
    IN(ReqPosInArr4, req_pos_vec);
    OUT(PosRespArr4, pos_resp_vec);
    IN(Valid<UpdPosTag>, upd_pos_tag);
    IN(Valid<PosClean>, clean_pos);
    OUT(Valid<RespFlit>, fast_resp);
    IN(bool, fast_resp_rdy);
    OUT(uint8_t, alr_use_pos);
    OUT(bool, working);

    MOD(ReqToChiTask, req2task);
    using RxQT = zj::prefab::FastQueue<HReqFlit, 2, false>;
    MOD(RxQT, rx_q);
    MOD(TaskBuffer, req_task_buf);
    MOD(Block, block);
    MOD(PosTable, pos_table);
    using S1PipeT = ValidPipe<TaskS1, 3>;  // readDirLatency-1=3
    MOD(S1PipeT, s1_pipe);
    MOD(FrontendDecode, decode);
    using FastRespQT = zj::prefab::FastQueue<RespFlit, 2, false>;
    MOD(FastRespQT, fast_resp_q);

    Frontend();
};

}  // namespace zj::dj

