#pragma once

// Directory / DirectoryBase：对齐 dongjiang/directory/{Directory,DirectoryBase}.scala，
// 语义提炼见 docs/dongjiang-semantics.md §3（流水阶段记号 d0..d4 与该文档一致）。
//
// DirectoryBase<Cfg>：单口 meta/tag SRAM（setup=1+latency=2+outputReg ⇒ d3 出数）、
// 双口 repl SRAM（latency=1+outputReg ⇒ d2 出数）、4 拍移位流水、
// lockTable（llc 15/sf 14 项 × posSets 4）与 reservationTable（仅 sf）。
// Cfg 为 elaboration 常量（llc: 8192 组/tag27/lock15；sf: 1024 组/tag30/lock14）。
//
// Directory：2 dirBank × (llc+sf) 组装（读联动、写按 dirBank 分发、
// rRespVec=!toRepl、wResp=toRepl 按 bank 序优先）。

#include <array>
#include <cstdint>

#include "model/dj/dj_types.h"
#include "prefab/sram.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::dj {

using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Valid;
using zj::prefab::DpSram;
using zj::prefab::SpSram;

// ---------------- elaboration 常量 ----------------

struct DirLlcCfg {
    static constexpr uint32_t kSets = 8192, kSetBits = 13, kTagBits = 27;
    static constexpr uint32_t kLockWays = 15;  // posWays-1
    static constexpr bool kHasReservation = false;
    static constexpr const char* kName = "llc";
};

struct DirSfCfg {
    static constexpr uint32_t kSets = 1024, kSetBits = 10, kTagBits = 30;
    static constexpr uint32_t kLockWays = 14;  // posWays-2
    static constexpr bool kHasReservation = true;
    static constexpr const char* kName = "sf";
};

// ---------------- DirectoryBase ----------------

template <class Cfg>
class DirectoryBase : public wolvicmod::Module {
public:
    static constexpr uint32_t kSets = Cfg::kSets;
    static constexpr uint32_t kSetBits = Cfg::kSetBits;
    static constexpr uint32_t kTagBits = Cfg::kTagBits;
    static constexpr uint32_t kLockWays = Cfg::kLockWays;
    static constexpr uint32_t kLocks = kPosSets * kLockWays;

    // tag/set 切片（useAddr 域）
    static constexpr uint32_t setOf(uint64_t addr) {
        return static_cast<uint32_t>((useAddr(addr) >> kDirBankBits) & ((1u << kSetBits) - 1u));
    }
    static constexpr uint64_t tagOf(uint64_t addr) {
        return useAddr(addr) >> (kDirBankBits + kSetBits);
    }

    IN(bool, clk);
    IN(uint8_t, cfg_bank_id);  // 1bit
    IN(uint8_t, dir_bank);     // 1bit
    IN(Valid<DirRdReq>, read);
    OUT(bool, read_rdy);
    IN(Valid<DirWrReq>, write);
    OUT(bool, write_rdy);
    OUT(Valid<DirResp>, resp);
    IN(Valid<uint8_t>, unlock);  // PackHnIdx

    // SRAM 模板（时序对齐 docs §3.7；prefab 注释已逐配置核对）
    using MetaRam = SpSram<DirMeta, kSets, kWays, 1, 2, false, true, true>;
    using TagRam = SpSram<uint32_t, kSets, kWays, 1, 2, false, true, false>;
    using ReplRam = DpSram<uint16_t, kSets, 1, true, 1, 1, false, true, true>;
    MOD(MetaRam, meta_ram);
    MOD(TagRam, tag_ram);
    MOD(ReplRam, repl_ram);

    struct LockEntry {
        bool valid = false;
        uint16_t set = 0;  // kSetBits
        uint8_t way = 0;   // 4bit

        bool operator==(const LockEntry&) const = default;
    };

    struct SftInfo {  // reqSftReg 项
        uint64_t addr = 0;
        uint16_t wriWayOH = 0;
        uint8_t hnIdx = 0;
        DirMeta meta = 0;

        bool operator==(const SftInfo&) const = default;
    };

    using SftArr = std::array<SftInfo, 4>;
    using LockArr = std::array<LockEntry, kLocks>;

    REG(uint8_t, sft_read);   // 4bit：fire 从 bit3 进，每拍右移；d1=bit3..d4=bit0
    REG(uint8_t, sft_write);
    REG(uint8_t, sft_repl);
    REG(SftArr, req_sft);
    REG(bool, rst_done);
    // d2→d3 寄存（en = req(D2)）
    REG(uint16_t, repl_mes_d3);
    REG(uint16_t, use_way_d3);
    REG(uint8_t, unuse_way_d3);
    REG(uint8_t, repl_way_d3);
    REG(bool, sel_is_using_d3);
    // repl 写回前递（d1↔d4 匹配 → 下一拍 d2 用）
    REG(bool, bp_d1_d4);
    REG(uint16_t, bp_mes_d4);
    // d3→d4 寄存（en = req(D3)）
    REG(bool, read_hit_d4);
    REG(uint16_t, sel_way_oh_d4);
    REG(uint16_t, new_repl_mes_d4);
    REG(DirResp, resp_d4);
    REG(LockArr, lock_tab);
    REG(LockArr, rsv_tab);  // 仅 sf 使用（llc 恒零，对齐 WireInit 0）

