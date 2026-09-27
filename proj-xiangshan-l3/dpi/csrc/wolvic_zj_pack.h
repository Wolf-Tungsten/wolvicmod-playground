#pragma once

// wolvic_zj_pack.h：WolvicZjBB DPI 边界的打包/解包共享实现。
//
// 布局与 dpi/sv/WolvicZjBB.sv 双侧一一对应（先列占高位；C++ 从 LSB 侧反向
// 顺序读/写）：CHI bits 复用 model/flit/xs_flit.h 的 pack()/unpack()。
// 供 wolvic_zj_dpi.cpp（emu 链接）与 verify/dpi/tb_wolvic_zjbb.cpp（壳冒烟
// 测试台）共用；改动必须与 SV 壳同步。

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "model/wolvic_zj_top.h"

// 与 svdpi.h 的 typedef 一致（自声明，免去 Verilator 头路径依赖）
typedef unsigned int svBitVecVal;

namespace wzj {

using namespace zj;
using wolvicmod::prefab::Valid;

constexpr int kInW = 1107;   // in_pack 位宽（SV 侧 $fatal 断言同步）
constexpr int kOutW = 1443;  // out_pack 位宽
using InPack = std::array<uint64_t, (kInW + 63) / 64>;
using OutPack = std::array<uint64_t, (kOutW + 63) / 64>;

// 模型单例：构造态 = 复位完成态（P4b 对齐语义）
inline WolvicZjTop& model() {
    static WolvicZjTop* inst = [] {
        auto* m = new WolvicZjTop();
        m->elaborate();
        m->ci.set(0);
        return m;
    }();
    return *inst;
}

inline std::mutex& mtx() {
    static std::mutex m;
    return m;
}

// svBitVecVal（uint32，LSW 在前）↔ uint64 字数组。Verilator 侧缓冲 =
// ceil(nbits/32) 个 uint32——必须按位宽截，不能按 uint64 字数组翻倍读/写
template <size_t N>
inline std::array<uint64_t, N> fromSv(const svBitVecVal* v, int nbits) {
    std::array<uint64_t, N> w{};
    const int n32 = (nbits + 31) / 32;
    for (int i = 0; i < n32; ++i)
        w[i >> 1] |= uint64_t(v[i]) << (32 * (i & 1));
    return w;
}
template <size_t N>
inline void toSv(svBitVecVal* v, const std::array<uint64_t, N>& w, int nbits) {
    const int n32 = (nbits + 31) / 32;
    for (int i = 0; i < n32; ++i)
        v[i] = uint32_t(w[i >> 1] >> (32 * (i & 1)));
}

inline void printHex(FILE* f, const uint64_t* w, int nbits) {
    for (int b = nbits; b > 0; b -= 64) {
        const int word = (b - 1) >> 6;
        if (b == nbits && (nbits & 63))
            std::fprintf(f, "%llx", (unsigned long long)(w[word] & ((uint64_t{1} << (nbits & 63)) - 1)));
        else
            std::fprintf(f, "%016llx", (unsigned long long)w[word]);
    }
}

// ---- 输入解包：顺序 = SV in_pack 拼接的逆序（LSB 先行） ----
inline void unpackInputs(WolvicZjTop& m, const InPack& w) {
    int b = 0;
    const auto get = [&](int n) { const uint64_t v = getBits(w, b + n - 1, b); b += n; return v; };

    // cfgAXI 返回
    Valid<axi::RFlit> cfg_r{};
    cfg_r.bits.last = get(1);
    cfg_r.bits.resp = get(2);
    cfg_r.bits.data = getWide<4>(w, b); b += 256;
    cfg_r.bits.id = get(3);
    cfg_r.valid = get(1);
    const bool cfg_arready = get(1);
    Valid<axi::BFlit> cfg_b{};
    cfg_b.bits.resp = get(2);
    cfg_b.bits.id = get(3);
    cfg_b.valid = get(1);
    const bool cfg_wready = get(1);
    const bool cfg_awready = get(1);
    // memAXI 返回
    Valid<axi::RFlit> mem_r{};
    mem_r.bits.last = get(1);
    mem_r.bits.resp = get(2);
    mem_r.bits.data = getWide<4>(w, b); b += 256;
    mem_r.bits.id = get(6);
    mem_r.valid = get(1);
    const bool mem_arready = get(1);
    Valid<axi::BFlit> mem_b{};
    mem_b.bits.resp = get(2);
    mem_b.bits.id = get(6);
    mem_b.valid = get(1);
    const bool mem_wready = get(1);
    const bool mem_awready = get(1);
    // L2 CHI
    const bool rx_snp_rdy = get(1);
    const bool rx_dat_rdy = get(1);
    const bool rx_rsp_rdy = get(1);
    Valid<xs::CHIDAT> tx_dat{};
    tx_dat.bits = xs::CHIDAT::unpack(getWide<(xs::CHIDAT::kWidth + 63) / 64>(w, b));
    b += xs::CHIDAT::kWidth;
    tx_dat.valid = get(1);
    Valid<xs::CHIRSP> tx_rsp{};
    tx_rsp.bits = xs::CHIRSP::unpack(getWide<(xs::CHIRSP::kWidth + 63) / 64>(w, b));
    b += xs::CHIRSP::kWidth;
    tx_rsp.valid = get(1);
    Valid<xs::CHIREQ> tx_req{};
    tx_req.bits = xs::CHIREQ::unpack(getWide<(xs::CHIREQ::kWidth + 63) / 64>(w, b));
    b += xs::CHIREQ::kWidth;
    tx_req.valid = get(1);
    if (b != kInW) std::fprintf(stderr, "[wolvic-zj] in_pack width drift: %d != %d\n", b, kInW);

    m.chi_tx_req.set(tx_req);
    m.chi_tx_rsp.set(tx_rsp);
    m.chi_tx_dat.set(tx_dat);
    m.chi_rx_rsp_rdy.set(rx_rsp_rdy);
    m.chi_rx_dat_rdy.set(rx_dat_rdy);
    m.chi_rx_snp_rdy.set(rx_snp_rdy);
    m.mem_aw_rdy.set(mem_awready);
    m.mem_w_rdy.set(mem_wready);
    m.mem_ar_rdy.set(mem_arready);
    m.mem_b.set(mem_b);
    m.mem_r.set(mem_r);
    m.cfg_aw_rdy.set(cfg_awready);
    m.cfg_w_rdy.set(cfg_wready);
    m.cfg_ar_rdy.set(cfg_arready);
    m.cfg_b.set(cfg_b);
    m.cfg_r.set(cfg_r);
}

// ---- 输出打包：顺序 = SV WolvicZjOut 声明的逆序（LSB 先行） ----
inline OutPack packOutputs(const WolvicZjTop& m) {
    OutPack w{};
    int b = 0;
    const auto put = [&](uint64_t v, int n) { setBits(w, b + n - 1, b, v); b += n; };
    const auto putWide = [&](const std::array<uint64_t, 4>& d) { setWide(w, b, d); b += 256; };
    const auto putAxOut = [&](const Valid<axi::AWFlit>& aw, const Valid<axi::WFlit>& wd,
                              bool b_rdy, const Valid<axi::ARFlit>& ar, bool r_rdy, int idw) {
        put(r_rdy, 1);
        put(ar.bits.qos, 4); put(ar.bits.prot, 3); put(ar.bits.cache, 4);
        put(ar.bits.lock, 1); put(ar.bits.burst, 2); put(ar.bits.size, 3);
        put(ar.bits.len, 8); put(ar.bits.addr, 49); put(ar.bits.id, idw);
        put(ar.valid, 1);
        put(b_rdy, 1);
        put(wd.bits.last, 1); put(wd.bits.strb, 32); putWide(wd.bits.data);
        put(wd.valid, 1);
        put(aw.bits.qos, 4); put(aw.bits.prot, 3); put(aw.bits.cache, 4);
        put(aw.bits.lock, 1); put(aw.bits.burst, 2); put(aw.bits.size, 3);
        put(aw.bits.len, 8); put(aw.bits.addr, 49); put(aw.bits.id, idw);
        put(aw.valid, 1);
    };

    // cfgAXI（id 3b）
    putAxOut(m.cfg_aw.get(), m.cfg_w.get(), m.cfg_b_rdy.get(), m.cfg_ar.get(),
             m.cfg_r_rdy.get(), 3);
    // memAXI（id 6b）
    putAxOut(m.mem_aw.get(), m.mem_w.get(), m.mem_b_rdy.get(), m.mem_ar.get(),
             m.mem_r_rdy.get(), 6);
    // L2 CHI
    const auto snp = m.chi_rx_snp.get();
    const auto snpBits = snp.bits.pack();
    setWide(w, b, snpBits); b += xs::CHISNP::kWidth;
    put(snp.valid, 1);
    const auto dat = m.chi_rx_dat.get();
    const auto datBits = dat.bits.pack();
    setWide(w, b, datBits); b += xs::CHIDAT::kWidth;
    put(dat.valid, 1);
    const auto rsp = m.chi_rx_rsp.get();
    const auto rspBits = rsp.bits.pack();
    setWide(w, b, rspBits); b += xs::CHIRSP::kWidth;
    put(rsp.valid, 1);
    put(m.chi_tx_dat_rdy.get(), 1);
    put(m.chi_tx_rsp_rdy.get(), 1);
    put(m.chi_tx_req_rdy.get(), 1);
    if (b != kOutW) std::fprintf(stderr, "[wolvic-zj] out_pack width drift: %d != %d\n", b, kOutW);
    return w;
}

}  // namespace wzj
