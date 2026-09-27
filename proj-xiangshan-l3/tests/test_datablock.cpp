// DataBlock 冒烟单测：DBIDPool 预充/双queue分配、BeatStorage 5 拍读延迟、
// DataBlock fetch（alloc→task(read+send)→dsResp 直发 txDat→resp→clean）。
// 全量随机对拍见 verify/cosim/harness_db.cpp。

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

#include "model/dj/data.h"
#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace zj::dj;
using namespace prefabtest;

namespace {

TEST_CASE("dj DBIDPool: 预充 64 拍后双queue出 id，单取从长queue") {
    DBIDPool d;
    d.elaborate();
    d.clk_en.set(true);
    d.enq0.set({false, 0});
    d.enq1.set({false, 0});
    d.deq0_rdy.set(false);
    d.deq1_rdy.set(false);
    uint64_t firstValid = 0;
    for (uint64_t c = 0; c < 80; ++c) {
        comb(d);
        if (d.deq0.get().valid && firstValid == 0) firstValid = c + 1;
        edge(d);
    }
    CHECK(firstValid == 65);  // 64 拍预充 + rst_done 锁存 1 拍
    // 双取：各弹一个（q0 头=0、q1 头=64）
    comb(d);
    CHECK(d.deq0.get().valid);
    CHECK(d.deq1.get().valid);
    CHECK(d.deq0.get().bits == 0);
    CHECK(d.deq1.get().bits == 64);
    d.deq0_rdy.set(true);
    d.deq1_rdy.set(true);
    edge(d);
    d.deq0_rdy.set(false);
    d.deq1_rdy.set(false);
    comb(d);
    CHECK(d.deq0.get().bits == 1);
    CHECK(d.deq1.get().bits == 65);
    edge(d);
    // 单取 deq0：deqSelQ0 = q0.count(63) >= q1.count(63) → 取 q0 头 1
    d.deq0_rdy.set(true);
    comb(d);
    CHECK(d.deq0.get().bits == 1);
    edge(d);
    comb(d);
    // 再单取 deq0：q0.count(62) < q1.count(63) → 从长 queue 取 q1 头 65
    CHECK(d.deq0.get().bits == 65);
    edge(d);
    d.deq0_rdy.set(false);
    comb(d);
}

TEST_CASE("dj BeatStorage: 写后读，响应恰好 5 拍") {
    BeatStorage d;
    d.elaborate();
    d.clk_en.set(true);
    d.read.set({false, {}});
    d.write.set({false, {}});
    cycle(d);  // rst_done 第 1 拍锁存
    WriteDS w;
    w.ds.idx = 7;
    w.beat = {0xAA, 0xBB, 0xCC, 0xDD};
    d.write.set({true, w});
    comb(d);
    CHECK(d.write_rdy.get() == true);
    edge(d);
    d.write.set({false, {}});
    cycle(d);  // 写优先间隔
    ReadDS r;
    r.ds.idx = 7;
    r.dcid = 3;
    r.dbid = 11;
    r.beatNum = 0;
    r.toCHI = true;
    d.read.set({true, r});
    comb(d);
    CHECK(d.read_rdy.get() == true);
    edge(d);  // d0
    d.read.set({false, {}});
    for (int i = 0; i < 4; ++i) {
        comb(d);
        CHECK(d.resp.get().valid == false);
        edge(d);
    }
    comb(d);  // d0+5
    CHECK(d.resp.get().valid == true);
    CHECK(d.resp.get().bits.dcid == 3);
    CHECK(d.resp.get().bits.dbid == 11);
    CHECK(d.resp.get().bits.toCHI == true);
    CHECK(d.resp.get().bits.beat[0] == 0xAA);
    CHECK(d.resp.get().bits.beat[3] == 0xDD);
    edge(d);
    comb(d);
    CHECK(d.resp.get().valid == false);
    edge(d);
}

// DataBlock fetch：alloc → task(read+send) → DS 零数据经 dsResp 直发 txDat → resp
TEST_CASE("dj DataBlock: fetch 全流程（DS→CHI 直发）") {
    DataBlock d;
    d.elaborate();
    d.clk_en.set(true);
    d.tx_dat_rdy.set(true);
    d.rx_dat.set({false, {}});
    d.upd_hn_txn_id.set({false, {}});
    d.req_db.set({false, {}});
    d.task.set({false, {}});
    d.clean_db.set({false, {}});
    for (int i = 0; i < 70; ++i) cycle(d);  // 过 DBID 预充

    // alloc：txn=0x12，两 beat
    ReqDB rq{0x12, 0x3};
    d.req_db.set({true, rq});
    comb(d);
    CHECK(d.req_db_rdy.get() == true);
    edge(d);
    d.req_db.set({false, {}});
    cycle(d);

    // task：read+send（fetch），ds 指向某 (addr,way)
    DataTask t;
    t.hnTxnID = 0x12;
    t.dataOp.read = true;
    t.dataOp.send = true;
    t.dataVec = 0x3;
    t.qos = 0;
    DsIdx ds;
    ds.set(catAddr(0, 0x123, 5, 13, 0), 3);  // llc setBits=13
    t.ds = ds;
    t.txDat.txn_id = 0x12;
    t.txDat.qos = 0;
    t.txDat.opcode = dat_op::kCompData;
    d.task.set({true, t});
    cycle(d);
    d.task.set({false, {}});

    // 等 txDat（两 beat 直发）与 resp
    int datBeats = 0;
    uint64_t datCyc = 0, respCyc = 0;
    for (uint64_t c = 0; c < 200 && (datBeats < 2 || respCyc == 0); ++c) {
        comb(d);
        if (d.tx_dat.get().valid) {
            ++datBeats;
            if (datCyc == 0) datCyc = c;
            CHECK(d.tx_dat.get().bits.txn_id == 0x12);
            CHECK(d.tx_dat.get().bits.be == 0xFFFFFFFFull);
            CHECK(d.tx_dat.get().bits.data_id == (datBeats == 1 ? 0 : 2));
            CHECK(d.tx_dat.get().bits.data[0] == 0);  // DS 零初值
        }
        if (d.resp.get().valid) {
            respCyc = c;
            CHECK(d.resp.get().bits == 0x12);
        }
        edge(d);
    }
    CHECK(datBeats == 2);
    CHECK(respCyc != 0);

    // clean 释放（ALLOC 态）
    d.clean_db.set({true, {0x12, 0x3}});
    cycle(d);
    d.clean_db.set({false, {}});
    for (int i = 0; i < 5; ++i) cycle(d);
    // 后续 alloc 不应被耗尽反压（entry/dbid 已释放）：连续 64 组 alloc+clean
    for (int k = 0; k < 64; ++k) {
        ReqDB q{static_cast<uint8_t>((k + 1) & 0x7F), 0x1};
        d.req_db.set({true, q});
        comb(d);
        CHECK(d.req_db_rdy.get() == true);
        edge(d);
        d.req_db.set({false, {}});
        d.clean_db.set({true, {q.hnTxnID, 0x1}});
        cycle(d);
        d.clean_db.set({false, {}});
        cycle(d);
    }
}

// save → fetch 数据回路：写数据经 rxDat 入 buf → save 写 DS → fetch 读回
TEST_CASE("dj DataBlock: save 后 fetch 数据回路") {
    DataBlock d;
    d.elaborate();
    d.clk_en.set(true);
    d.tx_dat_rdy.set(true);
    d.rx_dat.set({false, {}});
    d.upd_hn_txn_id.set({false, {}});
    d.req_db.set({false, {}});
    d.task.set({false, {}});
    d.clean_db.set({false, {}});
    for (int i = 0; i < 70; ++i) cycle(d);

    DsIdx ds;
    ds.set(catAddr(0, 0x321, 6, 13, 0), 5);

    // alloc A（save，两 beat）
    d.req_db.set({true, {0x20, 0x3}});
    cycle(d);
    d.req_db.set({false, {}});
    cycle(d);
    DataTask t;
    t.hnTxnID = 0x20;
    t.dataOp.save = true;
    t.dataVec = 0x3;
    t.ds = ds;
    t.txDat.txn_id = 0x20;
    d.task.set({true, t});
    cycle(d);
    d.task.set({false, {}});
    // rxDat 两 beat（NCBWr，BE 全 1）
    DataFlit f0 = {}, f1 = {};
    f0.txn_id = 0x20;
    f0.data_id = 0;
    f0.opcode = dat_op::kNonCopyBackWriteData;
    f0.be = 0xFFFFFFFFull;
    f0.data = {0x1111, 0x2222, 0x3333, 0x4444};
    f1.txn_id = 0x20;
    f1.data_id = 2;
    f1.opcode = dat_op::kNonCopyBackWriteData;
    f1.be = 0xFFFFFFFFull;
    f1.data = {0x5555, 0x6666, 0x7777, 0x8888};
    d.rx_dat.set({true, f0});
    cycle(d);
    d.rx_dat.set({true, f1});
    cycle(d);
    d.rx_dat.set({false, {}});
    // 等 resp 后 clean
    for (uint64_t c = 0; c < 100; ++c) {
        comb(d);
        if (d.resp.get().valid) break;
        edge(d);
    }
    comb(d);
    CHECK(d.resp.get().valid);
    edge(d);
    d.clean_db.set({true, {0x20, 0x3}});
    cycle(d);
    d.clean_db.set({false, {}});
    for (int i = 0; i < 3; ++i) cycle(d);

    // alloc B（fetch 同槽）
    d.req_db.set({true, {0x21, 0x3}});
    cycle(d);
    d.req_db.set({false, {}});
    cycle(d);
    DataTask t2;
    t2.hnTxnID = 0x21;
    t2.dataOp.read = true;
    t2.dataOp.send = true;
    t2.dataVec = 0x3;
    t2.ds = ds;
    t2.txDat.txn_id = 0x21;
    d.task.set({true, t2});
    cycle(d);
    d.task.set({false, {}});
    int datBeats = 0;
    for (uint64_t c = 0; c < 200 && datBeats < 2; ++c) {
        comb(d);
        if (d.tx_dat.get().valid) {
            if (datBeats == 0) CHECK(d.tx_dat.get().bits.data[0] == 0x1111);
            if (datBeats == 0) CHECK(d.tx_dat.get().bits.data[3] == 0x4444);
            if (datBeats == 1) CHECK(d.tx_dat.get().bits.data[0] == 0x5555);
            if (datBeats == 1) CHECK(d.tx_dat.get().bits.data[3] == 0x8888);
            ++datBeats;
        }
        edge(d);
    }
    CHECK(datBeats == 2);
    d.clean_db.set({true, {0x21, 0x3}});
    cycle(d);
    d.clean_db.set({false, {}});
}

}  // namespace