    // ---- d0 线网 ----
    WIRE(bool, w_wr_hit);
    WIRE(bool, w_wr_direct);
    WIRE(bool, w_wr_nohit);
    WIRE(bool, w_wr_any);
    WIRE(bool, w_wr_fire);
    WIRE(bool, w_read_d0);
    WIRE(bool, w_common_rdy);
    WIRE(uint16_t, w_req_set_d0);
    WIRE(uint16_t, w_wri_mask_d0);
    WIRE(DirMeta, w_wri_meta_d0);
    WIRE(bool, w_meta_req_v);
    WIRE(bool, w_meta_req_w);
    WIRE(bool, w_tag_req_v);
    WIRE(bool, w_tag_req_w);
    WIRE(bool, w_meta_fire);
    WIRE(bool, w_rec_read);
    WIRE(bool, w_rec_write);
    WIRE(bool, w_rec_repl);
    WIRE(bool, w_wreq_v);
    WIRE(bool, w_wreq_fire);
    // ---- d2 线网 ----
    WIRE(uint16_t, w_set_d2);
    WIRE(uint16_t, w_lock_way_d2);
    WIRE(uint16_t, w_rsv_way_d2);
    WIRE(uint16_t, w_use_way_d2);
    WIRE(uint16_t, w_repl_mes_d2);
    WIRE(uint8_t, w_repl_way_d2);
    WIRE(uint8_t, w_unuse_way_d2);
    WIRE(bool, w_sel_is_using_d2);
    WIRE(bool, w_match_d2_d4);
    WIRE(bool, w_match_d1_d4);
    // ---- d3 线网 ----
    WIRE(uint16_t, w_set_d3);
    WIRE(uint16_t, w_hit_vec_d3);
    WIRE(uint16_t, w_invalid_vec_d3);
    WIRE(bool, w_hit_d3);
    WIRE(bool, w_has_invalid_d3);
    WIRE(bool, w_read_hit_d3);
    WIRE(uint8_t, w_sel_way_d3);
    WIRE(uint16_t, w_sel_way_oh_d3);
    WIRE(uint16_t, w_new_repl_mes_d3);
    WIRE(DirResp, w_resp_d3);
    WIRE(bool, w_read_d3);
    WIRE(bool, w_pend_alloc_d3);
    // ---- 锁/预留次态 ----
    WIRE(LockArr, w_lock_next);
    WIRE(LockArr, w_rsv_next);

    DirectoryBase() {
        meta_ram.clk = clk;
        tag_ram.clk = clk;
        repl_ram.clk = clk;
        registerD0();
        registerD2();
        registerD3();
        registerLocks();
        registerState();
    }

private:
    // 首个置位位索引（chisel PriorityEncoder 语义；0 输入 → 0）
    static constexpr uint8_t priorityEnc(uint32_t v) {
        if (v == 0) return 0;
        uint8_t idx = 0;
        while (((v >> idx) & 1u) == 0) ++idx;
        return idx;
    }
    static constexpr uint8_t ohToUInt(uint32_t oh) { return priorityEnc(oh); }

