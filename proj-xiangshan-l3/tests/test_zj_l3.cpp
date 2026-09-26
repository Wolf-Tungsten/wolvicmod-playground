// P4a ZjL3 组装单测：L2 CHI 边界 → adapter → CcSocket → 环 → HomeShell →
// HnfStub 全通路连通（cacheable REQ 路由到 bank0 HNF 桩并收到 Comp/CompData
// 回声）；device REQ 经 RnRouter 落 defaultHni，穿越 HiNode 桥出 cfgAXI AR，
// R 应答经桥 → 环 → CC 回到 L2。
// 桩的行为参数见 hnf_stub.h（P3 由 DongJiang 全量替换）。

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
    top.n3_rx_req.set(Dec<RReqFlit>{});
    top.n3_rx_resp.set(Dec<RespFlit>{});
    top.n3_rx_data.set(Dec<DataFlit>{});
    top.n3_tx_resp_rdy.set(false);
    top.n3_tx_data_rdy.set(false);
    // AXI 边界：地址/数据通道全收，b/r 无流量
    top.mem_aw_rdy.set(true);
    top.mem_w_rdy.set(true);
    top.mem_ar_rdy.set(true);
    top.mem_b.set(Dec<BFlit>{});
    top.mem_r.set(Dec<RFlit>{});
    top.cfg_aw_rdy.set(true);
    top.cfg_w_rdy.set(true);
    top.cfg_ar_rdy.set(true);
    top.cfg_b.set(Dec<BFlit>{});
    top.cfg_r.set(Dec<RFlit>{});
    // L2 侧默认：无 tx 流量、rx 全收
    top.chi_tx_req.set(Dec<CHIREQ>{});
    top.chi_tx_rsp.set(Dec<CHIRSP>{});
    top.chi_tx_dat.set(Dec<CHIDAT>{});
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

TEST_CASE("ZjL3 通路：ReadOnce 到 bank0 HNF 桩并收回 Comp/CompData") {
    ZjL3 top;
    top.elaborate();
    tieOff(top);

    Dec<CHIREQ> req;
    req.valid            = true;
    req.bits.opcode      = 0x03;          // ReadOnce
    req.bits.addr        = 0x80000000ULL; // addr[12]=0 → bank0 → gid0
    req.bits.tgt_id      = 0;
    req.bits.src_id      = 0x08;          // CC 节点自身
    req.bits.txn_id      = 0x5A;
    req.bits.size        = 6;
    req.bits.mem_attr_cacheable = true;
    req.bits.mem_attr_allocate  = true;
    req.bits.mem_attr_ewa       = true;
    top.chi_tx_req.set(req);

    // 等待 socket 接收（rdy 起来即 fire）
    comb(top);
    CHECK(top.chi_tx_req_rdy.get());
    bool consumed = runUntil(top, 20, [&] { return top.chi_tx_req_rdy.get(); });
    CHECK(consumed);
    top.chi_tx_req.set(Dec<CHIREQ>{});

    // 收回 RSP(Comp) 与 DAT(CompData)
    bool got_rsp = runUntil(top, 200, [&] { return top.chi_rx_rsp.get().valid; });
    CHECK(got_rsp);
    CHECK(top.chi_rx_rsp.get().bits.opcode == 0x04);  // Comp
    CHECK(top.chi_rx_rsp.get().bits.txn_id == 0x5A);
    CHECK(top.chi_rx_rsp.get().bits.tgt_id == 0);     // adapter 不回填
    comb(top);  // RSP 被 L2 消费（rdy=1 恒真）

    bool got_dat = runUntil(top, 200, [&] { return top.chi_rx_dat.get().valid; });
    CHECK(got_dat);
    CHECK(top.chi_rx_dat.get().bits.opcode == 0x04);  // CompData
    CHECK(top.chi_rx_dat.get().bits.txn_id == 0x5A);
    CHECK(top.chi_rx_dat.get().bits.tgt_id == 0x08);  // 回到 CC

    // eject REQ 观察口全程无 valid（本集成不该有 REQ 到 CC）
    comb(top);
    CHECK(!top.cc_tx_req.get().valid);
}

TEST_CASE("ZjL3 通路：device REQ 经 RnRouter 落 defaultHni，穿 HI 桥出 cfgAXI 并回数") {
    ZjL3 top;
    top.elaborate();
    tieOff(top);

    Dec<CHIREQ> req;
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
    top.chi_tx_req.set(Dec<CHIREQ>{});

    // HI 桥应发出 cfgAXI AR（defaultHni gid4 → 桥内 CM → AR）
    bool got_ar = runUntil(top, 200, [&] { return top.cfg_ar.get().valid; });
    CHECK(got_ar);
    CHECK(top.cfg_ar.get().bits.addr == 0x38000008ULL);
    CHECK(top.cfg_ar.get().bits.size == 2);
    const uint8_t arid = top.cfg_ar.get().bits.id;
    edge(top);  // AR fire（tieOff 中 cfg_ar_rdy 恒 true）

    // 给 R：CompData 经桥 → 环 → CC socket → adapter 回到 L2
    Dec<RFlit> r;
    r.valid        = true;
    r.bits.id      = arid;
    r.bits.last    = true;
    r.bits.data[0] = 0xCAFEBABE;
    top.cfg_r.set(r);
    comb(top);
    edge(top);
    top.cfg_r.set(Dec<RFlit>{});

    bool got_dat = runUntil(top, 200, [&] { return top.chi_rx_dat.get().valid; });
    CHECK(got_dat);
    CHECK(top.chi_rx_dat.get().bits.opcode == 0x04);  // CompData
    CHECK(top.chi_rx_dat.get().bits.txn_id == 0x7B);
    CHECK(top.chi_rx_dat.get().bits.tgt_id == 0x08);
    CHECK(top.chi_rx_dat.get().bits.data[0] == 0xCAFEBABE);
}

}  // namespace
