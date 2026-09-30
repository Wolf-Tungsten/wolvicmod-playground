#pragma once

// Frontend 模型：对齐 frontend/{Frontend,ToChiTask,TaskBuffer,Block,PoS,Decode}.scala
// （语义 docs/wolvicmod-zhujiang-model.md §5.7；本配置 hasHPR=false，HPR 通道空转，
//   但 TaskBuffer 的 sort/lock 机制全量建模）。

#include <array>
#include <cstdint>

#include "model/dj/frontend_types.h"
#include "model/dj/qosrr.h"
#include "model/dj/replace.h"
#include "prefab/fastq.h"
#include "prefab/xsarb.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/expr.h"
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

// ---------------- TaskBuffer ----------------

// N = 任务项数（本配置 req=16、hpr=8，per dirBank）
// 拍平建模：TaskEntry 不做子模块，N 项状态为一个寄存器数组 + 一条 update 循环；
// alloc/s0 仲裁器（Alloc/VipArb）保留为子模块。
template <uint32_t N>
class TaskBuffer : public wolvicmod::Module {
public:
    static constexpr uint32_t kEntries = N;

    struct EntryReg {
        uint8_t state = taskst::kFree;  // one-hot
        Chi chi;
        uint64_t addr = 0;
        uint8_t qos = 0;

        bool operator==(const EntryReg&) const = default;
    };
    struct EntryV {
        EntryReg task;
        uint8_t nid = 0;
        uint8_t retryNum = 0;
        bool timeout = false;
        bool validD1 = false;

        bool operator==(const EntryV&) const = default;
    };
    using EntryArr = std::array<EntryV, kEntries>;

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

    using AllocT = zj::prefab::Alloc<ChiTask, kEntries>;
    MOD(AllocT, alloc_arb);
    using S0ArbT = zj::prefab::VipArb<ChiTask, kEntries>;
    MOD(S0ArbT, s0_arb);

    using S0InArr = std::array<Valid<ChiTask>, kEntries>;
    using BoolArr = std::array<bool, kEntries>;
    using U8Arr = std::array<uint8_t, kEntries>;
    using U64Arr = std::array<uint64_t, kEntries>;
    REG(EntryArr, entries);
    REG(bool, has_lock_reg);
    WIRE(S0InArr, w_s0_in);
    WIRE(BoolArr, w_alloc_rdy_all);
    WIRE(BoolArr, w_lock_all);
    // 整条 update 的静止门（perf-breakdown §23）
    WIRE(bool, w_any);

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

    // 合并 s1 流水状态（perf-breakdown：原 4 个独立 reg 同沿同读 chi_task_s0）。
    // valid 为 RegNext(valid)；task 为 RegEnable(bits, valid)；sReceipt/sDbid 为
    // 无条件 RegNext 的译码标志——按行为语义并为一个 struct + 一条 update。
    struct St {
        bool valid = false;    // RegNext(chi_task_s0.valid)
        ChiTask task;          // RegEnable(chi_task_s0.bits, valid)
        bool sReceipt = false; // RegNext(isRead && (isEO || isRO))
        bool sDbid = false;    // RegNext(isWrite && !isCopyBackWrite)

        bool operator==(const St&) const = default;
    };
    REG(St, st);
    // 阻塞条件组合链（原 6 条 wire assign，读集高度重叠：st 字段 + 三个 rdy +
    // pos_block_s1，同为 shouldResp→resp→any 一条链），并为一条 struct assign。
    struct BlkW {
        bool shouldResp = false;  // sReceipt || (sDbid && req_db_rdy)
        bool byDb = false;        // sDbid && !req_db_rdy
        bool pos = false;         // pos_block_s1 直通
        bool dir = false;         // memCacheable && !read_dir_rdy
        bool resp = false;        // byDb || (shouldResp && !fast_resp_rdy)
        bool any = false;         // pos || dir || resp

        bool operator==(const BlkW&) const = default;
    };
    WIRE(BlkW, w_blk);

    Block();
};