    void registerD0() {
        // 就绪：resetDone & tagMetaReady(无请求在 d1) & !replWillWrite；read 再让 write
        w_common_rdy.assign().reads(rst_done, sft_read, sft_write, sft_repl) = [](auto src) {
            auto [rst_done, sft_read, sft_write, sft_repl] = src;
            const bool tagMetaReady = (((sft_read | sft_write) >> 3) & 1u) == 0;
            const bool replWillWrite = (sft_read & sft_repl) != 0;
            return rst_done && tagMetaReady && !replWillWrite;
        };
        read_rdy.assign().reads(w_common_rdy, write) = [](auto src) {
            auto [w_common_rdy, write] = src;
            return w_common_rdy && !write.valid;
        };
        write_rdy.assign().reads(w_common_rdy) = [](auto src) {
            auto [w_common_rdy] = src;
            return w_common_rdy;
        };
        w_wr_fire.assign().reads(write, write_rdy) = [](auto src) {
            auto [write, write_rdy] = src;
            return write.valid && write_rdy;
        };
        w_read_d0.assign().reads(read, read_rdy) = [](auto src) {
            auto [read, read_rdy] = src;
            return read.valid && read_rdy;
        };
        w_wr_hit.assign().reads(w_wr_fire, write) = [](auto src) {
            auto [w_wr_fire, write] = src;
            return w_wr_fire && write.bits.hit && !write.bits.directAlloc;
        };
        w_wr_direct.assign().reads(w_wr_fire, write) = [](auto src) {
            auto [w_wr_fire, write] = src;
            return w_wr_fire && write.bits.directAlloc;
        };
        w_wr_nohit.assign().reads(w_wr_fire, write) = [](auto src) {
            auto [w_wr_fire, write] = src;
            return w_wr_fire && !write.bits.hit && !write.bits.directAlloc;
        };
        w_wr_any.assign().reads(w_wr_hit, w_wr_direct, w_wr_nohit) = [](auto src) {
            auto [w_wr_hit, w_wr_direct, w_wr_nohit] = src;
            return w_wr_hit || w_wr_direct || w_wr_nohit;
        };

        // d0 端口仲裁：repl_d0(d4 分配写回) > write > read（write_rdy 已保证互斥）
        w_req_set_d0.assign().reads(sft_read, sft_repl, req_sft, write, read) = [](auto src) {
            auto [sft_read, sft_repl, req_sft, write, read] = src;
            if (((sft_read & sft_repl) & 1u) != 0) return setOf(req_sft[0].addr);
            return write.valid ? setOf(write.bits.addr) : setOf(read.bits.addr);
        };
        w_wri_mask_d0.assign().reads(sft_read, sft_repl, sel_way_oh_d4, write) = [](auto src) {
            auto [sft_read, sft_repl, sel_way_oh_d4, write] = src;
            return ((sft_read & sft_repl) & 1u) != 0 ? sel_way_oh_d4 : write.bits.wayOH;
        };
        w_wri_meta_d0.assign().reads(sft_read, sft_repl, req_sft, write) = [](auto src) {
            auto [sft_read, sft_repl, req_sft, write] = src;
            return ((sft_read & sft_repl) & 1u) != 0 ? req_sft[0].meta : write.bits.meta;
        };

        w_meta_req_v.assign().reads(w_wr_any, w_read_d0, sft_read, sft_repl, rst_done) = [](auto src) {
            auto [w_wr_any, w_read_d0, sft_read, sft_repl, rst_done] = src;
            const bool repl_d0 = ((sft_read & sft_repl) & 1u) != 0;
            return (w_wr_any || w_read_d0 || repl_d0) && rst_done;
        };
        w_meta_req_w.assign().reads(w_wr_hit, w_wr_direct, sft_read, sft_repl) = [](auto src) {
            auto [w_wr_hit, w_wr_direct, sft_read, sft_repl] = src;
            const bool repl_d0 = ((sft_read & sft_repl) & 1u) != 0;
            return w_wr_hit || w_wr_direct || repl_d0;
        };
        w_tag_req_v.assign().reads(w_wr_direct, w_wr_nohit, w_read_d0, sft_read, sft_repl, rst_done) =
            [](auto src) {
                auto [w_wr_direct, w_wr_nohit, w_read_d0, sft_read, sft_repl, rst_done] = src;
                const bool repl_d0 = ((sft_read & sft_repl) & 1u) != 0;
                return (w_wr_direct || w_wr_nohit || w_read_d0 || repl_d0) && rst_done;
            };
        w_tag_req_w.assign().reads(w_wr_direct, sft_read, sft_repl) = [](auto src) {
            auto [w_wr_direct, sft_read, sft_repl] = src;
            const bool repl_d0 = ((sft_read & sft_repl) & 1u) != 0;
            return w_wr_direct || repl_d0;
        };
        w_meta_fire.assign().reads(w_meta_req_v, meta_ram.req_rdy) = [](auto src) {
            auto [w_meta_req_v, meta_ram_req_rdy] = src;
            return w_meta_req_v && meta_ram_req_rdy;
        };
        w_rec_read.assign().reads(w_meta_fire, w_meta_req_w) = [](auto src) {
            auto [w_meta_fire, w_meta_req_w] = src;
            return w_meta_fire && !w_meta_req_w;
        };
        w_rec_write.assign().reads(w_meta_fire, w_meta_req_w) = [](auto src) {
            auto [w_meta_fire, w_meta_req_w] = src;
            return w_meta_fire && w_meta_req_w;
        };
        w_rec_repl.assign().reads(w_meta_fire, w_wr_nohit, sft_read, sft_repl) = [](auto src) {
            auto [w_meta_fire, w_wr_nohit, sft_read, sft_repl] = src;
            const bool repl_d0 = ((sft_read & sft_repl) & 1u) != 0;
            return w_meta_fire && (w_wr_nohit || repl_d0);
        };

        meta_ram.req.assign().reads(w_meta_req_v, w_meta_req_w, w_req_set_d0, w_wri_mask_d0,
                                    w_wri_meta_d0) = [](auto src) {
            auto [w_meta_req_v, w_meta_req_w, w_req_set_d0, w_wri_mask_d0, w_wri_meta_d0] = src;
            typename MetaRam::ReqBits b;
            b.write = w_meta_req_w;
            b.addr = w_req_set_d0;
            b.mask = w_wri_mask_d0;
            b.data.fill(w_wri_meta_d0);
            return Valid<typename MetaRam::ReqBits>{w_meta_req_v, b};
        };
        tag_ram.req.assign().reads(w_tag_req_v, w_tag_req_w, w_req_set_d0, w_wri_mask_d0,
                                   w_wr_direct, req_sft, write) = [](auto src) {
            auto [w_tag_req_v, w_tag_req_w, w_req_set_d0, w_wri_mask_d0, w_wr_direct, req_sft,
                  write] = src;
            typename TagRam::ReqBits b;
            b.write = w_tag_req_w;
            b.addr = w_req_set_d0;
            b.mask = w_wri_mask_d0;  // direct→write.wayOH / repl_d0→selWayOHReg_d4，同源
            const uint32_t tag = w_wr_direct ? static_cast<uint32_t>(tagOf(write.bits.addr))
                                             : static_cast<uint32_t>(tagOf(req_sft[0].addr));
            b.data.fill(tag);
            return Valid<typename TagRam::ReqBits>{w_tag_req_v, b};
        };

        repl_ram.rreq.assign().reads(w_wr_any, w_read_d0, rst_done, write, read) = [](auto src) {
            auto [w_wr_any, w_read_d0, rst_done, write, read] = src;
            const uint32_t set = write.valid ? setOf(write.bits.addr) : setOf(read.bits.addr);
            return Valid<uint32_t>{(w_wr_any || w_read_d0) && rst_done, set};
        };

        // d4：repl 写回（写类触 PLRU / 分配写回 / 读命中触 PLRU）
        w_wreq_v.assign().reads(sft_read, sft_write, sft_repl, read_hit_d4) = [](auto src) {
            auto [sft_read, sft_write, sft_repl, read_hit_d4] = src;
            const bool wriUpdRepl = ((sft_write >> 0) & 1u) != 0 && ((sft_repl >> 0) & 1u) == 0;
            const bool updTagMeta = ((sft_read & sft_repl) & 1u) != 0;
            const bool readHitUpd = ((sft_read >> 0) & 1u) != 0 && read_hit_d4;
            return wriUpdRepl || updTagMeta || readHitUpd;
        };
        w_wreq_fire.assign().reads(w_wreq_v, repl_ram.wreq_rdy) = [](auto src) {
            auto [w_wreq_v, repl_ram_wreq_rdy] = src;
            return w_wreq_v && repl_ram_wreq_rdy;
        };
        repl_ram.wreq.assign().reads(w_wreq_v, req_sft, new_repl_mes_d4) = [](auto src) {
            auto [w_wreq_v, req_sft, new_repl_mes_d4] = src;
            typename ReplRam::WrBits b;
            b.addr = setOf(req_sft[0].addr);
            b.data[0] = new_repl_mes_d4;
            return Valid<typename ReplRam::WrBits>{w_wreq_v, b};
        };

        // 复位完成：三个 SRAM ready 同拍为真后锁存
        rst_done.update().on(posedge(clk)).reads(rst_done, meta_ram.req_rdy, repl_ram.rreq_rdy,
                                                 repl_ram.wreq_rdy) = [](auto src) {
            auto [rst_done, meta_rdy, repl_r_rdy, repl_w_rdy] = src;
            return rst_done || (meta_rdy && repl_r_rdy && repl_w_rdy);
        };

        // 响应输出（d4）
        resp.assign().reads(sft_read, resp_d4) = [](auto src) {
            auto [sft_read, resp_d4] = src;
            return Valid<DirResp>{((sft_read >> 0) & 1u) != 0, resp_d4};
        };
    }

