// Frontend 对拍 harness：wolvicmod Frontend vs refgenDj RTL Frontend
// （dongjiang/frontend 真实源码，kunminghu-v3 单核 32MB 配置，dirBank=0）。
// 激励：合法 rxReq 请求（decode 表内 opcode 族 + 小地址池）；环境 =
//   影子目录（read_dir 后 4 拍回 respDir，命中状态按地址记忆）、
//   PoS 管理器（reqPosVec→posResp 分配 free way；alloc 后随机延迟 cleanPos；
//   updPosTag 跟写地址表）、getAddrVec 地址表。
// 每拍比对：rx_req_rdy、req_db_s1/s3、fast_data、clean_db、read_dir、cmt_task
//   全字段、fast_resp、pos_resp_vec、alr_use_pos、working、get_addr_result。

#include <array>
#include <cstdint>
#include <deque>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "VFrontend.h"
#include "VFrontend___024root.h"
#include "VFrontend_PosSet.h"
#include "VFrontend_TaskEntry_8.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include "model/dj/frontend.h"

using namespace wolvicmod;
using namespace zj::dj;

namespace {

constexpr uint8_t kDBIDResp = 0x6, kReadReceipt = 0x8;

uint64_t randAddr(std::mt19937& rng) { return catAddr(0, rng() % 16, rng() % 4, 13, 0); }

// ---------------- 影子目录 ----------------
struct DirLine {
    bool valid = false;
    uint8_t llcMeta = 0;  // 0=I 1=SC 2=UD 3=UC
    uint16_t llcWayOH = 0;
    uint8_t sfMeta = 0;
    uint16_t sfWayOH = 0;
};

// ---------------- 请求族（与 backend harness 同构） ----------------
struct Fam {
    uint8_t opcode;
    uint8_t kind;  // 0=Read 1=Write 2=Dataless
    bool expCompAck;
    bool allocate, ewa, fullSize;
    uint8_t order;
};

Fam randFam(std::mt19937& rng) {
    const uint32_t k = rng() % 14;
    switch (k) {
        case 0: return {0x04, 0, false, false, false, false, 3};
        case 1: return {0x04, 0, true, false, false, false, 3};
        case 2: return {0x04, 0, true, false, true, false, 3};
        case 3: return {0x03, 0, true, false, false, false, 3};
        case 4: return {0x03, 0, true, true, false, false, 3};
        case 5: return {0x03, 0, true, true, true, true, 3};
        case 6: return {0x26, 0, true, true, true, true, 0};
        case 7: return {0x07, 0, true, true, true, true, 0};
        case 8: return {0x22, 0, false, true, true, true, 0};
        case 9: return {0x1c, 1, false, false, false, false, 3};
        case 10: return {0x1c, 1, true, false, false, false, 2};
        case 11: return {0x18, 1, true, false, false, false, 2};
        case 12: return {0x18, 1, true, true, true, false, 2};
        default: {
            switch (rng() % 5) {
                case 0: return {0x0c, 2, true, false, false, false, 0};
                case 1: return {0x0d, 2, false, false, false, false, 0};
                case 2: return {0x08, 2, false, false, false, false, 0};
                case 3: return {0x09, 2, false, false, false, false, 0};
                default: return {0x0a, 2, false, false, false, false, 0};
            }
        }
    }
}

struct WayPool {
    std::array<std::array<bool, 16>, 4> used{};
    int alloc(uint32_t s, std::mt19937& rng) {
        for (uint32_t w = 14; w < 16; ++w)
            if (!used[s][w]) {
                used[s][w] = true;
                return static_cast<int>(w);
            }
        std::vector<int> fr;
        for (uint32_t w = 0; w < 14; ++w)
            if (!used[s][w]) fr.push_back(w);
        if (fr.empty()) return -1;
        const int w = fr[rng() % fr.size()];
        used[s][w] = true;
        return w;
    }
    void free(uint8_t hnIdx) { used[hnIdxPosSet(hnIdx)][hnIdxPosWay(hnIdx)] = false; }
};

struct EvDir {
    uint64_t at;
    DirMsg m;
};
struct EvPosR {
    uint64_t at;
    uint8_t s, way;
};
struct EvClean {
    uint64_t at;
    PosClean p;
};

}  // namespace
namespace {

void setRxReq(VFrontend& ref, const Valid<HReqFlit>& v) {
    ref.io_rxReq_valid = v.valid;
    ref.io_rxReq_bits_ExpCompAck = v.bits.exp_comp_ack;
    ref.io_rxReq_bits_Excl = v.bits.excl;
    ref.io_rxReq_bits_SnpAttr = v.bits.snp_attr;
    ref.io_rxReq_bits_MemAttr = v.bits.mem_attr;
    ref.io_rxReq_bits_Order = v.bits.order;
    ref.io_rxReq_bits_Addr = v.bits.addr;
    ref.io_rxReq_bits_Size = v.bits.size;
    ref.io_rxReq_bits_Opcode = v.bits.opcode;
    ref.io_rxReq_bits_TxnID = v.bits.txn_id;
    ref.io_rxReq_bits_SrcID = v.bits.src_id;
    ref.io_rxReq_bits_TgtID = v.bits.tgt_id;
    ref.io_rxReq_bits_QoS = v.bits.qos;
}
void setRxHpr(VFrontend& ref, const Valid<HReqFlit>& v) {
    ref.io_rxHpr_valid = v.valid;
    ref.io_rxHpr_bits_ExpCompAck = v.bits.exp_comp_ack;
    ref.io_rxHpr_bits_Excl = v.bits.excl;
    ref.io_rxHpr_bits_SnpAttr = v.bits.snp_attr;
    ref.io_rxHpr_bits_MemAttr = v.bits.mem_attr;
    ref.io_rxHpr_bits_Order = v.bits.order;
    ref.io_rxHpr_bits_Addr = v.bits.addr;
    ref.io_rxHpr_bits_Size = v.bits.size;
    ref.io_rxHpr_bits_Opcode = v.bits.opcode;
    ref.io_rxHpr_bits_TxnID = v.bits.txn_id;
    ref.io_rxHpr_bits_SrcID = v.bits.src_id;
    ref.io_rxHpr_bits_TgtID = v.bits.tgt_id;
    ref.io_rxHpr_bits_QoS = v.bits.qos;
}
void setRespDir(VFrontend& ref, const Valid<DirMsg>& v) {
    ref.io_respDir_valid = v.valid;
    ref.io_respDir_bits_llc_wayOH = v.bits.llc.wayOH;
    ref.io_respDir_bits_llc_hit = v.bits.llc.hit;
    ref.io_respDir_bits_llc_metaVec_0_state = v.bits.llc.meta;
    ref.io_respDir_bits_sf_wayOH = v.bits.sf.wayOH;
    ref.io_respDir_bits_sf_hit = v.bits.sf.hit;
    ref.io_respDir_bits_sf_metaVec_0_state = v.bits.sf.meta;
}
void setReqPos(VFrontend& ref, int s, const Valid<ReplReqPos>& v) {
    switch (s) {
        case 0:
            ref.io_reqPosVec_0_valid = v.valid;
            ref.io_reqPosVec_0_bits_channel = v.bits.channel;
            break;
        case 1:
            ref.io_reqPosVec_1_valid = v.valid;
            ref.io_reqPosVec_1_bits_channel = v.bits.channel;
            break;
        case 2:
            ref.io_reqPosVec_2_valid = v.valid;
            ref.io_reqPosVec_2_bits_channel = v.bits.channel;
            break;
        default:
            ref.io_reqPosVec_3_valid = v.valid;
            ref.io_reqPosVec_3_bits_channel = v.bits.channel;
            break;
    }
}
void setUpdPosTag(VFrontend& ref, const Valid<UpdPosTag>& v) {
    ref.io_updPosTag_valid = v.valid;
    ref.io_updPosTag_bits_addr = v.bits.addr;
    ref.io_updPosTag_bits_addrVal = v.bits.addrVal;
    ref.io_updPosTag_bits_hnIdx_dirBank = hnIdxDirBank(v.bits.hnIdx);
    ref.io_updPosTag_bits_hnIdx_pos_set = hnIdxPosSet(v.bits.hnIdx);
    ref.io_updPosTag_bits_hnIdx_pos_way = hnIdxPosWay(v.bits.hnIdx);
}
void setCleanPos(VFrontend& ref, const Valid<PosClean>& v) {
    ref.io_cleanPoS_valid = v.valid;
    ref.io_cleanPoS_bits_hnIdx_dirBank = hnIdxDirBank(v.bits.hnIdx);
    ref.io_cleanPoS_bits_hnIdx_pos_set = hnIdxPosSet(v.bits.hnIdx);
    ref.io_cleanPoS_bits_hnIdx_pos_way = hnIdxPosWay(v.bits.hnIdx);
    ref.io_cleanPoS_bits_channel = v.bits.channel;
    ref.io_cleanPoS_bits_qos = v.bits.qos;
}
void setGetAddrHnIdx(VFrontend& ref, int i, uint8_t h) {
    const uint32_t db = hnIdxDirBank(h), ps = hnIdxPosSet(h), pw = hnIdxPosWay(h);
    switch (i) {
        case 0:
            ref.io_getAddrVec_0_hnIdx_dirBank = db;
            ref.io_getAddrVec_0_hnIdx_pos_set = ps;
            ref.io_getAddrVec_0_hnIdx_pos_way = pw;
            break;
        case 1:
            ref.io_getAddrVec_1_hnIdx_dirBank = db;
            ref.io_getAddrVec_1_hnIdx_pos_set = ps;
            ref.io_getAddrVec_1_hnIdx_pos_way = pw;
            break;
        default:
            ref.io_getAddrVec_2_hnIdx_dirBank = db;
            ref.io_getAddrVec_2_hnIdx_pos_set = ps;
            ref.io_getAddrVec_2_hnIdx_pos_way = pw;
            break;
    }
}

void CHECK_FE(VFrontend& ref, Frontend& dut, cosim::Stats& st, uint32_t seed, uint64_t c,
              cosim::Replay& rp, const std::array<uint64_t, 128>& addrTab);
void CHECK_CMT(VFrontend& ref, Frontend& dut, cosim::Stats& st, uint32_t seed, uint64_t c,
               cosim::Replay& rp);

// ---- 白盒内部状态对拍（FE_INTCMP=1 时启用）：ref 经 Verilator CELL 直读，dut 白盒 .get() ----
// 报告首个发散拍的所有差异字段，用于定位被端口比对掩盖的内部漂移。

struct PeRef {
    const CData *req, *snp, *tagVal, *offset;
    const QData* tag;
};

#define PE_CASE(J)                                             \
    case J:                                                    \
        return {&s.__PVT__entries_##J##__DOT__stateReg_req,    \
                &s.__PVT__entries_##J##__DOT__stateReg_snp,    \
                &s.__PVT__entries_##J##__DOT__stateReg_tagVal, \
                &s.__PVT__entries_##J##__DOT__stateReg_offset, \
                &s.__PVT__entries_##J##__DOT__stateReg_tag}

PeRef peRef(const VFrontend_PosSet& s, int j) {
    switch (j) {
        PE_CASE(0); PE_CASE(1); PE_CASE(2); PE_CASE(3);
        PE_CASE(4); PE_CASE(5); PE_CASE(6); PE_CASE(7);
        PE_CASE(8); PE_CASE(9); PE_CASE(10); PE_CASE(11);
        PE_CASE(12); PE_CASE(13); PE_CASE(14); PE_CASE(15);
        default: std::abort();
    }
}

int intcmpFrontend(VFrontend& ref, Frontend& dut, uint64_t c) {
    static bool reported = false;
    if (reported) return 0;
    auto& rp = *ref.rootp;
    const VFrontend_TaskEntry_8* const* te = &rp.__PVT__Frontend__DOT__reqTaskBuf__DOT__entries_0;
    const VFrontend_PosSet* const* ps = &rp.__PVT__Frontend__DOT__posTable__DOT__sets_0;
    int bad = 0;
    auto dif = [&](const char* nm, uint64_t r, uint64_t d) {
        if (r == d) return;
        if (!bad) std::cout << "INTCMP-DIVERGE cyc=" << c << "\n";
        std::cout << "  " << nm << " ref=0x" << std::hex << r << " dut=0x" << d << std::dec
                  << "\n";
        ++bad;
    };
    for (int i = 0; i < 16; ++i) {
        const auto* e = te[i];
        const auto& de = dut.req_task_buf.entries.get()[i];
        char nm[64];
        std::snprintf(nm, sizeof nm, "tb[%d].state", i);
        dif(nm, e->__PVT__taskReg_state, de.task.state);
        std::snprintf(nm, sizeof nm, "tb[%d].nid", i);
        dif(nm, e->__PVT__nidReg, de.nid);
        std::snprintf(nm, sizeof nm, "tb[%d].retryNum", i);
        dif(nm, e->__PVT__retryNumReg, de.retryNum);
        std::snprintf(nm, sizeof nm, "tb[%d].timeout", i);
        dif(nm, e->__PVT__timeoutReg, de.timeout);
        std::snprintf(nm, sizeof nm, "tb[%d].addr", i);
        dif(nm, e->__PVT__taskReg_addr, de.task.addr & 0xFFFFFFFFFFFFull);
        std::snprintf(nm, sizeof nm, "tb[%d].opcode", i);
        dif(nm, e->__PVT__taskReg_chi_opcode, de.task.chi.opcode);
    }
    for (int s = 0; s < 4; ++s) {
        const auto* rs = ps[s];
        const auto& ds1 = dut.pos_table.s1.get()[s];
        char nm[64];
        std::snprintf(nm, sizeof nm, "pos[%d].lock", s);
        dif(nm, rs->__PVT__lockReg, ds1.lock);
        std::snprintf(nm, sizeof nm, "pos[%d].allocS1v", s);
        dif(nm, rs->__PVT__allocReg_s1_valid, ds1.allocValid);
        std::snprintf(nm, sizeof nm, "pos[%d].allocWay", s);
        dif(nm, rs->__PVT__allocWayReg_s1, ds1.allocWay);
        for (int j = 0; j < 16; ++j) {
            const PeRef pr = peRef(*rs, j);
            const PosState& pst = dut.pos_table.entries.get()[s * 16 + j].state;
            std::snprintf(nm, sizeof nm, "pos[%d][%d].req", s, j);
            dif(nm, *pr.req, pst.req);
            std::snprintf(nm, sizeof nm, "pos[%d][%d].snp", s, j);
            dif(nm, *pr.snp, pst.snp);
            std::snprintf(nm, sizeof nm, "pos[%d][%d].tagVal", s, j);
            dif(nm, *pr.tagVal, pst.tagVal);
            std::snprintf(nm, sizeof nm, "pos[%d][%d].tag", s, j);
            dif(nm, *pr.tag, pst.tag & 0x3FFFFFFFFFull);
            std::snprintf(nm, sizeof nm, "pos[%d][%d].offset", s, j);
            dif(nm, *pr.offset, pst.offset);
        }
    }
    dif("block.validReg", rp.Frontend__DOT__block__DOT__validReg_s1,
        dut.block.valid_reg_s1.get());
    dif("tb.hasLockReg", rp.Frontend__DOT__reqTaskBuf__DOT__hasLockReg,
        dut.req_task_buf.has_lock_reg.get());
    {
        const uint32_t rvip = rp.Frontend__DOT__reqTaskBuf__DOT__io_chiTask_s0_arb__DOT__low_arb__DOT__vipPtrOH;
        uint32_t dvipl = 0;
        for (uint32_t i = 0; i < 16; ++i)
            if (dut.req_task_buf.s0_arb.vip.get() == i) dvipl = 1u << i;
        dif("tb.vipPtrOH", rvip, dvipl);
    }
    dif("block.sReceipt", rp.Frontend__DOT__block__DOT__sReceiptReg_s1,
        dut.block.s_receipt_reg_s1.get());
    dif("block.sDBID", rp.Frontend__DOT__block__DOT__sDBIDReg_s1,
        dut.block.s_dbid_reg_s1.get());
    dif("decode.validReg", rp.Frontend__DOT__decode__DOT__validReg_s3,
        dut.decode.valid_reg_s3.get());
    if (bad) reported = true;
    return bad;
}

void intwatchFrontend(VFrontend& ref, Frontend& dut, uint64_t c) {
    static uint64_t lo = 0, hi = 0;
    static bool init = false;
    if (!init) {
        const char* e = std::getenv("FE_INTWATCH");
        if (e) std::sscanf(e, "%lu:%lu", &lo, &hi);
        init = true;
    }
    if (c < lo || c > hi || lo == hi) return;
    auto& rp = *ref.rootp;
    uint32_t dvalid = 0, dlock = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        if (dut.req_task_buf.w_s0_in.get()[i].valid) dvalid |= 1u << i;
        if (dut.req_task_buf.w_lock_all.get()[i]) dlock |= 1u << i;
    }
    const uint32_t rvip = rp.Frontend__DOT__reqTaskBuf__DOT__io_chiTask_s0_arb__DOT__low_arb__DOT__vipPtrOH;
    std::cout << "  [w] cyc=" << c << " refVipOH=0x" << std::hex << rvip << " dutVip=0x"
              << (1u << dut.req_task_buf.s0_arb.vip.get()) << std::dec
              << " s0valids=0x" << std::hex << dvalid << std::dec
              << " dutLockVec=0x" << std::hex << dlock << std::dec
              << " refLock=" << (int)rp.Frontend__DOT__reqTaskBuf__DOT__hasLockReg
              << " refLockIdx=" << (int)rp.Frontend__DOT__reqTaskBuf__DOT__lockIdx
              << " refS0v=" << (int)rp.Frontend__DOT__block__DOT__validReg_s1
              << " dutLock=" << (int)dut.req_task_buf.has_lock_reg.get()
              << " arbOut(v=" << (int)dut.req_task_buf.s0_arb.out.get().valid
              << ",rdy=" << (int)dut.req_task_buf.s0_arb.out_rdy.get()
              << ",chosen=" << dut.req_task_buf.s0_arb.chosen.get() << ")"
              << " s0out(v=" << (int)dut.req_task_buf.chi_task_s0.get().valid << ")\n";
}


uint64_t cosimFrontend(uint32_t seed, uint64_t cycles) {
    VFrontend ref;
    Frontend dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;

    cosim::resetRef(ref, [&] {
        setRxReq(ref, {false, {}});
        setRespDir(ref, {false, {}});
        for (int s = 0; s < 4; ++s) setReqPos(ref, s, {false, {}});
        setUpdPosTag(ref, {false, {}});
        setCleanPos(ref, {false, {}});
        for (int i = 0; i < 3; ++i) setGetAddrHnIdx(ref, i, 0);
        ref.io_rxHpr_valid = 0;
        ref.io_reqDB_s1_ready = 0;
        ref.io_reqDB_s3_ready = 0;
        ref.io_fastData_ready = 0;
        ref.io_cleanDB_ready = 1;
        ref.io_readDir_ready = 0;
        ref.io_fastResp_ready = 0;
        ref.io_config_ci = 0;
        ref.io_config_closeLLC = 0;
        ref.io_config_bankId = 0;
        ref.io_dirBank = 0;
    });
    dut.cfg_ci.set(0);
    dut.cfg_bank_id.set(0);
    dut.dir_bank.set(0);
    dut.rx_req.set({false, {}});
    dut.req_db_s1_rdy.set(false);
    dut.req_db_s3_rdy.set(false);
    dut.fast_data_s3_rdy.set(false);
    dut.read_dir_rdy.set(false);
    dut.resp_dir.set({false, {}});
    dut.get_addr_hnidx.set({});
    dut.req_pos_vec.set({});
    dut.upd_pos_tag.set({false, {}});
    dut.clean_pos.set({false, {}});
    dut.fast_resp_rdy.set(false);
    dut.clk.set(0);
    dut.eval();

    WayPool pool;
    std::array<DirLine, 64> dirShadow{};  // 按 useAddr[7:0] 索引的影子目录
    std::array<uint64_t, 128> addrTab{};
    std::array<uint8_t, 128> slotChan{};
    std::deque<EvDir> evDir;
    std::deque<EvPosR> evPosR;
    std::deque<EvClean> evClean;
    Valid<HReqFlit> pendReq{false, {}};
    Valid<HReqFlit> pendHpr{false, {}};

    auto shadowLookup = [&](uint64_t addr, DirMsg& m) {
        auto& line = dirShadow[useAddr(addr) & 0x3F];
        if (!line.valid || rng() % 100 < 25) {  // 未记录或 25% 重随机（模拟替换）
            line.valid = true;
            const uint32_t r = rng() % 100;
            line.llcMeta = r < 45 ? 0 : (r < 65 ? 1 : (r < 85 ? 3 : 2));
            line.llcWayOH = static_cast<uint16_t>(1u << (rng() % 16));
            line.sfMeta = rng() % 100 < 35 ? 1 : 0;
            line.sfWayOH = static_cast<uint16_t>(1u << (rng() % 16));
        }
        m.llc = {line.llcWayOH, line.llcMeta, line.llcMeta != 0};
        m.sf = {line.sfWayOH, line.sfMeta, line.sfMeta != 0};
    };

    for (uint64_t c = 0; c < cycles; ++c) {
        const uint32_t pct = cosim::densityAt(c, cycles);

        // ---- 新请求 ----
        if (!pendReq.valid && cosim::roll(rng, pct / 2)) {
            const Fam f = randFam(rng);
            HReqFlit fl{};
            fl.addr = randAddr(rng);
            fl.qos = (rng() % 6 == 0) ? 0xF : (rng() & 7);
            fl.opcode = f.opcode;
            fl.order = f.order;
            fl.exp_comp_ack = f.expCompAck;
            fl.snp_attr = true;
            fl.excl = false;
            fl.mem_attr = (f.allocate << 3) | (1u << 2) | f.ewa;
            fl.size = f.fullSize ? 6 : 5;
            fl.txn_id = rng() & 0xFFF;
            fl.src_id = 0x09;
            fl.tgt_id = 0;
            pendReq = {true, fl};
        }
        // HPR：同合法性约束的独立流，qos 偏 0xf（ChiXbar 只把 0xf 改道 HPR）
        if (!pendHpr.valid && cosim::roll(rng, pct / 3)) {
            const Fam f = randFam(rng);
            HReqFlit fl{};
            fl.addr = randAddr(rng);
            fl.qos = (rng() % 4 != 0) ? 0xF : (rng() & 7);
            fl.opcode = f.opcode;
            fl.order = f.order;
            fl.exp_comp_ack = f.expCompAck;
            fl.snp_attr = true;
            fl.excl = false;
            fl.mem_attr = (f.allocate << 3) | (1u << 2) | f.ewa;
            fl.size = f.fullSize ? 6 : 5;
            fl.txn_id = rng() & 0xFFF;
            fl.src_id = 0x09;
            fl.tgt_id = 0;
            pendHpr = {true, fl};
        }

        // ---- 事件出队 ----
        Valid<DirMsg> dirV{false, {}};
        if (!evDir.empty() && evDir.front().at <= c) {
            dirV = {true, evDir.front().m};
            evDir.pop_front();
        }
        bool posRV = false;
        uint8_t posRS = 0, posRW = 0;
        if (!evPosR.empty() && evPosR.front().at <= c) {
            posRV = true;
            posRS = evPosR.front().s;
            posRW = evPosR.front().way;
            evPosR.pop_front();
        }
        Valid<PosClean> cleanV{false, {}};
        if (!evClean.empty() && evClean.front().at <= c) {
            cleanV = {true, evClean.front().p};
            cleanV.bits.channel = slotChan[cleanV.bits.hnIdx];
            pool.free(cleanV.bits.hnIdx);
            evClean.pop_front();
        }

        // ---- 驱动两侧 ----
        const bool reqS1Rdy = cosim::roll(rng, 70), reqS3Rdy = cosim::roll(rng, 70),
                   fdRdy = cosim::roll(rng, 70), rdRdy = cosim::roll(rng, 85),
                   frRdy = cosim::roll(rng, 70);
        dut.req_db_s1_rdy.set(reqS1Rdy);
        ref.io_reqDB_s1_ready = reqS1Rdy;
        dut.req_db_s3_rdy.set(reqS3Rdy);
        ref.io_reqDB_s3_ready = reqS3Rdy;
        dut.fast_data_s3_rdy.set(fdRdy);
        ref.io_fastData_ready = fdRdy;
        dut.read_dir_rdy.set(rdRdy);
        ref.io_readDir_ready = rdRdy;
        dut.fast_resp_rdy.set(frRdy);
        ref.io_fastResp_ready = frRdy;
        ref.io_cleanDB_ready = 1;

        dut.rx_req.set(pendReq);
        setRxReq(ref, pendReq);
        dut.rx_hpr.set(pendHpr);
        setRxHpr(ref, pendHpr);
        dut.resp_dir.set(dirV);
        setRespDir(ref, dirV);
        // reqPosVec：随机脉冲（ReplaceCM 侧要槽）
        std::array<Valid<ReplReqPos>, 4> reqPosArr{};
        for (uint32_t s = 0; s < 4; ++s) {
            const bool v = cosim::roll(rng, 8);
            reqPosArr[s] = {v, {0, static_cast<uint8_t>(cosim::roll(rng, 70) ? 0 : 3)}};
            setReqPos(ref, s, reqPosArr[s]);
        }
        dut.req_pos_vec.set(reqPosArr);
        // updPosTag：随机（repl 槽 tag 就绪）
        Valid<UpdPosTag> uptV{false, {}};
        if (cosim::roll(rng, 10)) {
            const uint8_t hnIdx = hnIdxOf(0, rng() % 4, 14 + rng() % 2);
            const uint64_t a = randAddr(rng) & ~0x3Full;
            uptV = {true, {a, true, hnIdx}};
            addrTab[hnIdx] = a;
        }
        dut.upd_pos_tag.set(uptV);
        setUpdPosTag(ref, uptV);
        dut.clean_pos.set(cleanV);
        setCleanPos(ref, cleanV);
        for (int i = 0; i < 3; ++i) setGetAddrHnIdx(ref, i, rng() % 128);
        typename Frontend::U8x3 ga{};
        for (int i = 0; i < 3; ++i) ga[i] = static_cast<uint8_t>(rng() % 128);
        dut.get_addr_hnidx.set(ga);
        for (int i = 0; i < 3; ++i) setGetAddrHnIdx(ref, i, ga[i]);

        rp.push("cyc=" + std::to_string(c));
        cosim::phaseLow(ref, dut);
        if (std::getenv("FE_INTCMP")) intcmpFrontend(ref, dut, c);
        intwatchFrontend(ref, dut, c);
        if (std::getenv("FE_TRACE") && c <= (uint64_t)std::atol(std::getenv("FE_TRACE"))) {
            std::cout << "  [t] cyc=" << c << " rdy(reqs1=" << reqS1Rdy << ",s3=" << reqS3Rdy
                      << ",fd=" << fdRdy << ",rd=" << rdRdy << ",fr=" << frRdy << ") pendReq(v="
                      << (int)pendReq.valid << ",op=0x" << std::hex
                      << (uint32_t)pendReq.bits.opcode << ",sz=" << (uint32_t)pendReq.bits.size
                      << ")" << std::dec << " ref: rdir=" << (int)ref.io_readDir_valid
                      << " reqs3=" << (int)ref.io_reqDB_s3_valid
                      << " fresp=" << (int)ref.io_fastResp_valid
                      << " alrpos=" << (int)ref.io_alrUsePoS << " | dut: rdir="
                      << (int)dut.read_dir.get().valid << " reqs3="
                      << (int)dut.req_db_s3.get().valid << " fresp="
                      << (int)dut.block.fast_resp_s1.get().valid << " blany="
                      << (int)dut.block.w_block_any.get() << " blpos="
                      << (int)dut.block.w_block_pos.get() << " bldir="
                      << (int)dut.block.w_block_dir.get() << " blresp="
                      << (int)dut.block.w_block_resp.get() << " alrpos="
                      << (int)dut.alr_use_pos.get() << " | dec: ts2v="
                      << (int)dut.decode.task_s2.get().valid << " v3="
                      << (int)dut.decode.valid_reg_s3.get() << " rdv="
                      << (int)dut.resp_dir.get().valid << " dl=" << std::hex
                      << (uint32_t)dut.decode.w_dec_list_s3.get()[0]
                      << (uint32_t)dut.decode.w_dec_list_s3.get()[1] << " tc=0x"
                      << dut.decode.w_task_code_s3.get() << " cc=0x"
                      << dut.decode.w_cmt_code_s3.get() << " rcd="
                      << (int)dut.decode.w_resp_comp_data_s3.get() << std::dec << " | pos: s0v="
                      << (int)dut.pos_table.alloc_s0_valid.get() << " blk="
                      << (int)dut.pos_table.w_block_s0.get()[0]
                      << (int)dut.pos_table.w_block_s0.get()[1]
                      << (int)dut.pos_table.w_block_s0.get()[2]
                      << (int)dut.pos_table.w_block_s0.get()[3] << " free="
                      << std::hex << dut.pos_table.w_free_vec.get()[0]
                      << dut.pos_table.w_free_vec.get()[1]
                      << dut.pos_table.w_free_vec.get()[2]
                      << dut.pos_table.w_free_vec.get()[3] << std::dec << " mat="
                      << std::hex << dut.pos_table.w_mat_tag_vec.get()[0]
                      << dut.pos_table.w_mat_tag_vec.get()[1]
                      << dut.pos_table.w_mat_tag_vec.get()[2]
                      << dut.pos_table.w_mat_tag_vec.get()[3] << std::dec << " lock="
                      << (int)dut.pos_table.s1.get()[0].lock
                      << (int)dut.pos_table.s1.get()[1].lock
                      << (int)dut.pos_table.s1.get()[2].lock
                      << (int)dut.pos_table.s1.get()[3].lock << " | refcmt: v="
                      << (int)ref.io_cmtTask_valid << " dl="
                      << (uint32_t)ref.io_cmtTask_bits_decList_0
                      << (uint32_t)ref.io_cmtTask_bits_decList_1 << " op=0x" << std::hex
                      << (uint32_t)ref.io_cmtTask_bits_chi_opcode << " ch="
                      << (uint32_t)ref.io_cmtTask_bits_chi_channel << std::dec << " dutcmt: v="
                      << (int)dut.cmt_task.get().valid << " rdv3="
                      << (int)dut.decode.resp_dir_s3.get().valid << " si=0x" << std::hex
                      << dut.decode.w_state_inst_s3.get() << std::dec << "\n";
        }

        // ---- 比对（前端通道全展开见 CHECK_FE） ----
        CHECK_FE(ref, dut, st, seed, c, rp, addrTab);

        // ---- fire 采样 + 环境响应 ----
        const bool readDirFire = dut.read_dir.get().valid && dut.read_dir_rdy.get();
        const uint64_t readDirAddr = dut.read_dir.get().bits.addr;
        const bool cacheableReq = readDirFire;
        const bool allocFire = dut.pos_table.hn_idx_s1_valid.get();
        const uint8_t allocHn = dut.pos_table.hn_idx_s1.get();
        const uint64_t allocAddr = dut.block.task_reg_s1.get().addr;

        cosim::phaseHigh(ref, dut);

        if (readDirFire) {
            DirMsg m;
            shadowLookup(readDirAddr, m);
            evDir.push_back({c + 4, m});
        }
        // posResp（PosSet 有效时分配）
        for (uint32_t s = 0; s < 4; ++s)
            if (reqPosArr[s].valid) {
                const int w = pool.alloc(s, rng);
                if (w >= 0) {
                    slotChan[hnIdxOf(0, s, w)] = reqPosArr[s].bits.channel;
                    evPosR.push_back({c + 1 + rng() % 3, static_cast<uint8_t>(s),
                                      static_cast<uint8_t>(w)});
                }
            }
        // alloc（hnIdx_s1 脉冲）→ 记录槽地址/channel + 计划 cleanPos
        if (allocFire) {
            addrTab[allocHn] = allocAddr;
            slotChan[allocHn] = 0;
            evClean.push_back(
                {c + 20 + rng() % 60, {allocHn, 0, static_cast<uint8_t>(rng() & 7)}});
        }
        // rxReq fire → 撤销
        if (pendReq.valid && dut.rx_req_rdy.get()) pendReq = {false, {}};
        if (pendHpr.valid && dut.rx_hpr_rdy.get()) pendHpr = {false, {}};
    }

    std::cout << (st.mismatches == 0 ? "PASS" : "FAIL") << " frontend Frontend seed=" << seed
              << " cycles=" << cycles << " checks=" << st.checks
              << " mismatches=" << st.mismatches << "\n";
    return st.mismatches;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {1u, 2u, 3u}) bad += cosimFrontend(seed, 150000);
    return bad == 0 ? 0 : 1;
}
namespace {

void CHECK_FE(VFrontend& ref, Frontend& dut, cosim::Stats& st, uint32_t seed, uint64_t c,
              cosim::Replay& rp, const std::array<uint64_t, 128>& addrTab) {
    cosim::check(st, "fe", "Frontend", seed, c, "rxreq_rdy", ref.io_rxReq_ready,
                 dut.rx_req_rdy.get(), rp);
    // req_db_s1 / req_db_s3
    cosim::check(st, "fe", "Frontend", seed, c, "reqs1_v", ref.io_reqDB_s1_valid,
                 dut.req_db_s1.get().valid, rp);
    if (ref.io_reqDB_s1_valid && dut.req_db_s1.get().valid) {
        cosim::check(st, "fe", "Frontend", seed, c, "reqs1_txn", ref.io_reqDB_s1_bits_hnTxnID,
                     dut.req_db_s1.get().bits.hnTxnID, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "reqs1_dv0",
                     ref.io_reqDB_s1_bits_dataVec_0, dut.req_db_s1.get().bits.dataVec & 1, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "reqs1_dv1",
                     ref.io_reqDB_s1_bits_dataVec_1, (dut.req_db_s1.get().bits.dataVec >> 1) & 1,
                     rp);
    }
    cosim::check(st, "fe", "Frontend", seed, c, "reqs3_v", ref.io_reqDB_s3_valid,
                 dut.req_db_s3.get().valid, rp);
    if (ref.io_reqDB_s3_valid && dut.req_db_s3.get().valid) {
        cosim::check(st, "fe", "Frontend", seed, c, "reqs3_txn", ref.io_reqDB_s3_bits_hnTxnID,
                     dut.req_db_s3.get().bits.hnTxnID, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "reqs3_dv0",
                     ref.io_reqDB_s3_bits_dataVec_0, dut.req_db_s3.get().bits.dataVec & 1, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "reqs3_dv1",
                     ref.io_reqDB_s3_bits_dataVec_1, (dut.req_db_s3.get().bits.dataVec >> 1) & 1,
                     rp);
    }
    // fast_data
    cosim::check(st, "fe", "Frontend", seed, c, "fdat_v", ref.io_fastData_valid,
                 dut.fast_data_s3.get().valid, rp);
    if (ref.io_fastData_valid && dut.fast_data_s3.get().valid) {
        const auto& b = dut.fast_data_s3.get().bits;
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_txn", ref.io_fastData_bits_hnTxnID,
                     b.hnTxnID, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_repl",
                     ref.io_fastData_bits_dataOp_repl, b.dataOp.repl, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_read",
                     ref.io_fastData_bits_dataOp_read, b.dataOp.read, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_send",
                     ref.io_fastData_bits_dataOp_send, b.dataOp.send, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_save",
                     ref.io_fastData_bits_dataOp_save, b.dataOp.save, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_merge",
                     ref.io_fastData_bits_dataOp_merge, b.dataOp.merge, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_bank",
                     ref.io_fastData_bits_ds_bank, b.ds.bank, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_idx", ref.io_fastData_bits_ds_idx,
                     b.ds.idx, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_dv0",
                     ref.io_fastData_bits_dataVec_0, b.dataVec & 1, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_dv1",
                     ref.io_fastData_bits_dataVec_1, (b.dataVec >> 1) & 1, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_qos", ref.io_fastData_bits_qos,
                     b.qos, rp);
        const auto& f = b.txDat;
        for (int w = 0; w < 4; ++w) {
            const uint64_t rv =
                (static_cast<uint64_t>(ref.io_fastData_bits_txDat_Data[2 * w + 1]) << 32) |
                ref.io_fastData_bits_txDat_Data[2 * w];
            cosim::check(st, "fe", "Frontend", seed, c,
                         (std::string("fdat_data") + std::to_string(w)).c_str(), rv, f.data[w],
                         rp);
        }
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_be", ref.io_fastData_bits_txDat_BE,
                     f.be, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_did",
                     ref.io_fastData_bits_txDat_DataID, f.data_id, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_dbid",
                     ref.io_fastData_bits_txDat_DBID, f.dbid, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_cbusy",
                     ref.io_fastData_bits_txDat_CBusy, f.c_busy, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_ds",
                     ref.io_fastData_bits_txDat_DataSource, f.data_source, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_resp",
                     ref.io_fastData_bits_txDat_Resp, f.resp, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_resperr",
                     ref.io_fastData_bits_txDat_RespErr, f.resp_err, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_opcode",
                     ref.io_fastData_bits_txDat_Opcode, f.opcode, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_homenid",
                     ref.io_fastData_bits_txDat_HomeNID, f.home_nid, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_txnid",
                     ref.io_fastData_bits_txDat_TxnID, f.txn_id, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_srcid",
                     ref.io_fastData_bits_txDat_SrcID, f.src_id, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_tgtid",
                     ref.io_fastData_bits_txDat_TgtID, f.tgt_id, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fdat_fqos",
                     ref.io_fastData_bits_txDat_QoS, f.qos, rp);
    }
    // clean_db
    cosim::check(st, "fe", "Frontend", seed, c, "cdb_v", ref.io_cleanDB_valid,
                 dut.clean_db_s3.get().valid, rp);
    if (ref.io_cleanDB_valid && dut.clean_db_s3.get().valid) {
        cosim::check(st, "fe", "Frontend", seed, c, "cdb_txn", ref.io_cleanDB_bits_hnTxnID,
                     dut.clean_db_s3.get().bits.hnTxnID, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "cdb_dv0", ref.io_cleanDB_bits_dataVec_0,
                     dut.clean_db_s3.get().bits.dataVec & 1, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "cdb_dv1", ref.io_cleanDB_bits_dataVec_1,
                     (dut.clean_db_s3.get().bits.dataVec >> 1) & 1, rp);
    }
    // read_dir
    cosim::check(st, "fe", "Frontend", seed, c, "rdir_v", ref.io_readDir_valid,
                 dut.read_dir.get().valid, rp);
    if (ref.io_readDir_valid && dut.read_dir.get().valid) {
        cosim::check(st, "fe", "Frontend", seed, c, "rdir_addr", ref.io_readDir_bits_addr,
                     dut.read_dir.get().bits.addr, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "rdir_db",
                     ref.io_readDir_bits_hnIdx_dirBank,
                     hnIdxDirBank(dut.read_dir.get().bits.hnIdx), rp);
        cosim::check(st, "fe", "Frontend", seed, c, "rdir_ps",
                     ref.io_readDir_bits_hnIdx_pos_set,
                     hnIdxPosSet(dut.read_dir.get().bits.hnIdx), rp);
        cosim::check(st, "fe", "Frontend", seed, c, "rdir_pw",
                     ref.io_readDir_bits_hnIdx_pos_way,
                     hnIdxPosWay(dut.read_dir.get().bits.hnIdx), rp);
    }
    // cmt_task
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_v", ref.io_cmtTask_valid,
                 dut.cmt_task.get().valid, rp);
    if (ref.io_cmtTask_valid && dut.cmt_task.get().valid) CHECK_CMT(ref, dut, st, seed, c, rp);
    // fast_resp
    cosim::check(st, "fe", "Frontend", seed, c, "fresp_v", ref.io_fastResp_valid,
                 dut.fast_resp.get().valid, rp);
    if (ref.io_fastResp_valid && dut.fast_resp.get().valid) {
        const auto& b = dut.fast_resp.get().bits;
        cosim::check(st, "fe", "Frontend", seed, c, "fresp_dbid", ref.io_fastResp_bits_DBID,
                     b.dbid, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fresp_cbusy",
                     ref.io_fastResp_bits_CBusy, b.c_busy, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fresp_fwdst",
                     ref.io_fastResp_bits_FwdState, b.fwd_state, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fresp_resp", ref.io_fastResp_bits_Resp,
                     b.resp, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fresp_resperr",
                     ref.io_fastResp_bits_RespErr, b.resp_err, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fresp_opcode",
                     ref.io_fastResp_bits_Opcode, b.opcode, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fresp_txnid",
                     ref.io_fastResp_bits_TxnID, b.txn_id, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fresp_srcid",
                     ref.io_fastResp_bits_SrcID, b.src_id, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fresp_tgtid",
                     ref.io_fastResp_bits_TgtID, b.tgt_id, rp);
        cosim::check(st, "fe", "Frontend", seed, c, "fresp_qos", ref.io_fastResp_bits_QoS,
                     b.qos, rp);
    }
    // pos_resp_vec
    for (int s = 0; s < 4; ++s) {
        const bool rv = s == 0 ? ref.io_posRespVec_0_valid
                        : s == 1 ? ref.io_posRespVec_1_valid
                        : s == 2 ? ref.io_posRespVec_2_valid
                                 : ref.io_posRespVec_3_valid;
        cosim::check(st, "fe", "Frontend", seed, c,
                     (std::string("posr_v") + std::to_string(s)).c_str(), rv,
                     dut.pos_resp_vec.get()[s].valid, rp);
        if (rv && dut.pos_resp_vec.get()[s].valid) {
            const uint32_t rw = s == 0 ? ref.io_posRespVec_0_bits
                                : s == 1 ? ref.io_posRespVec_1_bits
                                : s == 2 ? ref.io_posRespVec_2_bits
                                         : ref.io_posRespVec_3_bits;
            cosim::check(st, "fe", "Frontend", seed, c,
                         (std::string("posr_w") + std::to_string(s)).c_str(), rw,
                         dut.pos_resp_vec.get()[s].bits, rp);
        }
    }
    // alr_use_pos / working
    cosim::check(st, "fe", "Frontend", seed, c, "alrpos", ref.io_alrUsePoS,
                 dut.alr_use_pos.get(), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "working", ref.io_working,
                 dut.working.get(), rp);
    // get_addr_result（与地址表核对 —— 两侧各自从同一 hnidx 计算，真值表为准）
    for (int i = 0; i < 3; ++i) {
        const uint64_t rv = i == 0 ? ref.io_getAddrVec_0_result_addr
                            : i == 1 ? ref.io_getAddrVec_1_result_addr
                                     : ref.io_getAddrVec_2_result_addr;
        cosim::check(st, "fe", "Frontend", seed, c,
                     (std::string("gaddr") + std::to_string(i)).c_str(), rv,
                     dut.get_addr_result.get()[i], rp);
    }
}

}  // namespace
namespace {

void CHECK_CMT(VFrontend& ref, Frontend& dut, cosim::Stats& st, uint32_t seed, uint64_t c,
               cosim::Replay& rp) {
    const auto& t = dut.cmt_task.get().bits;
    const uint32_t task = t.task, cmt = t.cmt;
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_txn", ref.io_cmtTask_bits_hnTxnID,
                 t.hnTxnID, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_qos", ref.io_cmtTask_bits_qos, t.qos,
                 rp);
    // chi
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_fromLAN",
                 ref.io_cmtTask_bits_chi_fromLAN, t.chi.fromLAN, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_nodeId",
                 ref.io_cmtTask_bits_chi_nodeId, t.chi.nodeId, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_channel",
                 ref.io_cmtTask_bits_chi_channel, t.chi.channel, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_opcode",
                 ref.io_cmtTask_bits_chi_opcode, t.chi.opcode, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_order",
                 ref.io_cmtTask_bits_chi_order, t.chi.order, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_eca",
                 ref.io_cmtTask_bits_chi_expCompAck, t.chi.expCompAck, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_snpattr",
                 ref.io_cmtTask_bits_chi_snpAttr, t.chi.snpAttr, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_snoopme",
                 ref.io_cmtTask_bits_chi_snoopMe, t.chi.snoopMe, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_dv0",
                 ref.io_cmtTask_bits_chi_dataVec_0, t.chi.dataVec & 1, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_dv1",
                 ref.io_cmtTask_bits_chi_dataVec_1, (t.chi.dataVec >> 1) & 1, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_txnid",
                 ref.io_cmtTask_bits_chi_txnID, t.chi.txnID, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_alloc",
                 ref.io_cmtTask_bits_chi_memAttr_allocate, t.chi.memAllocate(), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_cache",
                 ref.io_cmtTask_bits_chi_memAttr_cacheable, t.chi.memCacheable(), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_dev",
                 ref.io_cmtTask_bits_chi_memAttr_device, t.chi.memDevice(), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_ewa",
                 ref.io_cmtTask_bits_chi_memAttr_ewa, t.chi.memEwa(), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_size",
                 ref.io_cmtTask_bits_chi_size, t.chi.size, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_fwdnid",
                 ref.io_cmtTask_bits_chi_fwdNID, t.chi.fwdNID, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_fwdtxn",
                 ref.io_cmtTask_bits_chi_fwdTxnID, t.chi.fwdTxnID, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_r2s",
                 ref.io_cmtTask_bits_chi_retToSrc, t.chi.retToSrc, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_chi_tolan",
                 ref.io_cmtTask_bits_chi_toLAN, t.chi.toLAN, rp);
    // dir
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_dir_lway",
                 ref.io_cmtTask_bits_dir_llc_wayOH, t.dir.llc.wayOH, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_dir_lhit",
                 ref.io_cmtTask_bits_dir_llc_hit, t.dir.llc.hit, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_dir_lmeta",
                 ref.io_cmtTask_bits_dir_llc_metaVec_0_state, t.dir.llc.meta, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_dir_sway",
                 ref.io_cmtTask_bits_dir_sf_wayOH, t.dir.sf.wayOH, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_dir_shit",
                 ref.io_cmtTask_bits_dir_sf_hit, t.dir.sf.hit, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_dir_smeta",
                 ref.io_cmtTask_bits_dir_sf_metaVec_0_state, t.dir.sf.meta, rp);
    // alr / ds / decList / task / cmt
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_alr_reqdb",
                 ref.io_cmtTask_bits_alr_reqDB, t.alr.reqDB, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_alr_sdata",
                 ref.io_cmtTask_bits_alr_sData, t.alr.sData, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_alr_sdbid",
                 ref.io_cmtTask_bits_alr_sDBID, t.alr.sDBID, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_ds_bank", ref.io_cmtTask_bits_ds_bank,
                 t.ds.bank, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_ds_idx", ref.io_cmtTask_bits_ds_idx,
                 t.ds.idx, rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_decl2", ref.io_cmtTask_bits_decList_2,
                 t.decList[2], rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_decl1", ref.io_cmtTask_bits_decList_1,
                 t.decList[1], rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_decl0", ref.io_cmtTask_bits_decList_0,
                 t.decList[0], rp);
    namespace dc = dectab;
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_snoop", ref.io_cmtTask_bits_task_snoop,
                 dc::tcSnoop(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_read", ref.io_cmtTask_bits_task_read,
                 dc::tcRead(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_dless",
                 ref.io_cmtTask_bits_task_dataless, dc::tcDataless(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_write",
                 ref.io_cmtTask_bits_task_write, dc::tcWrite(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_repl",
                 ref.io_cmtTask_bits_task_dataOp_repl, dc::tcOpRepl(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_opread",
                 ref.io_cmtTask_bits_task_dataOp_read, dc::tcOpRead(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_send",
                 ref.io_cmtTask_bits_task_dataOp_send, dc::tcOpSend(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_save",
                 ref.io_cmtTask_bits_task_dataOp_save, dc::tcOpSave(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_merge",
                 ref.io_cmtTask_bits_task_dataOp_merge, dc::tcOpMerge(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_opcode",
                 ref.io_cmtTask_bits_task_opcode, dc::tcOpcode(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_needdb",
                 ref.io_cmtTask_bits_task_needDB, dc::tcNeedDB(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_retdbid",
                 ref.io_cmtTask_bits_task_returnDBID, dc::tcReturnDBID(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_eca",
                 ref.io_cmtTask_bits_task_expCompAck, dc::tcExpCompAck(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_dmt", ref.io_cmtTask_bits_task_doDMT,
                 dc::tcDoDMT(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_r2s",
                 ref.io_cmtTask_bits_task_retToSrc, dc::tcRetToSrc(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_snptgt",
                 ref.io_cmtTask_bits_task_snpTgt, dc::tcSnpTgt(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_t_full",
                 ref.io_cmtTask_bits_task_fullSize, dc::tcFullSize(task), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_wsrc", ref.io_cmtTask_bits_cmt_wriSRC,
                 dc::ccWriSRC(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_wsnp", ref.io_cmtTask_bits_cmt_wriSNP,
                 dc::ccWriSNP(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_wllc", ref.io_cmtTask_bits_cmt_wriLLC,
                 dc::ccWriLLC(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_srcv",
                 ref.io_cmtTask_bits_cmt_srcValid, dc::ccSrcValid(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_snpv",
                 ref.io_cmtTask_bits_cmt_snpValid, dc::ccSnpValid(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_llcst",
                 ref.io_cmtTask_bits_cmt_llcState, dc::ccLlcState(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_repl",
                 ref.io_cmtTask_bits_cmt_dataOp_repl, dc::ccOpRepl(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_opread",
                 ref.io_cmtTask_bits_cmt_dataOp_read, dc::ccOpRead(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_send",
                 ref.io_cmtTask_bits_cmt_dataOp_send, dc::ccOpSend(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_save",
                 ref.io_cmtTask_bits_cmt_dataOp_save, dc::ccOpSave(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_merge",
                 ref.io_cmtTask_bits_cmt_dataOp_merge, dc::ccOpMerge(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_wait",
                 ref.io_cmtTask_bits_cmt_waitSecDone, dc::ccWaitSecDone(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_sresp",
                 ref.io_cmtTask_bits_cmt_sendResp, dc::ccSendResp(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_sfwd",
                 ref.io_cmtTask_bits_cmt_sendfwdResp, dc::ccSendFwdResp(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_channel",
                 ref.io_cmtTask_bits_cmt_channel, dc::ccChannel(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_opcode",
                 ref.io_cmtTask_bits_cmt_opcode, dc::ccOpcode(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_resp", ref.io_cmtTask_bits_cmt_resp,
                 dc::ccResp(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_fwdresp",
                 ref.io_cmtTask_bits_cmt_fwdResp, dc::ccFwdResp(cmt), rp);
    cosim::check(st, "fe", "Frontend", seed, c, "cmt_c_full",
                 ref.io_cmtTask_bits_cmt_fullSize, dc::ccFullSize(cmt), rp);
}

}  // namespace