// ---------------- PosTable ----------------
// 拍平建模：RTL 的 PosEntry/PosSet 层次在 C 模型里只是 for 循环，不再做子模块。
// 64 项表项状态是一个寄存器数组（一条 update 循环算 next）；每 set 的 s1 流水
// 与控制寄存器整项化为另一个。对外端口与原三层层次版逐位等价。

class PosTable : public wolvicmod::Module {
public:
    struct PosEntryV {  // 一个表项的全部状态
        PosState state;
        uint64_t wakeupAddr = 0;
        bool wakeup = false;

        bool operator==(const PosEntryV&) const = default;
    };
    struct PosSetS1 {  // 每 set 的 s1 流水 + 控制寄存器
        bool allocValid = false;
        uint64_t allocAddr = 0;
        uint8_t allocChannel = 0;
        uint8_t allocWay = 0;
        uint8_t posRespWay = 0;
        bool lock = false;
        bool sleep = false;
        bool block = false;
        bool hnIdxValid = false;
        bool posRespValid = false;

        bool operator==(const PosSetS1&) const = default;
    };
    using EntryArr = std::array<PosEntryV, 64>;  // [set*16 + way]
    using SetArr = std::array<PosSetS1, 4>;
    using U32Arr4 = std::array<uint32_t, 4>;
    using BoolArr4 = std::array<bool, 4>;
    using U8Arr4 = std::array<uint8_t, 4>;

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

    REG(EntryArr, entries);
    REG(SetArr, s1);

    WIRE(U32Arr4, w_mat_tag_vec);
    WIRE(U32Arr4, w_free_vec);
    WIRE(BoolArr4, w_block_s0);
    WIRE(U32Arr4, w_free_vec2);
    WIRE(U8Arr4, w_repl_sel_way);
    WIRE(BoolArr4, w_req_pos_fire);
    // 静止门（perf-breakdown §23）：s1 / entries 两条 update 各一条
    WIRE(bool, w_s1_any);
    WIRE(bool, w_ent_any);

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

    // 合并 s3 流水状态（perf-breakdown：原 3 个独立 reg 同沿、同以 task_s2.valid
    // 为使能）。valid 为 RegNext(valid)；task/decList 为 RegEnable(bits / fstDec
    // 结果, valid)——并为一个 struct + 一条 update，fstDec 查表内联进 update。
    struct St {
        bool valid = false;  // RegNext(task_s2.valid)
        TaskS1 task;         // RegEnable(task_s2.bits, valid)
        DecList4 decList{};  // RegEnable(fstDec(task_s2), valid)

        bool operator==(const St&) const = default;
    };
    REG(St, st);
    // s3 译码组合链（原 6 条 wire assign：stateInst→secDec→GetDecRes→快路径判定，
    // 同读 st + resp_dir_s3），并为一条 struct assign。
    struct DecW {
        uint32_t stateInst = 0;   // stateInst_s3
        DecList4 decList{};       // secDec 结果
        uint32_t taskCode = 0;    // GetDecRes.task
        uint32_t cmtCode = 0;     // GetDecRes.cmt
        bool respCompData = false;
        bool cleanUnuseDb = false;

        bool operator==(const DecW&) const = default;
    };
    WIRE(DecW, w_dec);

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
    IN(Valid<HReqFlit>, rx_hpr);
    OUT(bool, rx_hpr_rdy);
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
    MOD(ReqToChiTask, hpr2task);
    using RxQT = zj::prefab::FastQueue<HReqFlit, 2, false>;
    MOD(RxQT, rx_q);
    MOD(RxQT, rx_hpr_q);
    using ReqTaskBufT = TaskBuffer<16>;  // nrReqTaskBuf(per dirBank)=16
    using HprTaskBufT = TaskBuffer<8>;   // nrHprTaskBuf(per dirBank)=8
    MOD(ReqTaskBufT, req_task_buf);
    MOD(HprTaskBufT, hpr_task_buf);
    MOD(Block, block);
    MOD(PosTable, pos_table);
    using S1PipeT = ValidPipe<TaskS1, 3>;  // readDirLatency-1=3
    MOD(S1PipeT, s1_pipe);
    MOD(FrontendDecode, decode);
    using FastRespQT = zj::prefab::FastQueue<RespFlit, 2, false>;
    MOD(FastRespQT, fast_resp_q);