    // tag req 掩码与 meta 同源（direct→write.wayOH / repl_d0→selWayOHReg_d4）
    void registerD2() {
        w_set_d2.assign().reads(req_sft) = [](auto src) {
            auto [req_sft] = src;
            return setOf(req_sft[2].addr);
        };
        w_lock_way_d2.assign().reads(lock_tab, w_set_d2) = [](auto src) {
            auto [lock_tab, w_set_d2] = src;
            uint32_t v = 0;
            for (const auto& e : lock_tab)
                if (e.valid && e.set == w_set_d2) v |= (1u << e.way);
            return static_cast<uint16_t>(v);
        };
        w_rsv_way_d2.assign().reads(rsv_tab, w_set_d2) = [](auto src) {
            auto [rsv_tab, w_set_d2] = src;
            uint32_t v = 0;
            for (const auto& e : rsv_tab)
                if (e.valid && e.set == w_set_d2) v |= (1u << e.way);
            return static_cast<uint16_t>(v);
        };
        w_use_way_d2.assign().reads(w_lock_way_d2, w_rsv_way_d2, w_pend_alloc_d3, w_set_d3,
                                    w_set_d2, w_sel_way_oh_d3) = [](auto src) {
            auto [w_lock_way_d2, w_rsv_way_d2, w_pend_alloc_d3, w_set_d3, w_set_d2,
                  w_sel_way_oh_d3] = src;
            const bool pend = w_pend_alloc_d3 && (w_set_d3 == w_set_d2);
            return static_cast<uint16_t>(w_lock_way_d2 | w_rsv_way_d2 |
                                         (pend ? w_sel_way_oh_d3 : 0));
        };
        w_match_d2_d4.assign().reads(req_sft, w_set_d2, w_wreq_fire) = [](auto src) {
            auto [req_sft, w_set_d2, w_wreq_fire] = src;
            return w_wreq_fire && (setOf(req_sft[0].addr) == w_set_d2);
        };
        w_match_d1_d4.assign().reads(req_sft, w_wreq_fire) = [](auto src) {
            auto [req_sft, w_wreq_fire] = src;
            return w_wreq_fire && (setOf(req_sft[0].addr) == setOf(req_sft[3].addr));
        };
        w_repl_mes_d2.assign().reads(w_match_d2_d4, new_repl_mes_d4, bp_d1_d4, bp_mes_d4,
                                     repl_ram.rresp) = [](auto src) {
            auto [w_match_d2_d4, new_repl_mes_d4, bp_d1_d4, bp_mes_d4, repl_rresp] = src;
            if (w_match_d2_d4) return new_repl_mes_d4;
            if (bp_d1_d4) return bp_mes_d4;
            return repl_rresp.bits.data[0];
        };
        w_repl_way_d2.assign().reads(w_repl_mes_d2) = [](auto src) {
            auto [w_repl_mes_d2] = src;
            return static_cast<uint8_t>(plruReplaceWay(w_repl_mes_d2, kWays));
        };
        w_unuse_way_d2.assign().reads(w_use_way_d2) = [](auto src) {
            auto [w_use_way_d2] = src;
            return priorityEnc(~w_use_way_d2 & 0xFFFFu);
        };
        w_sel_is_using_d2.assign().reads(w_use_way_d2, w_repl_way_d2) = [](auto src) {
            auto [w_use_way_d2, w_repl_way_d2] = src;
            return ((w_use_way_d2 >> w_repl_way_d2) & 1u) != 0;
        };
    }

