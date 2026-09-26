// P2 PDC 单测：PdcTx token 计数/打拍；PdcRx flow 直通/grant 打拍；
// Tx→Rx 链路 2 拍延迟、token 耗尽反压与回补。
// 对齐 zhujiang/device/socket/PowerDomaincCrossing.scala:16-62；精确逐拍
// 时序的权威验收在 verify/ 的 socket RTL 对拍。

#include <cstdint>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <model/cc/pdc.h>
#include <model/flit/zj_flit.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace zj::sock;
using namespace zj::chi;
using namespace prefabtest;

namespace {

RReqFlit mkFlit(uint16_t txn) {
    RReqFlit f;
    f.txn_id = txn;
    f.src_id = 0x09;
    f.tgt_id = 0x10;
    return f;
}

TEST_CASE("PdcTx：enq 打拍到 pdc；token 计数随 fire/grant 增减") {
    PdcTx<RReqFlit> tx;
    tx.elaborate();
    tx.pdc_grant.set(false);

    Valid<RReqFlit> in;
    in.valid = true;
    in.bits  = mkFlit(0x11);
    tx.enq.set(in);
    comb(tx);
    CHECK(tx.enq_rdy.get());       // tokens=5
    CHECK(!tx.pdc.get().valid);    // 当拍不出（打 1 拍）
    edge(tx);
    tx.enq.set(Valid<RReqFlit>{});
    comb(tx);
    CHECK(tx.pdc.get().valid);     // 1 拍后可见
    CHECK(tx.pdc.get().bits.txn_id == 0x11);
    CHECK(tx.enq_rdy.get());       // tokens=4，仍可收
    edge(tx);
    comb(tx);
    CHECK(!tx.pdc.get().valid);    // txv 已撤

    // 连收 4 个（无 grant）→ tokens 耗尽
    for (int i = 0; i < 4; ++i) {
        in.bits = mkFlit(uint16_t(0x20 + i));
        tx.enq.set(in);
        cycle(tx);
    }
    comb(tx);
    CHECK(!tx.enq_rdy.get());      // tokens=0
    CHECK(tx.pdc.get().valid);     // 最后一个仍在 pdc

    // grant 来一拍 → rxg 打拍 → 下一拍 token+1 → 又能收
    tx.enq.set(Valid<RReqFlit>{});
    tx.pdc_grant.set(true);
    cycle(tx);                 // rxg<=1
    tx.pdc_grant.set(false);
    cycle(tx);                 // rxg=1 期间 tokens+1 提交
    comb(tx);
    CHECK(tx.enq_rdy.get());
}

TEST_CASE("PdcRx：pdc 打拍 + flow 直通 1 拍到 deq；grant 为 deq.fire 打拍") {
    PdcRx<RReqFlit> rx;
    rx.elaborate();
    rx.deq_rdy.set(true);

    Valid<RReqFlit> p;
    p.valid = true;
    p.bits  = mkFlit(0x33);
    rx.pdc.set(p);
    comb(rx);
    CHECK(!rx.deq.get().valid);    // 当拍不出（rxv 打拍）
    CHECK(!rx.pdc_grant.get());
    edge(rx);
    rx.pdc.set(Valid<RReqFlit>{});
    comb(rx);
    CHECK(rx.deq.get().valid);     // rxv=1 + flow 直通
    CHECK(rx.deq.get().bits.txn_id == 0x33);
    CHECK(!rx.pdc_grant.get());    // grant 打 1 拍
    edge(rx);
    comb(rx);
    CHECK(!rx.deq.get().valid);
    CHECK(rx.pdc_grant.get());     // 上拍 deq.fire
    edge(rx);
    comb(rx);
    CHECK(!rx.pdc_grant.get());

    // 反压：deq_rdy=0 时 flow 直通 valid 仍起（chisel flow：deq.valid 不被
    // deq.ready 门控），但不 fire；flit 落入队列保持，不丢
    rx.deq_rdy.set(false);
    p.bits = mkFlit(0x44);
    rx.pdc.set(p);
    cycle(rx);
    rx.pdc.set(Valid<RReqFlit>{});
    comb(rx);
    CHECK(rx.deq.get().valid);     // flow 直通（不 fire）
    CHECK(rx.deq.get().bits.txn_id == 0x44);
    edge(rx);
    comb(rx);
    CHECK(rx.deq.get().valid);     // 队列中保持
    CHECK(rx.deq.get().bits.txn_id == 0x44);
    rx.deq_rdy.set(true);
    cycle(rx);
    comb(rx);
    CHECK(!rx.deq.get().valid);
}

TEST_CASE("PdcTx→PdcRx 链路：enq 到 deq 固定 2 拍；grant 回路还 token") {
    PdcTx<RReqFlit> tx;
    PdcRx<RReqFlit> rx;
    tx.elaborate();
    rx.elaborate();
    rx.deq_rdy.set(true);

    auto link = [&] {
        // 手工连线（两模块各自 eval，按拍推进）
        rx.pdc.set(tx.pdc.get());
        tx.pdc_grant.set(rx.pdc_grant.get());
    };

    Valid<RReqFlit> in;
    in.valid = true;
    in.bits  = mkFlit(0x77);
    tx.enq.set(in);
    comb(tx);
    CHECK(tx.enq_rdy.get());
    edge(tx);                      // T0 fire（恰好 1 个）
    tx.enq.set(Valid<RReqFlit>{});
    link(); comb(tx); comb(rx);    // T1：pdc.valid=1，rx 尚无输出
    CHECK(!rx.deq.get().valid);
    edge(tx); edge(rx);            // rxv<=1
    link(); comb(tx); comb(rx);    // T2：rxv=1 + flow → deq.valid=1
    CHECK(rx.deq.get().valid);
    CHECK(rx.deq.get().bits.txn_id == 0x77);
    edge(tx); edge(rx);            // deq.fire@T2 → txg@T3
    link(); comb(tx); comb(rx);    // T3
    CHECK(!rx.deq.get().valid);
    CHECK(rx.pdc_grant.get());     // grant@T3
    edge(tx); edge(rx);            // rxg<=1@T4
    link(); comb(tx); comb(rx);    // T4
    CHECK(!rx.pdc_grant.get());
    edge(tx); edge(rx);            // T4 期间 rxg=1 → tokens+1 提交，T5 恢复 5
    link();

    // token 已回补：deq_rdy=0 断 grant，恰好能连收 5 个
    rx.deq_rdy.set(false);
    for (int i = 0; i < 5; ++i) {
        in.bits = mkFlit(uint16_t(0x80 + i));
        tx.enq.set(in);
        comb(tx);
        CHECK(tx.enq_rdy.get());   // 第 i+1 个可收（共 5 个 token）
        edge(tx);
        link(); comb(rx); edge(rx); link();
    }
    comb(tx);
    CHECK(!tx.enq_rdy.get());      // 恰好耗尽
    tx.enq.set(Valid<RReqFlit>{});
}

}  // namespace