    WIRE(bool, w_select_req);  // !hprBuf.s0.valid & !hprBuf.lockTask

    Frontend();
};

// ---------------- TaskBuffer 构造（模板，req=16/hpr=8 共用） ----------------

template <uint32_t N>
TaskBuffer<N>::TaskBuffer() {
    alloc_arb.clk = clk;
    s0_arb.clk = clk;

    // alloc 分配器：out_rdy = 各项空闲位
    w_alloc_rdy_all.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        BoolArr r{};
        for (uint32_t i = 0; i < kEntries; ++i) r[i] = entries[i].task.state == taskst::kFree;
        return r;
    };
    alloc_arb.in = chi_task_in;
    chi_task_in_rdy = alloc_arb.in_rdy;
    alloc_arb.out_rdy = w_alloc_rdy_all;

    // s0 出站：各项发射请求（kSend 且 nid==0）
    w_s0_in.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        S0InArr o{};
        for (uint32_t i = 0; i < kEntries; ++i) {
            ChiTask t;
            t.chi = entries[i].task.chi;
            t.addr = entries[i].task.addr;
            t.qos = entries[i].task.qos;
            o[i] = Valid<ChiTask>{entries[i].task.state == taskst::kSend && entries[i].nid == 0,
                                  t};
        }
        return o;
    };
    s0_arb.in = w_s0_in;
    // RTL 生成 SV 实证：arb 的 out.ready 直连 io_chiTask_s0_ready，与 hasLockReg
    // 无关——锁定期间仲裁器照样每拍 fire 并推进 vip 指针（仅输出被锁定项覆盖）。
    s0_arb.out_rdy = chi_task_s0_rdy;
    // lock = (kSend|kWait) & timeout
    w_lock_all.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        BoolArr l{};
        for (uint32_t i = 0; i < kEntries; ++i)
            l[i] = (entries[i].task.state == taskst::kSend ||
                    entries[i].task.state == taskst::kWait) &&
                   entries[i].timeout;
        return l;
    };
    has_lock_reg.update().on(posedge(clk)).reads(w_lock_all) = [](auto src) {
        auto [locks] = src;
        for (bool l : locks) {
            if (l) return true;
        }
        return false;
    };
    // lockIdx = 首个 lock 位，无 lock 时默认 N-1（chisel PriorityMux 无匹配取末值，
    // 生成 SV 实证：末分支为 {3'h7, ~lock14}）。锁定期间选中项即 lockIdx，即使
    // lockVec 已空（刚发射完）也选 N-1 项。
    chi_task_s0.assign().reads(has_lock_reg, w_lock_all, w_s0_in, s0_arb.out) = [](auto src) {
        auto [has_lock_reg, locks, s0_in, arb_out] = src;
        if (has_lock_reg) {
            uint32_t idx = kEntries - 1;
            for (uint32_t i = 0; i < kEntries; ++i)
                if (locks[i]) {
                    idx = i;
                    break;
                }
            return Valid<ChiTask>{s0_in[idx].valid, s0_in[idx].bits};
        }
        return arb_out;
    };
    lock_task = has_lock_reg;
    working.assign().reads(entries) = [](auto src) {
        auto [entries] = src;
        for (const auto& e : entries)
            if (e.task.state != taskst::kFree) return true;
        return false;
    };

    // N 项状态（一条 update 循环算 next；nid/retryNum/timeout/validD1 的
    // RegNext 语义全部读旧值）
    // 静止门（§23）：候选 = ∃非空闲项 || ∃validD1 || alloc fire。validD1 是
    // state 的一拍延迟且每拍无条件重赋（释放后下一拍才自清）——漏掉它会让
    // 末项释放后 validD1 永远滞留（rel/othRel 旁路写通道）。timeout 同理派生
    // 自 retryNum，但 retryNum 变化的拍该项必非空闲或 validD1 未清，候选已
    // 覆盖。其余写通道（nid/retryNum/状态机/wakeHit/sleep/retry）均以非空闲
    // 或 inFire 为前提。
    w_any.assign().reads(entries, alloc_arb.out) = [](auto src) {
        auto [entries, alloc_out] = src;
        for (uint32_t i = 0; i < kEntries; ++i)
            if (entries[i].task.state != taskst::kFree || entries[i].validD1 ||
                alloc_out[i].valid)
                return true;
        return false;
    };
    entries.update().on(posedge(clk)).en(w_any).reads(entries, alloc_arb.out, chi_task_in,
                                            chi_task_s0_rdy, has_lock_reg, w_lock_all,
                                            s0_arb.in_rdy, retry_s1, sleep_s1, wakeup) =
        [](auto src) {
            auto [entries, alloc_out, chi_task_in, s0_rdy_out, has_lock, locks, arb_rdy,
                  retry_s1, sleep_s1, wakeup] = src;
            EntryArr n = entries;
            // 旧值派生：lockIdx / valid / release / useAddr
            uint32_t lockIdx = kEntries - 1;
            for (uint32_t j = 0; j < kEntries; ++j)
                if (locks[j]) {
                    lockIdx = j;
                    break;
                }
            std::array<bool, kEntries> valid{}, rel{};
            std::array<uint64_t, kEntries> useA{};
            for (uint32_t j = 0; j < kEntries; ++j) {
                valid[j] = entries[j].task.state != taskst::kFree;
                rel[j] = entries[j].validD1 && entries[j].task.state == taskst::kFree;
                useA[j] = useAddr(entries[j].task.addr);
            }
            const uint64_t inAddr = useAddr(chi_task_in.bits.addr);
            const uint64_t wakeAddr = useAddr(wakeup.bits);
            for (uint32_t i = 0; i < kEntries; ++i) {
                const EntryV& cur = entries[i];
                EntryV& ne = n[i];
                const bool inFire = alloc_out[i].valid && cur.task.state == taskst::kFree;
                const bool s0Valid = cur.task.state == taskst::kSend && cur.nid == 0;
                const bool s0Rdy = has_lock ? (lockIdx == i && s0_rdy_out) : arb_rdy[i];
                const bool wakeHit = wakeup.valid && useA[i] == wakeAddr;
                // sort：initNid = 同址在途数；othRel = 同址有 release
                uint8_t initNid = 0;
                for (uint32_t j = 0; j < kEntries; ++j)
                    if (valid[j] && useA[j] == inAddr) ++initNid;
                bool othRel = false;
                for (uint32_t j = 0; j < kEntries; ++j)
                    if (rel[j] && useA[j] == useA[i]) {
                        othRel = true;
                        break;
                    }
                // nid
                if (inFire) {
                    ne.nid = initNid;
                } else if (cur.task.state != taskst::kFree && othRel) {
                    ne.nid = cur.nid > 0 ? cur.nid - 1 : 0;
                }
                // retryNum / timeout（timeout 读旧 retryNum）
                if (inFire) {
                    ne.retryNum = 0;
                } else if (cur.task.state == taskst::kWait && retry_s1 && cur.retryNum < 7) {
                    ne.retryNum = cur.retryNum + 1;
                }
                ne.timeout = cur.retryNum == 7;
                ne.validD1 = cur.task.state != taskst::kFree;
                // 状态机
                switch (cur.task.state) {
                    case taskst::kFree:
                        if (inFire) {
                            ne.task.state = taskst::kSend;
                            ne.task.chi = chi_task_in.bits.chi;
                            ne.task.addr = chi_task_in.bits.addr;
                            ne.task.qos = chi_task_in.bits.qos;
                        }
                        break;
                    case taskst::kSend:
                        if (s0Valid && s0Rdy) ne.task.state = taskst::kWait;
                        break;
                    case taskst::kWait:
                        if (wakeHit) {
                            ne.task.state = taskst::kSend;
                        } else if (sleep_s1) {
                            ne.task.state = taskst::kSleep;
                        } else if (retry_s1) {
                            ne.task.state = taskst::kSend;
                        } else {
                            ne.task.state = taskst::kFree;
                        }
                        break;
                    case taskst::kSleep:
                        if (wakeHit) ne.task.state = taskst::kSend;
                        break;
                    default: break;
                }
            }
            return n;
        };
}


}  // namespace zj::dj