    void registerD3() {
        w_set_d3.assign().reads(req_sft) = [](auto src) {
            auto [req_sft] = src;
            return setOf(req_sft[1].addr);
        };
        w_hit_vec_d3.assign().reads(tag_ram.resp, meta_ram.resp, req_sft) = [](auto src) {
            auto [tag_resp, meta_resp, req_sft] = src;
            const uint32_t tag = static_cast<uint32_t>(tagOf(req_sft[1].addr));
            uint32_t v = 0;
            for (uint32_t w = 0; w < kWays; ++w)
                if (tag_resp.bits.data[w] == tag && meta_resp.bits.data[w] != 0) v |= (1u << w);
            return static_cast<uint16_t>(v);
        };
        w_hit_d3.assign().reads(w_hit_vec_d3) = [](auto src) {
            auto [w_hit_vec_d3] = src;
            return w_hit_vec_d3 != 0;
        };
        w_invalid_vec_d3.assign().reads(meta_ram.resp, use_way_d3) = [](auto src) {
            auto [meta_resp, use_way_d3] = src;
            uint32_t v = 0;
            for (uint32_t w = 0; w < kWays; ++w)
                if (meta_resp.bits.data[w] == 0 && ((use_way_d3 >> w) & 1u) == 0) v |= (1u << w);
            return static_cast<uint16_t>(v);
        };
        w_has_invalid_d3.assign().reads(w_invalid_vec_d3) = [](auto src) {
            auto [w_invalid_vec_d3] = src;
            return w_invalid_vec_d3 != 0;
        };
        w_read_hit_d3.assign().reads(sft_read, w_hit_d3) = [](auto src) {
            auto [sft_read, w_hit_d3] = src;
            return ((sft_read >> 1) & 1u) != 0 && w_hit_d3;
        };
        w_sel_way_d3.assign().reads(w_hit_d3, w_hit_vec_d3, w_has_invalid_d3, w_invalid_vec_d3,
                                    sel_is_using_d3, unuse_way_d3, repl_way_d3) = [](auto src) {
            auto [w_hit_d3, w_hit_vec_d3, w_has_invalid_d3, w_invalid_vec_d3, sel_is_using_d3,
                  unuse_way_d3, repl_way_d3] = src;
            if (w_hit_d3) return priorityEnc(w_hit_vec_d3);
            if (w_has_invalid_d3) return priorityEnc(w_invalid_vec_d3);
            if (sel_is_using_d3) return unuse_way_d3;
            return repl_way_d3;
        };
        w_sel_way_oh_d3.assign().reads(w_sel_way_d3) = [](auto src) {
            auto [w_sel_way_d3] = src;
            return static_cast<uint16_t>(1u << w_sel_way_d3);
        };
        w_new_repl_mes_d3.assign().reads(repl_mes_d3, sft_write, sft_repl, req_sft,
                                         w_sel_way_oh_d3) = [](auto src) {
            auto [repl_mes_d3, sft_write, sft_repl, req_sft, w_sel_way_oh_d3] = src;
            const bool wriUpdRepl = ((sft_write >> 1) & 1u) != 0 && ((sft_repl >> 1) & 1u) == 0;
            const uint32_t oh = wriUpdRepl ? req_sft[1].wriWayOH : w_sel_way_oh_d3;
            return static_cast<uint16_t>(plruNextState(repl_mes_d3, ohToUInt(oh), kWays));
        };
        w_resp_d3.assign().reads(tag_ram.resp, meta_ram.resp, req_sft, w_set_d3, w_sel_way_d3,
                                 w_sel_way_oh_d3, w_hit_d3, sft_repl, cfg_bank_id,
                                 dir_bank) = [](auto src) {
            auto [tag_resp, meta_resp, req_sft, w_set_d3, w_sel_way_d3, w_sel_way_oh_d3, w_hit_d3,
                  sft_repl, cfg_bank_id, dir_bank] = src;
            DirResp r;
            r.addr = catAddr(cfg_bank_id, tag_resp.bits.data[w_sel_way_d3], w_set_d3, kSetBits,
                             dir_bank);
            r.wayOH = w_sel_way_oh_d3;
            r.meta = meta_resp.bits.data[w_sel_way_d3];
            r.hnTxnID = req_sft[1].hnIdx & 0x7Fu;
            r.hit = w_hit_d3;
            r.toRepl = ((sft_repl >> 1) & 1u) != 0;
            return r;
        };
        w_read_d3.assign().reads(sft_read, sft_write, sft_repl) = [](auto src) {
            auto [sft_read, sft_write, sft_repl] = src;
            return ((sft_read >> 1) & 1u) != 0 && ((sft_write >> 1) & 1u) == 0 &&
                   ((sft_repl >> 1) & 1u) == 0;
        };
        w_pend_alloc_d3.assign().reads(w_read_d3, w_hit_d3, w_has_invalid_d3) = [](auto src) {
            auto [w_read_d3, w_hit_d3, w_has_invalid_d3] = src;
            return Cfg::kHasReservation && w_read_d3 && !w_hit_d3 && w_has_invalid_d3;
        };
    }

