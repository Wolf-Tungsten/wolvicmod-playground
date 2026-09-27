// P4a/P3 ZjL3 组装单测：L2 CHI 边界 → adapter → CcSocket → 环 → HomeShell →
// DongJiang 全量模型（P3 已替换 P2 的 HnfStub 行为桩）：cacheable ReadOnce 经
// 真实通路收回 ReadReceipt + CompData（LLC miss → 内存模型供数 → LLC 分配），
// 并回 CompAck；device REQ 经 RnRouter 落 defaultHni，穿越 HiNode 桥出
// cfgAXI AR，R 应答经桥 → 环 → CC 回到 L2。

#include <cstdint>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <model/zj_l3.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace zj;
using namespace zj::chi;
using namespace zj::xs;
using namespace zj::axi;
using namespace prefabtest;

namespace {

void tieOff(ZjL3& top) {
    top.ci.set(0);
    // 未建模站点：无注入、不消费
    top.n3_rx_req.set(Valid<RReqFlit>{});
    top.n3_rx_resp.set(Valid<RespFlit>{});
    top.n3_rx_data.set(Valid<DataFlit>{});
    top.n3_tx_resp_rdy.set(false);
    top.n3_tx_data_rdy.set(false);
    // AXI 边界：地址/数据通道全收，b/r 无流量
    top.mem_aw_rdy.set(true);
    top.mem_w_rdy.set(true);
    top.mem_ar_rdy.set(true);
    top.mem_b.set(Valid<BFlit>{});
    top.mem_r.set(Valid<RFlit>{});
    top.cfg_aw_rdy.set(true);
    top.cfg_w_rdy.set(true);
    top.cfg_ar_rdy.set(true);
    top.cfg_b.set(Valid<BFlit>{});
    top.cfg_r.set(Valid<RFlit>{});
    // L2 侧默认：无 tx 流量、rx 全收
    top.chi_tx_req.set(Valid<CHIREQ>{});
    top.chi_tx_rsp.set(Valid<CHIRSP>{});
    top.chi_tx_dat.set(Valid<CHIDAT>{});
    top.chi_rx_rsp_rdy.set(true);
    top.chi_rx_dat_rdy.set(true);
    top.chi_rx_snp_rdy.set(true);
}

// 空跑至多 n 拍（每拍 cycle+comb），pred 满足即返回 true
template <class Pred>
bool runUntil(ZjL3& top, int n, Pred pred) {
    for (int i = 0; i < n; ++i) {
        cycle(top);
        comb(top);
        if (pred()) return true;
    }
    return false;
}

TEST_CASE("ZjL3 通路：ReadOnce 经 DongJiang 全量模型收回 ReadReceipt/CompData 并完成 CompAck") {
    ZjL3 top;
    top.elaborate();
    tieOff(top);

    // 内存模型：memAR → 4 拍后回 2 beat R（256b/beat，64B 行）
    struct {
        std::deque<std::pair<uint64_t, uint8_t>> q;  // (due, id)
        Valid<RFlit> pend{false, {}};
        uint32_t beat = 0;
        uint64_t c    = 0;
        void drive(ZjL3& t) {
            if (!pend.valid && !q.empty() && q.front().first <= c) {
                pend.valid = true;
                pend.bits.id = q.front().second;
                for (int i = 0; i < 4; ++i)
                    pend.bits.data[i] = 0xDEADBEEF00000000ull | (uint64_t)beat << 32 | i;
                pend.bits.last = beat == 1;
            }
            t.mem_r.set(pend);
        }
        void sample(ZjL3& t) {
            if (t.mem_ar.get().valid && t.mem_ar_rdy.get())
                q.push_back({c + 4, t.mem_ar.get().bits.id});
        }
        void consume(ZjL3& t) {  // 时钟沿后调用
            if (pend.valid && t.mem_r_rdy.get()) {
                if (pend.bits.last) {
                    q.pop_front();
                    beat = 0;
                } else {
                    ++beat;
                }
                pend = {false, {}};
            }
        }
    } mem;
    bool got_ar = false, got_receipt = false;
    uint32_t dat_beats = 0;

    // DongJiang 目录 SRAM 上电横扫约 8.2k 拍，期间 readDir 反压；给足窗口
    Valid<CHIREQ> req;
    req.valid                   = true;
    req.bits.opcode             = 0x03;  // ReadOnce
    req.bits.addr               = 0x80000000ULL;
    req.bits.tgt_id             = 0;
    req.bits.src_id             = 0x08;
    req.bits.txn_id             = 0x5A;
    req.bits.size               = 6;
    req.bits.order              = 3;  // 合法 ReadOnce 必须 order=3 且 eca=1（译码表实证）
    req.bits.exp_comp_ack       = true;
    req.bits.mem_attr_cacheable = true;
    req.bits.mem_attr_allocate  = true;
    req.bits.mem_attr_ewa       = true;
    top.chi_tx_req.set(req);

    bool consumed = false, ack_sent = false;
    for (uint64_t c = 0; c < 20000; ++c) {
        mem.c = c;
        mem.drive(top);
        comb(top);
        // 采样
        if (top.chi_tx_req_rdy.get() && req.valid) consumed = true;
        if (top.mem_ar.get().valid) {
            CHECK(top.mem_ar.get().bits.addr == 0x80000000ULL);
            got_ar = true;
        }
        if (top.chi_rx_rsp.get().valid) {
            CHECK(top.chi_rx_rsp.get().bits.opcode == 0x08);  // ReadReceipt
            CHECK(top.chi_rx_rsp.get().bits.txn_id == 0x5A);
            got_receipt = true;
        }
        if (top.chi_rx_dat.get().valid) {
            CHECK(top.chi_rx_dat.get().bits.opcode == 0x04);  // CompData
            CHECK(top.chi_rx_dat.get().bits.txn_id == 0x5A);
            CHECK(top.chi_rx_dat.get().bits.tgt_id == 0x08);
            CHECK((top.chi_rx_dat.get().bits.data[0] >> 32) == 0xDEADBEEF);
            ++dat_beats;
        }
        CHECK(!top.cc_tx_req.get().valid);
        mem.sample(top);
        edge(top);
        // 沿后动作
        if (consumed && req.valid) {
            top.chi_tx_req.set(Valid<CHIREQ>{});
            req.valid = false;
        }
        if (dat_beats == 2 && !ack_sent) {  // 全行到齐后回一次 CompAck
            ack_sent = true;
        }
        mem.consume(top);
        if (ack_sent) {
            Valid<CHIRSP> ack;
            ack.valid = true;
            ack.bits.opcode = 0x02;  // CompAck
            ack.bits.txn_id = 0x5A;
            top.chi_tx_rsp.set(ack);
            comb(top);
            if (top.chi_tx_rsp_rdy.get()) {
                top.chi_tx_rsp.set(Valid<CHIRSP>{});
                ack_sent = false;
                goto done;
            }
            edge(top);
        }
    }
done:
    CHECK(consumed);
    CHECK(got_receipt);
    CHECK(got_ar);
    CHECK(dat_beats == 2);
}

TEST_CASE("ZjL3 通路：device REQ 经 RnRouter 落 defaultHni，穿 HI 桥出 cfgAXI 并回数") {
    ZjL3 top;
    top.elaborate();
    tieOff(top);

    Valid<CHIREQ> req;
    req.valid            = true;
    req.bits.opcode      = 0x04;              // ReadNoSnp（HI 桥只收 ReadNoSnp/WriteNoSnpPtl）
    req.bits.addr        = 0x38000008ULL;     // addr[43:20]≠0 → 非 CC device 窗口
    req.bits.tgt_id      = 0;
    req.bits.src_id      = 0x08;
    req.bits.txn_id      = 0x7B;
    req.bits.size        = 2;
    req.bits.mem_attr_device = true;
    top.chi_tx_req.set(req);

    comb(top);
    bool consumed = runUntil(top, 20, [&] { return top.chi_tx_req_rdy.get(); });
    CHECK(consumed);
    top.chi_tx_req.set(Valid<CHIREQ>{});

    // HI 桥应发出 cfgAXI AR（defaultHni gid4 → 桥内 CM → AR）
    bool got_ar = runUntil(top, 200, [&] { return top.cfg_ar.get().valid; });
    CHECK(got_ar);
    CHECK(top.cfg_ar.get().bits.addr == 0x38000008ULL);
    CHECK(top.cfg_ar.get().bits.size == 2);
    const uint8_t arid = top.cfg_ar.get().bits.id;
    edge(top);  // AR fire（tieOff 中 cfg_ar_rdy 恒 true）

    // 给 R：CompData 经桥 → 环 → CC socket → adapter 回到 L2
    Valid<RFlit> r;
    r.valid        = true;
    r.bits.id      = arid;
    r.bits.last    = true;
    r.bits.data[0] = 0xCAFEBABE;
    top.cfg_r.set(r);
    comb(top);
    edge(top);
    top.cfg_r.set(Valid<RFlit>{});

    bool got_dat = runUntil(top, 200, [&] { return top.chi_rx_dat.get().valid; });
    CHECK(got_dat);
    CHECK(top.chi_rx_dat.get().bits.opcode == 0x04);  // CompData
    CHECK(top.chi_rx_dat.get().bits.txn_id == 0x7B);
    CHECK(top.chi_rx_dat.get().bits.tgt_id == 0x08);
    CHECK(top.chi_rx_dat.get().bits.data[0] == 0xCAFEBABE);
}

}  // namespace
