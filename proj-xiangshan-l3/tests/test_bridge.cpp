// P4a 桥单测（定向冒烟）：HI 桥读/写基本流、S 桥读/写基本流、同 tag 保序。
// 对齐 AxiBridge.scala / AxiLiteBridge.scala / BaseCtrlMachine.scala；
// 全面的随机对拍见 verify/cosim harness_bridge.cpp。

#include <cstdint>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <model/bridge/hinode_axilite_bridge.h>
#include <model/bridge/snode_axi_bridge.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace zj::bridge;
using namespace zj::chi;
using namespace zj::axi;
using namespace prefabtest;

namespace {

// ---------------- HI 桥 ----------------

void tieOffHi(HiNodeAxiLiteBridge& b) {
    b.node_id.set(0x20);
    b.rx_req.set(Valid<RReqFlit>{});
    b.rx_resp.set(Valid<RespFlit>{});
    b.rx_data.set(Valid<DataFlit>{});
    b.tx_resp_rdy.set(false);
    b.tx_data_rdy.set(false);
    b.axi_aw_rdy.set(false);
    b.axi_w_rdy.set(false);
    b.axi_ar_rdy.set(false);
    b.axi_b.set(Valid<BFlit>{});
    b.axi_r.set(Valid<RFlit>{});
}

RReqFlit mkReq(uint8_t opcode, uint64_t addr, uint8_t size, uint16_t txn, uint8_t order,
               bool eca = false) {
    RReqFlit r;
    r.opcode        = opcode;
    r.addr          = addr;
    r.size          = size;
    r.txn_id        = txn;
    r.src_id        = 0x08;
    r.order         = order;
    r.exp_comp_ack  = eca;
    r.mem_attr      = 0x2;  // device=1（MMIO）
    return r;
}

TEST_CASE("HiBridge 读：ReadNoSnp → ReadReceipt + AR → R → CompData") {
    HiNodeAxiLiteBridge b;
    b.elaborate();
    tieOffHi(b);
    b.tx_resp_rdy.set(true);
    b.tx_data_rdy.set(true);
    b.axi_ar_rdy.set(true);

    // 拍 0：请求入队（order=1 → 需要 ReadReceipt）
    Valid<RReqFlit> q;
    q.valid = true;
    q.bits  = mkReq(req_op::kReadNoSnp, 0x1000'0040, 3, 0x11, 1);
    b.rx_req.set(q);
    comb(b);
    CHECK(b.rx_req_rdy.get());
    CHECK(!b.axi_ar.get().valid);  // waiting 全 1，未发 AR
    edge(b);
    b.rx_req.set(Valid<RReqFlit>{});

    // ReadReceipt（经 CondVipArb 一拍注册延迟，轮询）
    bool sawReceipt = false;
    for (uint32_t t = 0; t < 6 && !sawReceipt; ++t) {
        comb(b);
        if (b.tx_resp.get().valid) {
            CHECK(b.tx_resp.get().bits.opcode == rsp_op::kReadReceipt);
            CHECK(b.tx_resp.get().bits.txn_id == 0x11);
            CHECK(b.tx_resp.get().bits.tgt_id == 0x08);
            sawReceipt = true;
        }
        edge(b);
    }
    CHECK(sawReceipt);

    // AR（waiting←0 后；同样经仲裁注册延迟）
    bool sawAr = false;
    for (uint32_t t = 0; t < 6 && !sawAr; ++t) {
        comb(b);
        if (b.axi_ar.get().valid) {
            CHECK(b.axi_ar.get().bits.id == 0);
            CHECK(b.axi_ar.get().bits.addr == 0x1000'0040);
            CHECK(b.axi_ar.get().bits.len == 0);
            CHECK(b.axi_ar.get().bits.size == 3);
            sawAr = true;
        }
        edge(b);
    }
    CHECK(sawAr);

    // R 到达 → CompData（DataID = addr(5)<<1 = 0）
    Valid<RFlit> r;
    r.valid      = true;
    r.bits.id    = 0;
    r.bits.last  = true;
    r.bits.resp  = 0;
    r.bits.data  = {0xDEADBEEF, 0, 0, 0};
    b.axi_r.set(r);
    comb(b);
    CHECK(b.axi_r_rdy.get());  // readDataPipe 空
    edge(b);
    b.axi_r.set(Valid<RFlit>{});

    comb(b);
    CHECK(b.tx_data.get().valid);
    CHECK(b.tx_data.get().bits.opcode == dat_op::kCompData);
    CHECK(b.tx_data.get().bits.data[0] == 0xDEADBEEF);
    CHECK(b.tx_data.get().bits.txn_id == 0x11);
    CHECK(b.tx_data.get().bits.tgt_id == 0x08);
    CHECK(b.tx_data.get().bits.home_nid == 0x20);
    CHECK(b.tx_data.get().bits.dbid == 0);
    edge(b);  // CompData fire；CM 完成

    comb(b);
    CHECK(!b.tx_data.get().valid);
    CHECK(b.rx_req_rdy.get());  // CM 已释放
}

TEST_CASE("HiBridge 写：WriteNoSnpPtl → DBIDResp → 数据 → AW/W → B → Comp") {
    HiNodeAxiLiteBridge b;
    b.elaborate();
    tieOffHi(b);
    b.tx_resp_rdy.set(true);
    b.axi_aw_rdy.set(true);
    b.axi_w_rdy.set(true);

    // 拍 0：写请求（ewa=0 → Comp 等 B；expCompAck=0 → 不等 CompAck）
    Valid<RReqFlit> q;
    q.valid = true;
    q.bits  = mkReq(req_op::kWriteNoSnpPtl, 0x1000'0048, 3, 0x22, 0);
    b.rx_req.set(q);
    comb(b);
    CHECK(b.rx_req_rdy.get());
    edge(b);
    b.rx_req.set(Valid<RReqFlit>{});

    // DBIDResp（DBID=CM idx=0）
    bool sawDbid = false;
    for (uint32_t t = 0; t < 6 && !sawDbid; ++t) {
        comb(b);
        if (b.tx_resp.get().valid) {
            CHECK(b.tx_resp.get().bits.opcode == rsp_op::kDBIDResp);
            CHECK(b.tx_resp.get().bits.dbid == 0);
            CHECK(b.tx_resp.get().bits.txn_id == 0x22);
            sawDbid = true;
        }
        edge(b);
    }
    CHECK(sawDbid);

    // 写数据（NCBWrData，TxnID=DBID=0，8B @ addr[4:3]=1 → data[1]）
    Valid<DataFlit> wd;
    wd.valid          = true;
    wd.bits.opcode    = dat_op::kNonCopyBackWriteData;
    wd.bits.txn_id    = 0;
    wd.bits.data_id   = 1;
    wd.bits.data[1]   = 0x1234567890ABCDEF;
    wd.bits.be        = 0x0000FF00;
    b.rx_data.set(wd);
    comb(b);
    CHECK(b.rx_data_rdy.get());  // 恒 true
    edge(b);
    b.rx_data.set(Valid<DataFlit>{});

    // AW（u.wdata 后；仲裁延迟）
    bool sawAw = false;
    for (uint32_t t = 0; t < 6 && !sawAw; ++t) {
        comb(b);
        if (b.axi_aw.get().valid) {
            CHECK(b.axi_aw.get().bits.addr == 0x1000'0048);
            CHECK(b.axi_aw.get().bits.size == 3);
            sawAw = true;
        }
        edge(b);
    }
    CHECK(sawAw);

    // W（awQueue deq 选中；strb=MaskGen(addr=0x48,size=3)）
    bool sawW = false;
    for (uint32_t t = 0; t < 6 && !sawW; ++t) {
        comb(b);
        if (b.axi_w.get().valid) {
            CHECK(b.axi_w.get().bits.last);
            CHECK(b.axi_w.get().bits.strb == 0x0000FF00);
            CHECK(b.axi_w.get().bits.data[1] == 0x1234567890ABCDEF);
            CHECK(b.axi_w.get().bits.data[0] == 0x1234567890ABCDEF);  // Fill 复制
            sawW = true;
        }
        edge(b);
    }
    CHECK(sawW);

    // B → Comp
    Valid<BFlit> br;
    br.valid   = true;
    br.bits.id = 0;
    b.axi_b.set(br);
    comb(b);
    CHECK(b.axi_b_rdy.get());
    edge(b);
    b.axi_b.set(Valid<BFlit>{});

    bool sawComp = false;
    for (uint32_t t = 0; t < 6 && !sawComp; ++t) {
        comb(b);
        if (b.tx_resp.get().valid) {
            CHECK(b.tx_resp.get().bits.opcode == rsp_op::kComp);
            CHECK(b.tx_resp.get().bits.txn_id == 0x22);
            sawComp = true;
        }
        edge(b);
    }
    CHECK(sawComp);
    comb(b);
    CHECK(b.rx_req_rdy.get());
}

// ---------------- S 桥 ----------------

void tieOffS(SNodeAxiBridge& b) {
    b.rx_req.set(Valid<HReqFlit>{});
    b.rx_data.set(Valid<DataFlit>{});
    b.tx_resp_rdy.set(false);
    b.tx_data_rdy.set(false);
    b.axi_aw_rdy.set(false);
    b.axi_w_rdy.set(false);
    b.axi_ar_rdy.set(false);
    b.axi_b.set(Valid<BFlit>{});
    b.axi_r.set(Valid<RFlit>{});
}

HReqFlit mkEReq(uint8_t opcode, uint64_t addr, uint8_t size, uint16_t txn, uint8_t order,
                uint8_t memAttr = 0xE) {
    HReqFlit r;
    r.opcode        = opcode;
    r.addr          = addr;
    r.size          = size;
    r.txn_id        = txn;
    r.src_id        = 0x10;
    r.return_nid    = 0x08;
    r.return_txn_id = 0x55;
    r.order         = order;
    r.mem_attr      = memAttr;  // allocate+cacheable+device 视场景
    return r;
}

TEST_CASE("SBridge 读 64B：AR(len=1) → R×2 → CompData×2（DataID 0/2）") {
    SNodeAxiBridge b;
    b.elaborate();
    tieOffS(b);
    b.tx_resp_rdy.set(true);
    b.tx_data_rdy.set(true);
    b.axi_ar_rdy.set(true);

    // order=0 → 无 ReadReceipt；mem_attr=0xE（非 device）→ cache=0b010|ewa
    Valid<HReqFlit> q;
    q.valid = true;
    q.bits  = mkEReq(req_op::kReadNoSnp, 0x2000'0000, 6, 0x33, 0, 0xE);
    b.rx_req.set(q);
    comb(b);
    CHECK(b.rx_req_rdy.get());
    edge(b);
    b.rx_req.set(Valid<HReqFlit>{});

    // AR（len=1, size=5）——经 CondVipArb 的 selReg 有一拍注册延迟，轮询等待
    bool sawAr = false;
    for (uint32_t t = 0; t < 6 && !sawAr; ++t) {
        comb(b);
        CHECK(!b.tx_resp.get().valid);  // order=0
        if (b.axi_ar.get().valid) {
            CHECK(b.axi_ar.get().bits.len == 1);
            CHECK(b.axi_ar.get().bits.size == 5);
            CHECK(b.axi_ar.get().bits.burst == 1);
            sawAr = true;
        }
        edge(b);
    }
    CHECK(sawAr);

    // 拍 2/3：R 两拍 → CompData×2（readCnt 驱动 DataID）
    for (uint32_t k = 0; k < 2; ++k) {
        Valid<RFlit> r;
        r.valid          = true;
        r.bits.id        = 0;
        r.bits.last      = k == 1;
        r.bits.data[0]   = 0xA0 + k;
        b.axi_r.set(r);
        comb(b);
        CHECK(b.axi_r_rdy.get());
        edge(b);
        b.axi_r.set(Valid<RFlit>{});
        comb(b);
        CHECK(b.tx_data.get().valid);
        CHECK(b.tx_data.get().bits.opcode == dat_op::kCompData);
        CHECK(b.tx_data.get().bits.data_id == 2 * k);
        CHECK(b.tx_data.get().bits.data[0] == 0xA0 + k);
        CHECK(b.tx_data.get().bits.txn_id == 0x55);   // returnTxnId
        CHECK(b.tx_data.get().bits.tgt_id == 0x08);   // returnNid
        CHECK(b.tx_data.get().bits.dbid == 0x33);     // 桥内 txnId
        CHECK(b.tx_data.get().bits.home_nid == 0x10); // srcId
        CHECK(b.tx_data.get().bits.be == 0xFFFFFFFF);
        edge(b);  // CompData fire
    }
    comb(b);
    CHECK(b.rx_req_rdy.get());  // CM 释放
}

TEST_CASE("SBridge 写 64B：alloc→DBIDResp→数据×2→AW→W×2→B→Comp") {
    SNodeAxiBridge b;
    b.elaborate();
    tieOffS(b);
    b.tx_resp_rdy.set(true);
    b.axi_aw_rdy.set(true);
    b.axi_w_rdy.set(true);

    // 拍 0：WriteNoSnpFull 64B（ewa=0 → Comp 等 B）
    Valid<HReqFlit> q;
    q.valid = true;
    q.bits  = mkEReq(req_op::kWriteNoSnpFull, 0x2000'0000, 6, 0x44, 0, 0xC);
    b.rx_req.set(q);
    comb(b);
    CHECK(b.rx_req_rdy.get());
    edge(b);
    b.rx_req.set(Valid<HReqFlit>{});

    // alloc →（allocSel+Queue(2)+freelist）→ DBIDResp；逐拍等到其出现
    bool gotDbid = false;
    for (uint32_t t = 0; t < 8 && !gotDbid; ++t) {
        comb(b);
        if (b.tx_resp.get().valid) {
            CHECK(b.tx_resp.get().bits.opcode == rsp_op::kDBIDResp);
            CHECK(b.tx_resp.get().bits.dbid == 0);
            CHECK(b.tx_resp.get().bits.txn_id == 0x44);
            gotDbid = true;
        }
        edge(b);
    }
    CHECK(gotDbid);

    // 写数据两拍（DataID=0/2 → buf 槽 0/1；TxnID=DBID=0）
    for (uint32_t k = 0; k < 2; ++k) {
        Valid<DataFlit> wd;
        wd.valid        = true;
        wd.bits.opcode  = dat_op::kNonCopyBackWriteData;
        wd.bits.txn_id  = 0;
        wd.bits.data_id = uint8_t(2 * k);
        wd.bits.data    = {0xB0 + k, 0, 0, 0};
        wd.bits.be      = 0xFFFFFFFF;
        b.rx_data.set(wd);
        comb(b);
        edge(b);
        b.rx_data.set(Valid<DataFlit>{});
    }

    // AW（u.wdata 到齐后；len=1）
    bool sawAw = false;
    for (uint32_t t = 0; t < 8 && !sawAw; ++t) {
        comb(b);
        if (b.axi_aw.get().valid) {
            CHECK(b.axi_aw.get().bits.len == 1);
            CHECK(b.axi_aw.get().bits.size == 5);
            sawAw = true;
        }
        edge(b);
    }
    CHECK(sawAw);

    // W 两拍（数据来自 dataBuffer RAM；第二拍 last）
    uint32_t wBeats = 0;
    for (uint32_t t = 0; t < 16 && wBeats < 2; ++t) {
        comb(b);
        if (b.axi_w.get().valid) {
            CHECK(b.axi_w.get().bits.strb == 0xFFFFFFFF);
            CHECK(b.axi_w.get().bits.last == (wBeats == 1));
            ++wBeats;
        }
        edge(b);
    }
    CHECK(wBeats == 2);

    // B → Comp
    Valid<BFlit> br;
    br.valid   = true;
    br.bits.id = 0;
    b.axi_b.set(br);
    comb(b);
    edge(b);
    b.axi_b.set(Valid<BFlit>{});

    bool sawComp = false;
    for (uint32_t t = 0; t < 8 && !sawComp; ++t) {
        comb(b);
        if (b.tx_resp.get().valid && b.tx_resp.get().bits.opcode == rsp_op::kComp) {
            CHECK(b.tx_resp.get().bits.txn_id == 0x44);
            sawComp = true;
        }
        edge(b);
    }
    CHECK(sawComp);
    comb(b);
    CHECK(b.rx_req_rdy.get());
}

TEST_CASE("SBridge 同 tag 保序：写未完成时同 32KB 读被阻塞") {
    SNodeAxiBridge b;
    b.elaborate();
    tieOffS(b);
    b.tx_resp_rdy.set(true);
    b.axi_aw_rdy.set(true);
    b.axi_w_rdy.set(true);
    b.axi_ar_rdy.set(true);

    // 写 @0x2000'0000 先行（挂起：不给 B）
    Valid<HReqFlit> qw;
    qw.valid = true;
    qw.bits  = mkEReq(req_op::kWriteNoSnpFull, 0x2000'0000, 6, 0x50, 0, 0xC);
    b.rx_req.set(qw);
    comb(b);
    edge(b);
    b.rx_req.set(Valid<HReqFlit>{});

    // 等 DBIDResp（协议时序：数据必须在 DBIDResp 之后给）
    for (uint32_t t = 0; t < 10; ++t) {
        comb(b);
        if (b.tx_resp.get().valid && b.tx_resp.get().bits.opcode == rsp_op::kDBIDResp) break;
        edge(b);
    }
    edge(b);  // DBIDResp fire

    // 写数据两拍
    for (uint32_t k = 0; k < 2; ++k) {
        Valid<DataFlit> wd;
        wd.valid        = true;
        wd.bits.opcode  = dat_op::kNonCopyBackWriteData;
        wd.bits.txn_id  = 0;
        wd.bits.data_id = uint8_t(2 * k);
        wd.bits.be      = 0xFFFFFFFF;
        b.rx_data.set(wd);
        comb(b);
        edge(b);
        b.rx_data.set(Valid<DataFlit>{});
    }

    // 同 tag 读入队（addr[37:6] 相同 = 同 64B 块：写 @0x2000'0000、读 @0x2000'0008）
    Valid<HReqFlit> qr;
    qr.valid = true;
    qr.bits  = mkEReq(req_op::kReadNoSnp, 0x2000'0008, 2, 0x51, 0, 0xE);
    b.rx_req.set(qr);
    comb(b);
    CHECK(b.rx_req_rdy.get());  // CM 有空闲，入队不受阻
    edge(b);
    b.rx_req.set(Valid<HReqFlit>{});

    // 推进若干拍：写的 AW/W 走完（不给 B）；读的 AR 必须一直压着
    for (uint32_t t = 0; t < 20; ++t) {
        comb(b);
        CHECK(!b.axi_ar.get().valid);  // 读 AR 被 waiting 阻塞
        edge(b);
    }
    // 给 B：写完成 → wakeup → 读的 waiting 递减到 0 → AR 发出
    Valid<BFlit> br;
    br.valid   = true;
    br.bits.id = 0;
    b.axi_b.set(br);
    comb(b);
    edge(b);
    b.axi_b.set(Valid<BFlit>{});

    bool sawAr = false;
    for (uint32_t t = 0; t < 10 && !sawAr; ++t) {
        comb(b);
        if (b.axi_ar.get().valid) sawAr = true;
        edge(b);
    }
    CHECK(sawAr);
}

}  // namespace