    void registerLocks() {
        w_lock_next.assign().reads(lock_tab, unlock, dir_bank, sft_read, sft_write, sft_repl,
                                   req_sft, w_set_d3, w_sel_way_d3, w_read_d3,
                                   w_hit_d3) = [](auto src) {
            auto [lock_tab, unlock, dir_bank, sft_read, sft_write, sft_repl, req_sft, w_set_d3,
                  w_sel_way_d3, w_read_d3, w_hit_d3] = src;
            const bool reqD3 = (((sft_read | sft_write) >> 1) & 1u) != 0;
            const bool readReplD3 = ((sft_read >> 1) & 1u) != 0 && ((sft_write >> 1) & 1u) == 0 &&
                                    ((sft_repl >> 1) & 1u) != 0;
            auto next = lock_tab;
            for (uint32_t i = 0; i < kPosSets; ++i) {
                for (uint32_t j = 0; j < kLockWays; ++j) {
                    const uint8_t hn = hnIdxOf(dir_bank, i, j);
                    auto& e = next[i * kLockWays + j];
                    const bool unlHit = unlock.valid && unlock.bits == hn;
                    const bool reqHit = reqD3 && req_sft[1].hnIdx == hn;
                    const bool setEvt = reqHit && ((w_read_d3 && w_hit_d3) || readReplD3);
                    if (unlHit) {
                        e.valid = false;
                    } else if (setEvt) {
                        // RTL 条件为 reqHit & (!oldLock & newLock)，其中 oldLock/newLock
                        // 来自事件类型编码（readHit/readRepl → b01），与实际锁态无关
                        // → 无条件覆盖（已锁项被重写；RTL 另有 HAssert 假定不发生）
                        e.valid = true;
                        e.set = w_set_d3;
                        e.way = w_sel_way_d3;
                    }
                }
            }
            return next;
        };
        if constexpr (Cfg::kHasReservation) {
            w_rsv_next.assign().reads(rsv_tab, unlock, dir_bank, w_wr_direct, write, req_sft,
                                      w_set_d3, w_sel_way_d3, w_pend_alloc_d3) = [](auto src) {
                auto [rsv_tab, unlock, dir_bank, w_wr_direct, write, req_sft, w_set_d3,
                      w_sel_way_d3, w_pend_alloc_d3] = src;
                auto next = rsv_tab;
                for (uint32_t i = 0; i < kPosSets; ++i) {
                    for (uint32_t j = 0; j < kLockWays; ++j) {
                        const uint8_t hn = hnIdxOf(dir_bank, i, j);
                        auto& e = next[i * kLockWays + j];
                        const bool clr = (w_wr_direct && write.bits.hnIdx == hn) ||
                                         (unlock.valid && unlock.bits == hn);
                        const bool reserve = w_pend_alloc_d3 && req_sft[1].hnIdx == hn;
                        if (clr) {
                            e.valid = false;
                        } else if (reserve) {
                            e.valid = true;
                            e.set = w_set_d3;
                            e.way = w_sel_way_d3;
                        }
                    }
                }
                return next;
            };
            rsv_tab.update().on(posedge(clk)).reads(rsv_tab, w_rsv_next, w_pend_alloc_d3,
                                                    w_wr_direct, unlock) = [](auto src) {
                auto [rsv_tab, w_rsv_next, w_pend_alloc_d3, w_wr_direct, unlock] = src;
                return (w_pend_alloc_d3 || w_wr_direct || unlock.valid) ? w_rsv_next : rsv_tab;
            };
        } else {
            w_rsv_next = rsv_tab;  // llc：恒零占位（RTL 为 WireInit 0）
        }
        lock_tab.update().on(posedge(clk)).reads(lock_tab, w_lock_next, sft_read, sft_write,
                                                 unlock) = [](auto src) {
            auto [lock_tab, w_lock_next, sft_read, sft_write, unlock] = src;
            const bool reqD3 = (((sft_read | sft_write) >> 1) & 1u) != 0;
            return (reqD3 || unlock.valid) ? w_lock_next : lock_tab;
        };
    }

