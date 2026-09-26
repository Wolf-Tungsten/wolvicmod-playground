// Directory 对拍 harness：wolvicmod Directory vs refgen RTL Directory
// （dongjiang/directory 真实源码，kunminghu-v3 DefaultConfig 单核 32MB 配置）。
// 激励：小地址池（每 bank 4 set × 24 tag，重竞争）随机读/写（hit/directAlloc/
// wriNoHit）/unlock，密度 100/50/10 三段轮换；覆盖复位横扫窗口（前 ~8200 拍
// read/write_rdy 对齐比对）、lock/reservation 挤占、PLRU 替换、读联动。
// 每拍比对：read_0/1_rdy、write_rdy、rresp_0/1（valid+wayOH/hit/meta）、
// wresp_llc/sf（valid+addr/wayOH/hit/meta/hnTxnID）。

#include <bitset>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

#include "VDirectory.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include "model/dj/directory.h"

using namespace wolvicmod;
using namespace zj::dj;

namespace {

// DIR_TRACE_START/END 环境变量：在窗口内逐拍打印两侧输出与 dut 内部选路状态
uint64_t traceStart() {
    const char* s = std::getenv("DIR_TRACE_START");
    return s ? std::strtoull(s, nullptr, 10) : UINT64_MAX;
}
uint64_t traceEnd() {
    const char* s = std::getenv("DIR_TRACE_END");
    return s ? std::strtoull(s, nullptr, 10) : 0;
}

template <class Cfg>
void dumpBase(const char* tag, const DirectoryBase<Cfg>& b, uint64_t c) {
    std::cout << "    " << tag << " cyc=" << c << " sft(r="
              << std::bitset<4>(b.sft_read.get()) << ",w=" << std::bitset<4>(b.sft_write.get())
              << ",p=" << std::bitset<4>(b.sft_repl.get()) << ") set_d3=0x" << std::hex
              << b.w_set_d3.get() << " hit_vec=0x" << b.w_hit_vec_d3.get() << " inv_vec=0x"
              << b.w_invalid_vec_d3.get() << " use_d2=0x" << b.w_use_way_d2.get() << " use_d3=0x"
              << b.use_way_d3.get() << " mes_d2=0x" << b.w_repl_mes_d2.get() << " mes_d3=0x"
              << b.repl_mes_d3.get() << " sel=0x" << (uint32_t)b.w_sel_way_d3.get() << std::dec
              << "\n";
    std::cout << "      locks:";
    for (uint32_t i = 0; i < DirectoryBase<Cfg>::kLocks; ++i) {
        const auto& e = b.lock_tab.get()[i];
        if (e.valid)
            std::cout << " [" << (int)i << "]s0x" << std::hex << e.set << "w"
                      << (uint32_t)e.way << std::dec;
    }
    if constexpr (Cfg::kHasReservation) {
        std::cout << "  rsv:";
        for (uint32_t i = 0; i < DirectoryBase<Cfg>::kLocks; ++i) {
            const auto& e = b.rsv_tab.get()[i];
            if (e.valid)
                std::cout << " [" << (int)i << "]s0x" << std::hex << e.set << "w"
                          << (uint32_t)e.way << std::dec;
        }
    }
    std::cout << "\n";
}

uint8_t randHnIdx(std::mt19937& rng) {
    return hnIdxOf(rng() & 1, rng() & 3, rng() & 15);
}

// 小地址池：bank 位恒 0；tag 24 个 × 每 bank 4 个 llc set → 高冲突
uint64_t randAddr(std::mt19937& rng) {
    return catAddr(0, rng() % 24, rng() % 4, DirLlcCfg::kSetBits, rng() & 1);
}

DirWrReq randWr(std::mt19937& rng, uint64_t addr, bool llc) {
    const uint32_t kind = rng() % 3;  // hit / wriNoHit / directAlloc
    DirWrReq w;
    w.addr = addr;
    w.wayOH = static_cast<uint16_t>(1u << (rng() % 16));
    w.meta = llc ? static_cast<DirMeta>(1 + rng() % 3) : 1;  // 恒 valid
    w.hnIdx = randHnIdx(rng);
    w.hit = (kind == 0);
    w.directAlloc = (kind == 2);
    return w;
}

void setRefRead(VDirectory& ref, int b, const Valid<DirRdReq>& r) {
    if (b == 0) {
        ref.io_readVec_0_valid = r.valid;
        ref.io_readVec_0_bits_addr = r.bits.addr;
        ref.io_readVec_0_bits_hnIdx_dirBank = hnIdxDirBank(r.bits.hnIdx);
        ref.io_readVec_0_bits_hnIdx_pos_set = hnIdxPosSet(r.bits.hnIdx);
        ref.io_readVec_0_bits_hnIdx_pos_way = hnIdxPosWay(r.bits.hnIdx);
    } else {
        ref.io_readVec_1_valid = r.valid;
        ref.io_readVec_1_bits_addr = r.bits.addr;
        ref.io_readVec_1_bits_hnIdx_dirBank = hnIdxDirBank(r.bits.hnIdx);
        ref.io_readVec_1_bits_hnIdx_pos_set = hnIdxPosSet(r.bits.hnIdx);
        ref.io_readVec_1_bits_hnIdx_pos_way = hnIdxPosWay(r.bits.hnIdx);
    }
}

void setRefWrite(VDirectory& ref, const Valid<DirWrBoth>& w) {
    ref.io_write_valid = w.valid;
    ref.io_write_bits_llc_valid = w.bits.llcValid;
    ref.io_write_bits_llc_bits_addr = w.bits.llc.addr;
    ref.io_write_bits_llc_bits_wayOH = w.bits.llc.wayOH;
    ref.io_write_bits_llc_bits_hit = w.bits.llc.hit;
    ref.io_write_bits_llc_bits_metaVec_0_state = w.bits.llc.meta;
    ref.io_write_bits_llc_bits_hnIdx_dirBank = hnIdxDirBank(w.bits.llc.hnIdx);
    ref.io_write_bits_llc_bits_hnIdx_pos_set = hnIdxPosSet(w.bits.llc.hnIdx);
    ref.io_write_bits_llc_bits_hnIdx_pos_way = hnIdxPosWay(w.bits.llc.hnIdx);
    ref.io_write_bits_llc_bits_directAlloc = w.bits.llc.directAlloc;
    ref.io_write_bits_sf_valid = w.bits.sfValid;
    ref.io_write_bits_sf_bits_addr = w.bits.sf.addr;
    ref.io_write_bits_sf_bits_wayOH = w.bits.sf.wayOH;
    ref.io_write_bits_sf_bits_hit = w.bits.sf.hit;
    ref.io_write_bits_sf_bits_metaVec_0_state = w.bits.sf.meta;
    ref.io_write_bits_sf_bits_hnIdx_dirBank = hnIdxDirBank(w.bits.sf.hnIdx);
    ref.io_write_bits_sf_bits_hnIdx_pos_set = hnIdxPosSet(w.bits.sf.hnIdx);
    ref.io_write_bits_sf_bits_hnIdx_pos_way = hnIdxPosWay(w.bits.sf.hnIdx);
    ref.io_write_bits_sf_bits_directAlloc = w.bits.sf.directAlloc;
}

void setRefUnlock(VDirectory& ref, const Valid<uint8_t>& u) {
    ref.io_unlock_valid = u.valid;
    ref.io_unlock_bits_hnIdx_dirBank = hnIdxDirBank(u.bits);
    ref.io_unlock_bits_hnIdx_pos_set = hnIdxPosSet(u.bits);
    ref.io_unlock_bits_hnIdx_pos_way = hnIdxPosWay(u.bits);
}

uint64_t cosimDir(uint32_t seed, uint64_t cycles) {
    VDirectory ref;
    Directory dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;

    cosim::resetRef(ref, [&] {
        setRefRead(ref, 0, {false, {}});
        setRefRead(ref, 1, {false, {}});
        setRefWrite(ref, {false, {}});
        setRefUnlock(ref, {false, 0});
        ref.io_config_ci = 0;
        ref.io_config_closeLLC = 0;
        ref.io_config_bankId = 0;
    });
    dut.cfg_bank_id.set(0);
    dut.read_0.set({false, {}});
    dut.read_1.set({false, {}});
    dut.write.set({false, {}});
    dut.unlock.set({false, 0});
    dut.clk.set(0);
    dut.eval();

    // 当前挂起的激励（Decoupled 保持到 fire）
    Valid<DirRdReq> rd0{false, {}}, rd1{false, {}};
    Valid<DirWrBoth> wr{false, {}};
    for (uint64_t c = 0; c < cycles; ++c) {
        const uint32_t pct = cosim::densityAt(c, cycles);
        if (!rd0.valid && cosim::roll(rng, pct / 2)) rd0 = {true, {randAddr(rng), randHnIdx(rng)}};
        if (!rd1.valid && cosim::roll(rng, pct / 2)) rd1 = {true, {randAddr(rng), randHnIdx(rng)}};
        if (!wr.valid && cosim::roll(rng, pct / 3)) {
            DirWrBoth b;
            b.llcValid = cosim::roll(rng, 70);
            b.sfValid = cosim::roll(rng, 70);
            if (!b.llcValid && !b.sfValid) b.llcValid = true;
            b.llc = randWr(rng, randAddr(rng), true);
            b.sf = randWr(rng, randAddr(rng), false);
            wr = {true, b};
        }
        Valid<uint8_t> unl{cosim::roll(rng, 10), randHnIdx(rng)};

        dut.read_0.set(rd0);
        dut.read_1.set(rd1);
        dut.write.set(wr);
        dut.unlock.set(unl);
        setRefRead(ref, 0, rd0);
        setRefRead(ref, 1, rd1);
        setRefWrite(ref, wr);
        setRefUnlock(ref, unl);
        rp.push("cyc=" + std::to_string(c) + " rd0(v=" + std::to_string(rd0.valid) +
                ",a=0x" + std::to_string(rd0.bits.addr) + ") wr(v=" + std::to_string(wr.valid) +
                ") unl(v=" + std::to_string(unl.valid) + ")");

        cosim::phaseLow(ref, dut);

        if (c >= traceStart() && c <= traceEnd()) {
            std::cout << "  [trace] cyc=" << c << " ref: rresp(v=" << (int)ref.io_rRespVec_0_valid
                      << "," << (int)ref.io_rRespVec_1_valid << ") wresp_llc(v="
                      << (int)ref.io_wResp_llc_valid << ",way=0x" << std::hex
                      << ref.io_wResp_llc_bits_wayOH << ") wresp_sf(v="
                      << (int)ref.io_wResp_sf_valid << ",way=0x" << ref.io_wResp_sf_bits_wayOH
                      << ")" << std::dec << " dut: rresp(v=" << (int)dut.rresp_0.get().valid
                      << "," << (int)dut.rresp_1.get().valid << ")\n";
            dumpBase("llc0", dut.llcs[0], c);
            dumpBase("sf0", dut.sfs[0], c);
            dumpBase("llc1", dut.llcs[1], c);
            dumpBase("sf1", dut.sfs[1], c);
        }

        // ---- 比对 ----
        cosim::check(st, "dir", "Directory", seed, c, "read_0_rdy", ref.io_readVec_0_ready,
                     dut.read_0_rdy.get(), rp);
        cosim::check(st, "dir", "Directory", seed, c, "read_1_rdy", ref.io_readVec_1_ready,
                     dut.read_1_rdy.get(), rp);
        cosim::check(st, "dir", "Directory", seed, c, "write_rdy", ref.io_write_ready,
                     dut.write_rdy.get(), rp);
        for (int b = 0; b < 2; ++b) {
            const auto rvalid = b == 0 ? ref.io_rRespVec_0_valid : ref.io_rRespVec_1_valid;
            const auto dvalid = b == 0 ? dut.rresp_0.get().valid : dut.rresp_1.get().valid;
            cosim::check(st, "dir", "Directory", seed, c, cosim::lanePort("rresp_valid", b).c_str(),
                         rvalid, dvalid, rp);
            if (rvalid && dvalid) {
                const auto dm = b == 0 ? dut.rresp_0.get().bits : dut.rresp_1.get().bits;
                const uint32_t rLlcWay = b == 0 ? ref.io_rRespVec_0_bits_llc_wayOH
                                                : ref.io_rRespVec_1_bits_llc_wayOH;
                const uint32_t rLlcHit = b == 0 ? ref.io_rRespVec_0_bits_llc_hit
                                                : ref.io_rRespVec_1_bits_llc_hit;
                const uint32_t rLlcMeta = b == 0 ? ref.io_rRespVec_0_bits_llc_metaVec_0_state
                                                 : ref.io_rRespVec_1_bits_llc_metaVec_0_state;
                const uint32_t rSfWay = b == 0 ? ref.io_rRespVec_0_bits_sf_wayOH
                                               : ref.io_rRespVec_1_bits_sf_wayOH;
                const uint32_t rSfHit = b == 0 ? ref.io_rRespVec_0_bits_sf_hit
                                               : ref.io_rRespVec_1_bits_sf_hit;
                const uint32_t rSfMeta = b == 0 ? ref.io_rRespVec_0_bits_sf_metaVec_0_state
                                                : ref.io_rRespVec_1_bits_sf_metaVec_0_state;
                cosim::check(st, "dir", "Directory", seed, c,
                             cosim::lanePort("rresp_llc_wayOH", b).c_str(), rLlcWay, dm.llc.wayOH,
                             rp);
                cosim::check(st, "dir", "Directory", seed, c,
                             cosim::lanePort("rresp_llc_hit", b).c_str(), rLlcHit, dm.llc.hit, rp);
                cosim::check(st, "dir", "Directory", seed, c,
                             cosim::lanePort("rresp_llc_meta", b).c_str(), rLlcMeta, dm.llc.meta,
                             rp);
                cosim::check(st, "dir", "Directory", seed, c,
                             cosim::lanePort("rresp_sf_wayOH", b).c_str(), rSfWay, dm.sf.wayOH,
                             rp);
                cosim::check(st, "dir", "Directory", seed, c,
                             cosim::lanePort("rresp_sf_hit", b).c_str(), rSfHit, dm.sf.hit, rp);
                cosim::check(st, "dir", "Directory", seed, c,
                             cosim::lanePort("rresp_sf_meta", b).c_str(), rSfMeta, dm.sf.meta,
                             rp);
            }
        }
        cosim::check(st, "dir", "Directory", seed, c, "wresp_llc_valid", ref.io_wResp_llc_valid,
                     dut.wresp_llc.get().valid, rp);
        if (ref.io_wResp_llc_valid && dut.wresp_llc.get().valid) {
            const auto db = dut.wresp_llc.get().bits;
            cosim::check(st, "dir", "Directory", seed, c, "wresp_llc_addr",
                         ref.io_wResp_llc_bits_addr, db.addr, rp);
            cosim::check(st, "dir", "Directory", seed, c, "wresp_llc_wayOH",
                         ref.io_wResp_llc_bits_wayOH, db.wayOH, rp);
            cosim::check(st, "dir", "Directory", seed, c, "wresp_llc_hit",
                         ref.io_wResp_llc_bits_hit, db.hit, rp);
            cosim::check(st, "dir", "Directory", seed, c, "wresp_llc_meta",
                         ref.io_wResp_llc_bits_metaVec_0_state, db.meta, rp);
            cosim::check(st, "dir", "Directory", seed, c, "wresp_llc_txn",
                         ref.io_wResp_llc_bits_hnTxnID, db.hnTxnID, rp);
        }
        cosim::check(st, "dir", "Directory", seed, c, "wresp_sf_valid", ref.io_wResp_sf_valid,
                     dut.wresp_sf.get().valid, rp);
        if (ref.io_wResp_sf_valid && dut.wresp_sf.get().valid) {
            const auto db = dut.wresp_sf.get().bits;
            cosim::check(st, "dir", "Directory", seed, c, "wresp_sf_addr",
                         ref.io_wResp_sf_bits_addr, db.addr, rp);
            cosim::check(st, "dir", "Directory", seed, c, "wresp_sf_wayOH",
                         ref.io_wResp_sf_bits_wayOH, db.wayOH, rp);
            cosim::check(st, "dir", "Directory", seed, c, "wresp_sf_hit",
                         ref.io_wResp_sf_bits_hit, db.hit, rp);
            cosim::check(st, "dir", "Directory", seed, c, "wresp_sf_meta",
                         ref.io_wResp_sf_bits_metaVec_0_state, db.meta, rp);
            cosim::check(st, "dir", "Directory", seed, c, "wresp_sf_txn",
                         ref.io_wResp_sf_bits_hnTxnID, db.hnTxnID, rp);
        }

        // fire 采样（phaseLow 组合稳态值）
        const bool rd0Fire = rd0.valid && dut.read_0_rdy.get();
        const bool rd1Fire = rd1.valid && dut.read_1_rdy.get();
        const bool wrFire = wr.valid && dut.write_rdy.get();

        cosim::phaseHigh(ref, dut);

        // fire 后撤销激励（rdy 以本拍 phaseLow 采样为准）
        if (rd0Fire) rd0 = {false, {}};
        if (rd1Fire) rd1 = {false, {}};
        if (wrFire) wr = {false, {}};
    }

    std::cout << (st.mismatches == 0 ? "PASS" : "FAIL") << " dir Directory seed=" << seed
              << " cycles=" << cycles << " checks=" << st.checks
              << " mismatches=" << st.mismatches << "\n";
    return st.mismatches;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {1u, 2u, 3u}) bad += cosimDir(seed, 200000);
    return bad == 0 ? 0 : 1;
}
