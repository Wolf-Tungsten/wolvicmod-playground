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
    IN(bool, clk_en);  // 门控时钟使能（横扫冻结，见 prefab/sram.h）
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

    // ---- 合并时序状态（perf：同沿同使能、读集高度重叠的离散 reg 并为 struct，
    // 一条 update；单消费的组合中转内联进 update lambda） ----
    // 移位流水：三条移位器同拍右移，req 载荷随移位器同拍前进，d0..d4 各阶段
    // 几乎都同时引用两者 → 并为一条 update。
    struct Sft {
        uint8_t read = 0, write = 0, repl = 0;  // 4bit：fire 从 bit3 进；d1=bit3..d4=bit0
        SftArr req{};                           // 原 req_sft

        bool operator==(const Sft&) const = default;
    };
    REG(Sft, sft);
    REG(bool, rst_done);
    // d2→d3 寄存（en = req(D2)）：同沿同使能。原 w_repl_way_d2/w_unuse_way_d2/
    // w_sel_is_using_d2 为本组单消费中转，内联进 update。
    struct D3 {
        uint16_t replMes = 0;
        uint16_t useWay = 0;
        uint8_t unuseWay = 0;
        uint8_t replWay = 0;
        bool selIsUsing = false;

        bool operator==(const D3&) const = default;
    };
    REG(D3, d3);
    // repl 写回前递（d1↔d4 匹配 → 下一拍 d2 用）：同由 w_match.d1d4 驱动，并为一条
    struct Bp {
        bool d1d4 = false;
        uint16_t mes = 0;

        bool operator==(const Bp&) const = default;
    };
    REG(Bp, bp);
    // d3→d4 寄存（en = req(D3)）：同沿同使能。原 w_resp_d3/w_new_repl_mes_d3
    // 组合链为本组单消费，内联进 update。
    struct D4 {
        bool readHit = false;
        uint16_t selWayOH = 0;
        uint16_t newReplMes = 0;
        DirResp resp{};

        bool operator==(const D4&) const = default;
    };
    REG(D4, d4);
    REG(LockArr, lock_tab);
    REG(LockArr, rsv_tab);  // 仅 sf 使用（llc 恒零，对齐 WireInit 0）

    // ---- d0 线网 ----
    WIRE(bool, w_wr_fire);
    WIRE(bool, w_read_d0);
    WIRE(bool, w_common_rdy);
    // 写类三分：同读 (w_wr_fire, write)，并为一条（w_wr_any ≡ w_wr_fire，内联消除）
    struct WrKind {
        bool hit = false, direct = false, nohit = false;

        bool operator==(const WrKind&) const = default;
    };
    WIRE(WrKind, w_wr_kind);
    // d0 端口仲裁载荷（repl_d0 > write > read 的 set/mask/meta 选择）：同源选择
    // 信号、同喂 meta/tag RAM 请求，并为一条
    struct WriD0 {
        uint16_t set = 0, mask = 0;
        DirMeta meta = 0;

        bool operator==(const WriD0&) const = default;
    };
    WIRE(WriD0, w_wri_d0);
    // meta/tag 请求 v/w：读集高度重叠（wr_kind/read_d0/repl_d0/rst_done），并为一条
    struct RamReq {
        bool metaV = false, metaW = false, tagV = false, tagW = false;

        bool operator==(const RamReq&) const = default;
    };
    WIRE(RamReq, w_ram_req);
    // meta 请求 fire 与三类流水记录（read/write/repl 入移位器）：同一组合链，并为一条
    struct Rec {
        bool fire = false, read = false, write = false, repl = false;

        bool operator==(const Rec&) const = default;
    };
    WIRE(Rec, w_rec);
    // d4 repl 写回请求 v/fire：fire 仅多一级 rdy 与，并为一条
    struct Wreq {
        bool v = false, fire = false;

        bool operator==(const Wreq&) const = default;
    };
    WIRE(Wreq, w_wreq);
    // ---- d2 线网 ----
    WIRE(uint16_t, w_set_d2);
    // lock/reservation 命中 way 位图：同读 w_set_d2、同喂 w_use_way_d2，并为一条
    struct LockRsv {
        uint16_t lock = 0, rsv = 0;

        bool operator==(const LockRsv&) const = default;
    };
    WIRE(LockRsv, w_lock_rsv_d2);
    WIRE(uint16_t, w_use_way_d2);
    WIRE(uint16_t, w_repl_mes_d2);
    // d2/d1 与 d4 写回的 set 匹配：同读 (req, w_wreq.fire)，并为一条
    struct Match {
        bool d2d4 = false, d1d4 = false;

        bool operator==(const Match&) const = default;
    };
    WIRE(Match, w_match);
    // ---- d3 线网 ----
    WIRE(uint16_t, w_set_d3);
    WIRE(uint16_t, w_hit_vec_d3);
    WIRE(uint16_t, w_invalid_vec_d3);
    // 命中/有空 invalid/读命中：vec→bool 同一组合链，并为一条
    struct HitD3 {
        bool hit = false, hasInvalid = false, readHit = false;

        bool operator==(const HitD3&) const = default;
    };
    WIRE(HitD3, w_hit_d3);
    // 选中 way 及其 one-hot：wayOH = 1<<way 单链派生，并为一条
    struct SelWay {
        uint8_t way = 0;
        uint16_t wayOH = 0;

        bool operator==(const SelWay&) const = default;
    };
    WIRE(SelWay, w_sel_way_d3);
    // 读 d3 / sf 待分配（reservation 置位条件）：pendAlloc 由 readD3 派生，并为一条
    struct PendD3 {
        bool read = false, pendAlloc = false;

        bool operator==(const PendD3&) const = default;
    };
    WIRE(PendD3, w_pend_d3);
    // ---- 锁/预留次态 ----
    WIRE(LockArr, w_lock_next);
    WIRE(LockArr, w_rsv_next);

    DirectoryBase() {
        meta_ram.clk = clk;
        tag_ram.clk = clk;
        repl_ram.clk = clk;
        meta_ram.clk_en = clk_en;
        tag_ram.clk_en = clk_en;
        repl_ram.clk_en = clk_en;
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
        w_common_rdy.assign().reads(rst_done, sft) = [](auto src) {
            auto [rst_done, sft] = src;
            const bool tagMetaReady = (((sft.read | sft.write) >> 3) & 1u) == 0;
            const bool replWillWrite = (sft.read & sft.repl) != 0;
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
        w_wr_kind.assign().reads(w_wr_fire, write) = [](auto src) {
            auto [w_wr_fire, write] = src;
            return WrKind{w_wr_fire && write.bits.hit && !write.bits.directAlloc,
                          w_wr_fire && write.bits.directAlloc,
                          w_wr_fire && !write.bits.hit && !write.bits.directAlloc};
        };

        // d0 端口仲裁：repl_d0(d4 分配写回) > write > read（write_rdy 已保证互斥）
        w_wri_d0.assign().reads(sft, d4, write, read) = [](auto src) {
            auto [sft, d4, write, read] = src;
            if (((sft.read & sft.repl) & 1u) != 0)
                return WriD0{static_cast<uint16_t>(setOf(sft.req[0].addr)), d4.selWayOH,
                             sft.req[0].meta};
            const uint16_t set =
                static_cast<uint16_t>(write.valid ? setOf(write.bits.addr) : setOf(read.bits.addr));
            return WriD0{set, write.bits.wayOH, write.bits.meta};
        };

        w_ram_req.assign().reads(w_wr_kind, w_wr_fire, w_read_d0, sft, rst_done) = [](auto src) {
            auto [k, w_wr_fire, w_read_d0, sft, rst_done] = src;
            const bool repl_d0 = ((sft.read & sft.repl) & 1u) != 0;
            // 原 w_wr_any ≡ w_wr_fire（fire 时三类必居其一）
            return RamReq{(w_wr_fire || w_read_d0 || repl_d0) && rst_done,
                          k.hit || k.direct || repl_d0,
                          (k.direct || k.nohit || w_read_d0 || repl_d0) && rst_done,
                          k.direct || repl_d0};
        };
        w_rec.assign().reads(w_ram_req, meta_ram.req_rdy, w_wr_kind, sft) = [](auto src) {
            auto [q, meta_req_rdy, k, sft] = src;
            const bool fire = q.metaV && meta_req_rdy;
            const bool repl_d0 = ((sft.read & sft.repl) & 1u) != 0;
            return Rec{fire, fire && !q.metaW, fire && q.metaW, fire && (k.nohit || repl_d0)};
        };

        meta_ram.req.assign().reads(w_ram_req, w_wri_d0) = [](auto src) {
            auto [q, w] = src;
            typename MetaRam::ReqBits b;
            b.write = q.metaW;
            b.addr = w.set;
            b.mask = w.mask;
            b.data.fill(w.meta);
            return Valid<typename MetaRam::ReqBits>{q.metaV, b};
        };
        tag_ram.req.assign().reads(w_ram_req, w_wri_d0, w_wr_kind, sft, write) = [](auto src) {
            auto [q, w, k, sft, write] = src;
            typename TagRam::ReqBits b;
            b.write = q.tagW;
            b.addr = w.set;
            b.mask = w.mask;  // direct→write.wayOH / repl_d0→selWayOHReg_d4，同源
            const uint32_t tag = k.direct ? static_cast<uint32_t>(tagOf(write.bits.addr))
                                          : static_cast<uint32_t>(tagOf(sft.req[0].addr));
            b.data.fill(tag);
            return Valid<typename TagRam::ReqBits>{q.tagV, b};
        };

        repl_ram.rreq.assign().reads(w_wr_fire, w_read_d0, rst_done, write, read) = [](auto src) {
            auto [w_wr_fire, w_read_d0, rst_done, write, read] = src;
            const uint32_t set = write.valid ? setOf(write.bits.addr) : setOf(read.bits.addr);
            return Valid<uint32_t>{(w_wr_fire || w_read_d0) && rst_done, set};
        };

        // d4：repl 写回（写类触 PLRU / 分配写回 / 读命中触 PLRU）
        w_wreq.assign().reads(sft, d4, repl_ram.wreq_rdy) = [](auto src) {
            auto [sft, d4, wreq_rdy] = src;
            const bool wriUpdRepl = ((sft.write >> 0) & 1u) != 0 && ((sft.repl >> 0) & 1u) == 0;
            const bool updTagMeta = ((sft.read & sft.repl) & 1u) != 0;
            const bool readHitUpd = ((sft.read >> 0) & 1u) != 0 && d4.readHit;
            const bool v = wriUpdRepl || updTagMeta || readHitUpd;
            return Wreq{v, v && wreq_rdy};
        };
        repl_ram.wreq.assign().reads(w_wreq, sft, d4) = [](auto src) {
            auto [wq, sft, d4] = src;
            typename ReplRam::WrBits b;
            b.addr = setOf(sft.req[0].addr);
            b.data[0] = d4.newReplMes;
            return Valid<typename ReplRam::WrBits>{wq.v, b};
        };

        // 复位完成：三个 SRAM ready 同拍为真后锁存
        rst_done.update().on(posedge(clk)).reads(rst_done, meta_ram.req_rdy, repl_ram.rreq_rdy,
                                                 repl_ram.wreq_rdy) = [](auto src) {
            auto [rst_done, meta_rdy, repl_r_rdy, repl_w_rdy] = src;
            return rst_done || (meta_rdy && repl_r_rdy && repl_w_rdy);
        };

        // 响应输出（d4）
        resp.assign().reads(sft, d4) = [](auto src) {
            auto [sft, d4] = src;
            return Valid<DirResp>{((sft.read >> 0) & 1u) != 0, d4.resp};
        };
    }

    // tag req 掩码与 meta 同源（direct→write.wayOH / repl_d0→selWayOHReg_d4）
    void registerD2() {
        w_set_d2.assign().reads(sft) = [](auto src) {
            auto [sft] = src;
            return setOf(sft.req[2].addr);
        };
        w_lock_rsv_d2.assign().reads(lock_tab, rsv_tab, w_set_d2) = [](auto src) {
            auto [lock_tab, rsv_tab, w_set_d2] = src;
            uint32_t lv = 0, rv = 0;
            for (const auto& e : lock_tab)
                if (e.valid && e.set == w_set_d2) lv |= (1u << e.way);
            for (const auto& e : rsv_tab)
                if (e.valid && e.set == w_set_d2) rv |= (1u << e.way);
            return LockRsv{static_cast<uint16_t>(lv), static_cast<uint16_t>(rv)};
        };
        w_use_way_d2.assign().reads(w_lock_rsv_d2, w_pend_d3, w_set_d3, w_set_d2,
                                    w_sel_way_d3) = [](auto src) {
            auto [lr, pend, w_set_d3, w_set_d2, sel] = src;
            const bool p = pend.pendAlloc && (w_set_d3 == w_set_d2);
            return static_cast<uint16_t>(lr.lock | lr.rsv | (p ? sel.wayOH : 0));
        };
        w_match.assign().reads(sft, w_set_d2, w_wreq) = [](auto src) {
            auto [sft, w_set_d2, wq] = src;
            return Match{wq.fire && (setOf(sft.req[0].addr) == w_set_d2),
                         wq.fire && (setOf(sft.req[0].addr) == setOf(sft.req[3].addr))};
        };
        w_repl_mes_d2.assign().reads(w_match, d4, bp, repl_ram.rresp) = [](auto src) {
            auto [m, d4, bp, repl_rresp] = src;
            if (m.d2d4) return d4.newReplMes;
            if (bp.d1d4) return bp.mes;
            return repl_rresp.bits.data[0];
        };
    }

    void registerD3() {
        w_set_d3.assign().reads(sft) = [](auto src) {
            auto [sft] = src;
            return setOf(sft.req[1].addr);
        };
        w_hit_vec_d3.assign().reads(tag_ram.resp, meta_ram.resp, sft) = [](auto src) {
            auto [tag_resp, meta_resp, sft] = src;
            const uint32_t tag = static_cast<uint32_t>(tagOf(sft.req[1].addr));
            uint32_t v = 0;
            for (uint32_t w = 0; w < kWays; ++w)
                if (tag_resp.bits.data[w] == tag && meta_resp.bits.data[w] != 0) v |= (1u << w);
            return static_cast<uint16_t>(v);
        };
        w_invalid_vec_d3.assign().reads(meta_ram.resp, d3) = [](auto src) {
            auto [meta_resp, d3] = src;
            uint32_t v = 0;
            for (uint32_t w = 0; w < kWays; ++w)
                if (meta_resp.bits.data[w] == 0 && ((d3.useWay >> w) & 1u) == 0) v |= (1u << w);
            return static_cast<uint16_t>(v);
        };
        w_hit_d3.assign().reads(w_hit_vec_d3, w_invalid_vec_d3, sft) = [](auto src) {
            auto [w_hit_vec_d3, w_invalid_vec_d3, sft] = src;
            const bool hit = w_hit_vec_d3 != 0;
            return HitD3{hit, w_invalid_vec_d3 != 0, ((sft.read >> 1) & 1u) != 0 && hit};
        };
        w_sel_way_d3.assign().reads(w_hit_d3, w_hit_vec_d3, w_invalid_vec_d3, d3) = [](auto src) {
            auto [h, w_hit_vec_d3, w_invalid_vec_d3, d3] = src;
            uint8_t way;
            if (h.hit) {
                way = priorityEnc(w_hit_vec_d3);
            } else if (h.hasInvalid) {
                way = priorityEnc(w_invalid_vec_d3);
            } else if (d3.selIsUsing) {
                way = d3.unuseWay;
            } else {
                way = d3.replWay;
            }
            return SelWay{way, static_cast<uint16_t>(1u << way)};
        };
        w_pend_d3.assign().reads(sft, w_hit_d3) = [](auto src) {
            auto [sft, h] = src;
            const bool rd = ((sft.read >> 1) & 1u) != 0 && ((sft.write >> 1) & 1u) == 0 &&
                            ((sft.repl >> 1) & 1u) == 0;
            return PendD3{rd, Cfg::kHasReservation && rd && !h.hit && h.hasInvalid};
        };
    }

    void registerLocks() {
        w_lock_next.assign().reads(lock_tab, unlock, dir_bank, sft, w_set_d3, w_sel_way_d3,
                                   w_pend_d3, w_hit_d3) = [](auto src) {
            auto [lock_tab, unlock, dir_bank, sft, w_set_d3, sel, pend, h] = src;
            const bool reqD3 = (((sft.read | sft.write) >> 1) & 1u) != 0;
            const bool readReplD3 = ((sft.read >> 1) & 1u) != 0 && ((sft.write >> 1) & 1u) == 0 &&
                                    ((sft.repl >> 1) & 1u) != 0;
            auto next = lock_tab;
            for (uint32_t i = 0; i < kPosSets; ++i) {
                for (uint32_t j = 0; j < kLockWays; ++j) {
                    const uint8_t hn = hnIdxOf(dir_bank, i, j);
                    auto& e = next[i * kLockWays + j];
                    const bool unlHit = unlock.valid && unlock.bits == hn;
                    const bool reqHit = reqD3 && sft.req[1].hnIdx == hn;
                    const bool setEvt = reqHit && ((pend.read && h.hit) || readReplD3);
                    if (unlHit) {
                        e.valid = false;
                    } else if (setEvt) {
                        // RTL 条件为 reqHit & (!oldLock & newLock)，其中 oldLock/newLock
                        // 来自事件类型编码（readHit/readRepl → b01），与实际锁态无关
                        // → 无条件覆盖（已锁项被重写；RTL 另有 HAssert 假定不发生）
                        e.valid = true;
                        e.set = w_set_d3;
                        e.way = sel.way;
                    }
                }
            }
            return next;
        };
        if constexpr (Cfg::kHasReservation) {
            w_rsv_next.assign().reads(rsv_tab, unlock, dir_bank, w_wr_kind, write, sft, w_set_d3,
                                      w_sel_way_d3, w_pend_d3) = [](auto src) {
                auto [rsv_tab, unlock, dir_bank, k, write, sft, w_set_d3, sel, pend] = src;
                auto next = rsv_tab;
                for (uint32_t i = 0; i < kPosSets; ++i) {
                    for (uint32_t j = 0; j < kLockWays; ++j) {
                        const uint8_t hn = hnIdxOf(dir_bank, i, j);
                        auto& e = next[i * kLockWays + j];
                        const bool clr = (k.direct && write.bits.hnIdx == hn) ||
                                         (unlock.valid && unlock.bits == hn);
                        const bool reserve = pend.pendAlloc && sft.req[1].hnIdx == hn;
                        if (clr) {
                            e.valid = false;
                        } else if (reserve) {
                            e.valid = true;
                            e.set = w_set_d3;
                            e.way = sel.way;
                        }
                    }
                }
                return next;
            };
            rsv_tab.update().on(posedge(clk)).reads(rsv_tab, w_rsv_next, w_pend_d3, w_wr_kind,
                                                    unlock) = [](auto src) {
                auto [rsv_tab, w_rsv_next, pend, k, unlock] = src;
                return (pend.pendAlloc || k.direct || unlock.valid) ? w_rsv_next : rsv_tab;
            };
        } else {
            w_rsv_next = rsv_tab;  // llc：恒零占位（RTL 为 WireInit 0）
        }
        lock_tab.update().on(posedge(clk)).reads(lock_tab, w_lock_next, sft,
                                                 unlock) = [](auto src) {
            auto [lock_tab, w_lock_next, sft, unlock] = src;
            const bool reqD3 = (((sft.read | sft.write) >> 1) & 1u) != 0;
            return (reqD3 || unlock.valid) ? w_lock_next : lock_tab;
        };
    }

    void registerState() {
        // 移位流水：三条移位器 + req 载荷同拍更新（原 4 条 update）
        sft.update().on(posedge(clk)).reads(sft, w_rec, w_wr_fire, w_read_d0, write,
                                            read) = [](auto src) {
            auto [sft, rec, w_wr_fire, w_read_d0, write, read] = src;
            Sft next;
            next.read = static_cast<uint8_t>((rec.read << 3) | (sft.read >> 1));
            next.write = static_cast<uint8_t>((rec.write << 3) | (sft.write >> 1));
            next.repl = static_cast<uint8_t>((rec.repl << 3) | (sft.repl >> 1));
            next.req = sft.req;
            if (w_wr_fire || w_read_d0) {
                next.req[3].addr = write.valid ? write.bits.addr : read.bits.addr;
                next.req[3].hnIdx = write.valid ? write.bits.hnIdx : read.bits.hnIdx;
                next.req[3].wriWayOH = write.valid ? write.bits.wayOH : 0;
                next.req[3].meta = write.valid ? write.bits.meta : 0;
            }
            if (((sft.read | sft.write) != 0) || w_wr_fire || w_read_d0)
                for (uint32_t i = 1; i < 4; ++i) next.req[i - 1] = next.req[i];
            return next;
        };
        // d2→d3（en = req(D2)）；replWay/unuseWay/selIsUsing 由 mes/useWay 单链
        // 派生（原 w_repl_way_d2/w_unuse_way_d2/w_sel_is_using_d2），内联
        d3.update().on(posedge(clk)).reads(sft, d3, w_repl_mes_d2, w_use_way_d2) = [](auto src) {
            auto [sft, d3, w_repl_mes_d2, w_use_way_d2] = src;
            const bool en = (((sft.read | sft.write) >> 2) & 1u) != 0;
            if (!en) return d3;
            const uint8_t replWay = static_cast<uint8_t>(plruReplaceWay(w_repl_mes_d2, kWays));
            const uint8_t unuseWay = priorityEnc(~w_use_way_d2 & 0xFFFFu);
            return D3{w_repl_mes_d2, w_use_way_d2, unuseWay, replWay,
                      ((w_use_way_d2 >> replWay) & 1u) != 0};
        };
        // 前递寄存
        bp.update().on(posedge(clk)).reads(w_match, d4, bp) = [](auto src) {
            auto [m, d4, bp] = src;
            return Bp{m.d1d4, m.d1d4 ? d4.newReplMes : bp.mes};
        };
        // d3→d4（en = req(D3)）；newReplMes/resp 组合链（原 w_new_repl_mes_d3/
        // w_resp_d3）仅在此被采样，内联
        d4.update().on(posedge(clk)).reads(sft, d4, d3, tag_ram.resp, meta_ram.resp, w_set_d3,
                                           w_sel_way_d3, w_hit_d3, cfg_bank_id,
                                           dir_bank) = [](auto src) {
            auto [sft, d4, d3, tag_resp, meta_resp, w_set_d3, sel, h, cfg_bank_id, dir_bank] = src;
            const bool en = (((sft.read | sft.write) >> 1) & 1u) != 0;
            if (!en) return d4;
            const bool wriUpdRepl = ((sft.write >> 1) & 1u) != 0 && ((sft.repl >> 1) & 1u) == 0;
            const uint32_t oh = wriUpdRepl ? sft.req[1].wriWayOH : sel.wayOH;
            DirResp r;
            r.addr = catAddr(cfg_bank_id, tag_resp.bits.data[sel.way], w_set_d3, kSetBits,
                             dir_bank);
            r.wayOH = sel.wayOH;
            r.meta = meta_resp.bits.data[sel.way];
            r.hnTxnID = sft.req[1].hnIdx & 0x7Fu;
            r.hit = h.hit;
            r.toRepl = ((sft.repl >> 1) & 1u) != 0;
            return D4{h.readHit, sel.wayOH,
                      static_cast<uint16_t>(plruNextState(d3.replMes, ohToUInt(oh), kWays)), r};
        };
    }
};

// ---------------- Directory（顶层组装） ----------------

class Directory : public wolvicmod::Module {
public:
    static constexpr uint32_t kDirBanks = 2;

    IN(bool, clk);
    IN(bool, clk_en);  // 门控时钟使能（横扫冻结），透传各 DirectoryBase
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