    void registerState() {
        sft_read.update().on(posedge(clk)).reads(sft_read, w_rec_read) = [](auto src) {
            auto [sft_read, w_rec_read] = src;
            return static_cast<uint8_t>((w_rec_read << 3) | (sft_read >> 1));
        };
        sft_write.update().on(posedge(clk)).reads(sft_write, w_rec_write) = [](auto src) {
            auto [sft_write, w_rec_write] = src;
            return static_cast<uint8_t>((w_rec_write << 3) | (sft_write >> 1));
        };
        sft_repl.update().on(posedge(clk)).reads(sft_repl, w_rec_repl) = [](auto src) {
            auto [sft_repl, w_rec_repl] = src;
            return static_cast<uint8_t>((w_rec_repl << 3) | (sft_repl >> 1));
        };
        req_sft.update().on(posedge(clk)).reads(req_sft, sft_read, sft_write, w_wr_fire, w_read_d0,
                                                write, read) = [](auto src) {
            auto [req_sft, sft_read, sft_write, w_wr_fire, w_read_d0, write, read] = src;
            auto next = req_sft;
            if (w_wr_fire || w_read_d0) {
                next[3].addr = write.valid ? write.bits.addr : read.bits.addr;
                next[3].hnIdx = write.valid ? write.bits.hnIdx : read.bits.hnIdx;
                next[3].wriWayOH = write.valid ? write.bits.wayOH : 0;
                next[3].meta = write.valid ? write.bits.meta : 0;
            }
            if (((sft_read | sft_write) != 0) || w_wr_fire || w_read_d0)
                for (uint32_t i = 1; i < 4; ++i) next[i - 1] = next[i];
            return next;
        };
        // d2→d3
        const auto enD2 = [](uint8_t r, uint8_t w) { return (((r | w) >> 2) & 1u) != 0; };
        repl_mes_d3.update().on(posedge(clk)).reads(sft_read, sft_write, w_repl_mes_d2,
                                                    repl_mes_d3) = [=](auto src) {
            auto [sft_read, sft_write, w_repl_mes_d2, repl_mes_d3] = src;
            return enD2(sft_read, sft_write) ? w_repl_mes_d2 : repl_mes_d3;
        };
        use_way_d3.update().on(posedge(clk)).reads(sft_read, sft_write, w_use_way_d2, use_way_d3) =
            [=](auto src) {
                auto [sft_read, sft_write, w_use_way_d2, use_way_d3] = src;
                return enD2(sft_read, sft_write) ? w_use_way_d2 : use_way_d3;
            };
        unuse_way_d3.update().on(posedge(clk)).reads(sft_read, sft_write, w_unuse_way_d2,
                                                     unuse_way_d3) = [=](auto src) {
            auto [sft_read, sft_write, w_unuse_way_d2, unuse_way_d3] = src;
            return enD2(sft_read, sft_write) ? w_unuse_way_d2 : unuse_way_d3;
        };
        repl_way_d3.update().on(posedge(clk)).reads(sft_read, sft_write, w_repl_way_d2,
                                                    repl_way_d3) = [=](auto src) {
            auto [sft_read, sft_write, w_repl_way_d2, repl_way_d3] = src;
            return enD2(sft_read, sft_write) ? w_repl_way_d2 : repl_way_d3;
        };
        sel_is_using_d3.update().on(posedge(clk)).reads(sft_read, sft_write, w_sel_is_using_d2,
                                                        sel_is_using_d3) = [=](auto src) {
            auto [sft_read, sft_write, w_sel_is_using_d2, sel_is_using_d3] = src;
            return enD2(sft_read, sft_write) ? w_sel_is_using_d2 : sel_is_using_d3;
        };
        // 前递寄存
        bp_d1_d4.update().on(posedge(clk)).reads(w_match_d1_d4) = [](auto src) {
            auto [w_match_d1_d4] = src;
            return w_match_d1_d4;
        };
        bp_mes_d4.update().on(posedge(clk)).reads(w_match_d1_d4, new_repl_mes_d4, bp_mes_d4) =
            [](auto src) {
                auto [w_match_d1_d4, new_repl_mes_d4, bp_mes_d4] = src;
                return w_match_d1_d4 ? new_repl_mes_d4 : bp_mes_d4;
            };
        // d3→d4
        const auto enD3 = [](uint8_t r, uint8_t w) { return (((r | w) >> 1) & 1u) != 0; };
        read_hit_d4.update().on(posedge(clk)).reads(sft_read, sft_write, w_read_hit_d3,
                                                    read_hit_d4) = [=](auto src) {
            auto [sft_read, sft_write, w_read_hit_d3, read_hit_d4] = src;
            return enD3(sft_read, sft_write) ? w_read_hit_d3 : read_hit_d4;
        };
        sel_way_oh_d4.update().on(posedge(clk)).reads(sft_read, sft_write, w_sel_way_oh_d3,
                                                      sel_way_oh_d4) = [=](auto src) {
            auto [sft_read, sft_write, w_sel_way_oh_d3, sel_way_oh_d4] = src;
            return enD3(sft_read, sft_write) ? w_sel_way_oh_d3 : sel_way_oh_d4;
        };
        new_repl_mes_d4.update().on(posedge(clk)).reads(sft_read, sft_write, w_new_repl_mes_d3,
                                                        new_repl_mes_d4) = [=](auto src) {
            auto [sft_read, sft_write, w_new_repl_mes_d3, new_repl_mes_d4] = src;
            return enD3(sft_read, sft_write) ? w_new_repl_mes_d3 : new_repl_mes_d4;
        };
        resp_d4.update().on(posedge(clk)).reads(sft_read, sft_write, w_resp_d3, resp_d4) =
            [=](auto src) {
                auto [sft_read, sft_write, w_resp_d3, resp_d4] = src;
                return enD3(sft_read, sft_write) ? w_resp_d3 : resp_d4;
            };
    }
};

