// flit 类型层单测（实施计划第 1 步验收）：
//   1. 位段辅助函数 getBits/setBits/getWide/setWide 与逐位参考实现对拍（含跨字）；
//   2. zhujiang 五件套 + xscache 四件套 pack/unpack 全字段随机往返；
//   3. 总宽钉死在 RTL 实测值（生成 RTL 端口核实：105/128/66/113/375 与 118/66/367/102）；
//   4. 关键字段偏移抽查（QoS 恒 [3:0]、Addr/Data 跨字位置）；
//   5. DBID 12↔16 适配助手、NodeId.router、FlitType 编码、RingSlot 默认态；
//   6. config traits 可配置性：备选 MiniCfg 实例化后位宽随参数变化且往返正确。

#include <array>
#include <cstdint>
#include <random>

#include <doctest/doctest.h>
#include <model/ring_slot.h>
#include <model/xs_flit.h>
#include <model/zj_flit.h>

using namespace zj;

namespace {

std::mt19937_64 rng(20260925);

uint64_t rnd(int bits) {
    const uint64_t m = (bits >= 64) ? ~uint64_t{0} : ((uint64_t{1} << bits) - 1);
    return rng() & m;
}

// ---- 逐位参考实现（与被测的字优化实现交叉验证）----
template <size_t N>
uint64_t refGet(const std::array<uint64_t, N>& w, int hi, int lo) {
    uint64_t v = 0;
    for (int i = 0; i <= hi - lo; i++)
        v |= ((w[(lo + i) >> 6] >> ((lo + i) & 63)) & 1) << i;
    return v;
}

template <size_t N>
void refSet(std::array<uint64_t, N>& w, int hi, int lo, uint64_t v) {
    for (int i = 0; i <= hi - lo; i++) {
        if ((v >> i) & 1)
            w[(lo + i) >> 6] |= uint64_t{1} << ((lo + i) & 63);
        else
            w[(lo + i) >> 6] &= ~(uint64_t{1} << ((lo + i) & 63));
    }
}

TEST_CASE("bit_pack: getBits/setBits 与逐位参考对拍（随机偏移/宽度/初值）") {
    for (int iter = 0; iter < 20000; iter++) {
        std::array<uint64_t, 6> a{}, b{};
        for (auto& x : a) x = rng();
        b = a;
        const int lo = rng() % 380;
        const int n  = 1 + rng() % 64;
        const int hi = lo + n - 1;
        if (hi >= 384) continue;
        const uint64_t v = rng();
        setBits(a, hi, lo, v);
        refSet(b, hi, lo, v);
        CHECK(a == b);
        CHECK(getBits(a, hi, lo) == refGet(a, hi, lo));
        CHECK(getBits(a, hi, lo) == (n == 64 ? v : (v & ((uint64_t{1} << n) - 1))));
    }
}

TEST_CASE("bit_pack: getWide/setWide 非字对齐往返 + 邻位无污染") {
    for (int iter = 0; iter < 2000; iter++) {
        std::array<uint64_t, 8> w{}, golden{};
        for (auto& x : w) x = rng();
        golden = w;
        const int off = rng() % 200;  // 任意位偏移
        std::array<uint64_t, 3> f{rng(), rng(), rng()};
        setWide(w, off, f);
        CHECK(getWide<3>(w, off) == f);
        // 参考：区域内逐位一致，区域外逐位不变
        for (int i = 0; i < 192; i++) CHECK(((w[(off + i) >> 6] >> ((off + i) & 63)) & 1) == ((f[i >> 6] >> (i & 63)) & 1));
        for (int i = 0; i < off; i++) CHECK(((w[i >> 6] >> (i & 63)) & 1) == ((golden[i >> 6] >> (i & 63)) & 1));
        for (int i = off + 192; i < 512; i++) CHECK(((w[i >> 6] >> (i & 63)) & 1) == ((golden[i >> 6] >> (i & 63)) & 1));
    }
}

// ---- 位宽钉死（RTL 实测）----
static_assert(chi::RReqFlit::kWidth == 105);
static_assert(chi::HReqFlit::kWidth == 128);
static_assert(chi::RespFlit::kWidth == 66);
static_assert(chi::SnoopFlit::kWidth == 113);
static_assert(chi::DataFlit::kWidth == 375);
static_assert(chi::kRingReqBits == 105 && chi::kRingRspBits == 66);
static_assert(chi::kRingDatBits == 375 && chi::kRingHrqBits == 128);
static_assert(xs::CHIREQ::kWidth == 118);
static_assert(xs::CHIRSP::kWidth == 66);
static_assert(xs::CHIDAT::kWidth == 367);
static_assert(xs::CHISNP::kWidth == 102);

TEST_CASE("flit 位宽 = RTL 实测（static_assert 已编译期钉死，此处 doctest 可见）") {
    CHECK(chi::RReqFlit::kWidth == 105);
    CHECK(chi::HReqFlit::kWidth == 128);
    CHECK(chi::RespFlit::kWidth == 66);
    CHECK(chi::SnoopFlit::kWidth == 113);
    CHECK(chi::DataFlit::kWidth == 375);
    CHECK(xs::CHIREQ::kWidth == 118);
    CHECK(xs::CHIRSP::kWidth == 66);
    CHECK(xs::CHIDAT::kWidth == 367);
    CHECK(xs::CHISNP::kWidth == 102);
}

// ---- 随机化填充 ----
chi::RReqFlit randRReq() {
    chi::RReqFlit f;
    f.qos = rnd(4);
    f.tgt_id = rnd(11);
    f.src_id = rnd(11);
    f.txn_id = rnd(12);
    f.opcode = rnd(7);
    f.size = rnd(3);
    f.addr = rnd(48);
    f.order = rnd(2);
    f.mem_attr = rnd(4);
    f.snp_attr = rnd(1);
    f.excl = rnd(1);
    f.exp_comp_ack = rnd(1);
    return f;
}

TEST_CASE("zj ReqFlit: pack/unpack 随机往返 + RReq 不带 DMT 字段") {
    for (int i = 0; i < 1000; i++) {
        const auto f = randRReq();
        CHECK(chi::RReqFlit::unpack(f.pack()) == f);
        chi::HReqFlit h;
        h.qos = rnd(4);
        h.tgt_id = rnd(11);
        h.src_id = rnd(11);
        h.txn_id = rnd(12);
        h.return_nid = rnd(11);
        h.return_txn_id = rnd(12);
        h.opcode = rnd(7);
        h.size = rnd(3);
        h.addr = rnd(48);
        h.order = rnd(2);
        h.mem_attr = rnd(4);
        h.snp_attr = rnd(1);
        h.excl = rnd(1);
        h.exp_comp_ack = rnd(1);
        CHECK(chi::HReqFlit::unpack(h.pack()) == h);
    }
    // RReq 的 return_* 不进入位流：置位后打包与清零打包一致
    auto f = randRReq();
    const auto clean = f.pack();
    f.return_nid = 0x7FF;
    f.return_txn_id = 0xFFF;
    CHECK(f.pack() == clean);
}

TEST_CASE("zj RespFlit/SnoopFlit/DataFlit: pack/unpack 随机往返") {
    for (int i = 0; i < 1000; i++) {
        chi::RespFlit r;
        r.qos = rnd(4); r.tgt_id = rnd(11); r.src_id = rnd(11); r.txn_id = rnd(12);
        r.opcode = rnd(5); r.resp_err = rnd(2); r.resp = rnd(3);
        r.fwd_state = rnd(3); r.c_busy = rnd(3); r.dbid = rnd(12);
        CHECK(chi::RespFlit::unpack(r.pack()) == r);

        chi::SnoopFlit s;
        s.qos = rnd(4); s.tgt_id = rnd(11); s.src_id = rnd(11); s.txn_id = rnd(12);
        s.fwd_nid = rnd(11); s.fwd_txn_id = rnd(12); s.opcode = rnd(5);
        s.addr = rnd(45); s.do_not_go_to_sd = rnd(1); s.ret_to_src = rnd(1);
        CHECK(chi::SnoopFlit::unpack(s.pack()) == s);

        chi::DataFlit d;
        d.qos = rnd(4); d.tgt_id = rnd(11); d.src_id = rnd(11); d.txn_id = rnd(12);
        d.home_nid = rnd(11); d.opcode = rnd(4); d.resp_err = rnd(2); d.resp = rnd(3);
        d.data_source = rnd(8); d.c_busy = rnd(3); d.dbid = rnd(16);
        d.data_id = rnd(2); d.be = rnd(32);
        for (auto& x : d.data) x = rng();
        CHECK(chi::DataFlit::unpack(d.pack()) == d);
    }
}

TEST_CASE("zj flit 字段偏移抽查（对照 Flit.scala 声明序推出的 LSB 布局）") {
    chi::RReqFlit f;  // 全零
    f.qos = 0xA;
    CHECK(f.pack()[0] == 0xA);          // QoS [3:0]
    f = {}; f.tgt_id = 1;
    CHECK(f.pack()[0] == 1 << 4);       // TgtID [14:4]
    f = {}; f.txn_id = 1;
    CHECK(f.pack()[0] == 1 << 26);      // TxnID [37:26]
    f = {}; f.addr = 1;
    CHECK(f.pack()[0] == uint64_t{1} << 48);  // Addr [95:48] 起始于跨字点
    f = {}; f.exp_comp_ack = true;      // 最高位 [104]
    CHECK(getBits(f.pack(), 104, 104) == 1);
    CHECK(getBits(f.pack(), 103, 0) == 0);

    chi::DataFlit d;
    d.data[0] = 1;                      // Data [374:119]，word0 占 [182:119]
    const auto w = d.pack();
    CHECK(getBits(w, 118, 0) == 0);
    CHECK(refGet(w, 182, 119) == 1);
    d = {}; d.dbid = 0xABCD;            // DBID [84:69]（16b）
    CHECK(getBits(d.pack(), 84, 69) == 0xABCD);

    // HReq 的 ReturnNID/ReturnTxnID 插在 TxnID 之上（[48:38]/[60:49]）
    chi::HReqFlit h;
    h.return_nid = 1;
    CHECK(getBits(h.pack(), 48, 38) == 1);
    h = {}; h.opcode = 1;
    CHECK(getBits(h.pack(), 67, 61) == 1);
}

TEST_CASE("xs flit: pack/unpack 随机往返") {
    for (int i = 0; i < 1000; i++) {
        xs::CHIREQ q;
        q.qos = rnd(4); q.tgt_id = rnd(11); q.src_id = rnd(11); q.txn_id = rnd(12);
        q.opcode = rnd(7); q.size = rnd(3); q.addr = rnd(48); q.order = rnd(2);
        q.mem_attr_allocate = rnd(1); q.mem_attr_cacheable = rnd(1);
        q.mem_attr_device = rnd(1); q.mem_attr_ewa = rnd(1);
        q.snp_attr = rnd(1); q.snoop_me = rnd(1); q.exp_comp_ack = rnd(1);
        q.mpam_part_id = rnd(9); q.rsvdc = rnd(4);
        CHECK(xs::CHIREQ::unpack(q.pack()) == q);

        xs::CHIRSP r;
        r.qos = rnd(4); r.tgt_id = rnd(11); r.src_id = rnd(11); r.txn_id = rnd(12);
        r.opcode = rnd(5); r.resp_err = rnd(2); r.resp = rnd(3);
        r.fwd_state = rnd(3); r.c_busy = rnd(3); r.dbid = rnd(12);
        CHECK(xs::CHIRSP::unpack(r.pack()) == r);

        xs::CHIDAT d;
        d.qos = rnd(4); d.tgt_id = rnd(11); d.src_id = rnd(11); d.txn_id = rnd(12);
        d.home_nid = rnd(11); d.opcode = rnd(4); d.resp_err = rnd(2); d.resp = rnd(3);
        d.data_source = rnd(4); d.c_busy = rnd(3); d.dbid = rnd(12);
        d.data_id = rnd(2); d.be = rnd(32);
        for (auto& x : d.data) x = rng();
        CHECK(xs::CHIDAT::unpack(d.pack()) == d);

        xs::CHISNP s;
        s.qos = rnd(4); s.src_id = rnd(11); s.txn_id = rnd(12);
        s.fwd_nid = rnd(11); s.fwd_txn_id = rnd(12); s.opcode = rnd(5);
        s.addr = rnd(45); s.do_not_go_to_sd = rnd(1); s.ret_to_src = rnd(1);
        CHECK(xs::CHISNP::unpack(s.pack()) == s);
    }
}

TEST_CASE("xs CHIREQ: memAttr 位序 = {allocate, cacheable, device, ewa}（桥 Cat 序）") {
    xs::CHIREQ q;
    q.mem_attr_ewa = true;
    CHECK(q.mem_attr() == 0b0001);
    q = {}; q.mem_attr_device = true;
    CHECK(q.mem_attr() == 0b0010);
    q = {}; q.mem_attr_allocate = true;
    CHECK(q.mem_attr() == 0b1000);
}

TEST_CASE("DBID 12↔16 适配（ZhuJiangBridge.scala:214,232）") {
    CHECK(xs::dbidXsToZj(0x0ABC) == 0x0ABC);   // 零扩展
    CHECK(xs::dbidZjToXs(0x00ABC & 0xFFFF) == 0x0ABC);
    CHECK(xs::dbidZjToXs(0xFABC) == 0x0ABC);   // 截断丢高 4 位（Chisel := 宽赋窄）
    CHECK(xs::dbidFitsXs(0x0FFF));
    CHECK(!xs::dbidFitsXs(0x1000));
    static_assert(xs::XsChiCfg::kDbidW == 12);
    static_assert(chi::ZjFlitCfg::kDbidW == 16);
}

TEST_CASE("NodeId / FlitType / RingSlot") {
    chi::NodeId p;
    p.nid = 9; p.aid = 0;
    CHECK(p.router() == 0x48);          // 拓扑 idx9 P 节点 nodeId=0x48
    CHECK(p.id() == 0x48);
    chi::NodeId cc;
    cc.nid = 1; cc.aid = 0;
    CHECK(cc.router() == 0x08);         // 拓扑 idx1 CC 节点
    CHECK(chi::kFlitReq == 0);
    CHECK(chi::kFlitRsp == 1);
    CHECK(chi::kFlitDat == 2);
    CHECK(chi::kFlitSnp == 3);
    CHECK(chi::kFlitErq == 4);

    RingSlot<chi::RReqFlit> slot;
    CHECK(!slot.valid);
    CHECK(!slot.rsvd_valid);
    CHECK(slot.flit == chi::RReqFlit{});
    slot.rsvd_payload = 0x7FF;          // niw=11 位
    CHECK(slot.rsvd_payload == 0x7FF);
}

// ---- 可配置性证明：备选 config 实例化 ----
struct MiniCfg {  // 假想小配置：niw=7（下限）、raw=44、dw=64
    static constexpr int kNodeNidBits = 4, kNodeAidBits = 3;
    static constexpr int kNiw = 7, kRaw = 44, kSaw = 41, kDw = 64, kBew = 8;
    static constexpr int kM = 0, kPb = 0, kE = 0, kR = 0, kS = 0, kY = 0;
    static constexpr int kDbidW = 16, kTxnW = 12;
};
using MiniReq  = chi::ReqFlitT<MiniCfg, false>;
using MiniData = chi::DataFlitT<MiniCfg>;
static_assert(MiniReq::kWidth == 35 + 2 * 7 + 44);          // 93
static_assert(MiniData::kWidth == 54 + 3 * 7 + 8 + 64);     // 147

TEST_CASE("config traits 可配置：MiniCfg 位宽随参数变化且往返正确") {
    CHECK(MiniReq::kWidth == 93);
    CHECK(MiniData::kWidth == 147);
    for (int i = 0; i < 1000; i++) {
        MiniReq f;
        f.qos = rnd(4); f.tgt_id = rnd(7); f.src_id = rnd(7); f.txn_id = rnd(12);
        f.opcode = rnd(7); f.size = rnd(3); f.addr = rnd(44); f.order = rnd(2);
        f.mem_attr = rnd(4); f.snp_attr = rnd(1); f.excl = rnd(1); f.exp_comp_ack = rnd(1);
        CHECK(MiniReq::unpack(f.pack()) == f);

        MiniData d;
        d.tgt_id = rnd(7); d.be = rnd(8); d.data[0] = rng();
        CHECK(MiniData::unpack(d.pack()) == d);
    }
}

}  // namespace