// ---------------- Directory（顶层组装） ----------------

class Directory : public wolvicmod::Module {
public:
    static constexpr uint32_t kDirBanks = 2;

    IN(bool, clk);
    IN(uint8_t, cfg_bank_id);  // 1bit
    // 每 dirBank 一路读口
    IN(Valid<DirRdReq>, read_0);
    OUT(bool, read_0_rdy);
    IN(Valid<DirRdReq>, read_1);
    OUT(bool, read_1_rdy);
    OUT(Valid<DirMsg>, rresp_0);
    OUT(Valid<DirMsg>, rresp_1);
    // 写口（llc/sf 各自 Valid）
    IN(Valid<DirWrBoth>, write);
    OUT(bool, write_rdy);
    // 替换响应（toRepl 的 DirectoryBase resp，按 bank 序优先）
    OUT(Valid<DirResp>, wresp_llc);
    OUT(Valid<DirResp>, wresp_sf);
    IN(Valid<uint8_t>, unlock);

    using LlcDir = DirectoryBase<DirLlcCfg>;
    using SfDir = DirectoryBase<DirSfCfg>;
    MOD_ARRAY(LlcDir, kDirBanks, llcs);
    MOD_ARRAY(SfDir, kDirBanks, sfs);

    WIRE(bool, w_write_fire);

    Directory();
};

}  // namespace zj::dj
