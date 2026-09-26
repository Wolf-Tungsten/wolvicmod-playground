// P2 XscChiAdapter 单测：六通道字段级重映射（期望值逐字段手算自
// ZhuJiangBridge.scala:152-252）+ ready/valid 纯组合直通。
// 权威等价性证据：coremark 前端 trace 重放（CHI 侧驱动、环侧比对，adapter
// 在通路上）；本文件为定向字段用例。

#include <cstdint>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <model/cc/xsc_chi_adapter.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace zj::xs;
using namespace zj::chi;
using namespace prefabtest;

namespace {

TEST_CASE("mapReq：CHIREQ→ReqFlit 逐字段") {
    CHIREQ s{};
    s.qos = 0xA; s.tgt_id = 0x155; s.src_id = 0x0AA; s.txn_id = 0xABC;
    s.opcode = 0x55; s.size = 0x6; s.addr = 0x123456789ABCULL; s.order = 0x2;
    s.mem_attr_allocate = true; s.mem_attr_cacheable = true;
    s.mem_attr_device = false; s.mem_attr_ewa = true;
    s.snp_attr = true; s.snoop_me = true; s.exp_comp_ack = true;
    s.mpam_part_id = 0x1FF; s.rsvdc = 0xF;  // 被读但落零宽字段 → 丢弃

    RReqFlit d = mapReq(s);
    CHECK(d.qos == 0xA);
    CHECK(d.tgt_id == 0x155);
    CHECK(d.src_id == 0x0AA);
    CHECK(d.txn_id == 0xABC);
    CHECK(d.opcode == 0x55);
    CHECK(d.size == 0x6);
    CHECK(d.addr == 0x123456789ABCULL);
    CHECK(d.order == 0x2);
    CHECK(d.mem_attr == 0b1101);  // {allocate, cacheable, device, ewa}
    CHECK(d.snp_attr);
    CHECK(d.excl);
    CHECK(d.exp_comp_ack);
    // 零宽字段无落点：struct 本身无对应成员（编译期保证）
}

TEST_CASE("mapRsp：CHIRSP→RespFlit；RespFlit→CHIRSP（tgt 恒 0）") {
    CHIRSP s{};
    s.qos = 0x5; s.tgt_id = 0x199; s.src_id = 0x066; s.txn_id = 0x777;
    s.opcode = 0x15; s.resp_err = 0x2; s.resp = 0x5; s.fwd_state = 0x3;
    s.c_busy = 0x6; s.dbid = 0x9AB;

    RespFlit z = mapRspZj(s);
    CHECK(z.qos == 0x5);
    CHECK(z.tgt_id == 0x199);
    CHECK(z.src_id == 0x066);
    CHECK(z.txn_id == 0x777);
    CHECK(z.opcode == 0x15);
    CHECK(z.resp_err == 0x2);
    CHECK(z.resp == 0x5);
    CHECK(z.fwd_state == 0x3);
    CHECK(z.c_busy == 0x6);
    CHECK(z.dbid == 0x9AB);

    RespFlit zr{};
    zr.qos = 0x3; zr.tgt_id = 0x1EE; zr.src_id = 0x011; zr.txn_id = 0x234;
    zr.opcode = 0x04; zr.resp_err = 0x1; zr.resp = 0x2; zr.fwd_state = 0x7;
    zr.c_busy = 0x1; zr.dbid = 0xFED;
    CHIRSP x = mapRspXs(zr);
    CHECK(x.qos == 0x3);
    CHECK(x.tgt_id == 0);  // 不回填（ZhuJiangBridge.scala:187-199）
    CHECK(x.src_id == 0x011);
    CHECK(x.txn_id == 0x234);
    CHECK(x.opcode == 0x04);
    CHECK(x.resp_err == 0x1);
    CHECK(x.resp == 0x2);
    CHECK(x.fwd_state == 0x7);
    CHECK(x.c_busy == 0x1);
    CHECK(x.dbid == 0xFED);
}

TEST_CASE("mapDat：宽度适配（DBID 12↔16、DataSource 4↔8）") {
    CHIDAT s{};
    s.qos = 0x1; s.tgt_id = 0x123; s.src_id = 0x045; s.txn_id = 0x678;
    s.home_nid = 0x0AB; s.opcode = 0xB; s.resp_err = 0x3; s.resp = 0x6;
    s.data_source = 0xD; s.c_busy = 0x2; s.dbid = 0xFFF; s.data_id = 0x2;
    s.be = 0xDEADBEEFULL;
    for (int i = 0; i < 4; ++i) s.data[i] = 0x1111ULL * (i + 1);

    DataFlit z = mapDatZj(s);
    CHECK(z.qos == 0x1);
    CHECK(z.tgt_id == 0x123);
    CHECK(z.src_id == 0x045);
    CHECK(z.txn_id == 0x678);
    CHECK(z.home_nid == 0x0AB);
    CHECK(z.opcode == 0xB);
    CHECK(z.resp_err == 0x3);
    CHECK(z.resp == 0x6);
    CHECK(z.data_source == 0xD);   // 零扩展到 8b
    CHECK(z.c_busy == 0x2);
    CHECK(z.dbid == 0x0FFF);       // 12→16 零扩展
    CHECK(z.data_id == 0x2);
    CHECK(z.be == 0xDEADBEEFULL);
    CHECK(z.data[3] == 0x4444ULL);

    DataFlit zr{};
    zr.qos = 0x2; zr.tgt_id = 0x077; zr.src_id = 0x088; zr.txn_id = 0x999;
    zr.home_nid = 0x033; zr.opcode = 0x4; zr.resp_err = 0x1; zr.resp = 0x5;
    zr.data_source = 0xAB; zr.c_busy = 0x5; zr.dbid = 0xF123; zr.data_id = 0x1;
    zr.be = 0x12345678ULL;
    for (int i = 0; i < 4; ++i) zr.data[i] = 0x2222ULL * (i + 1);
    CHIDAT x = mapDatXs(zr);
    CHECK(x.qos == 0x2);
    CHECK(x.tgt_id == 0x077);
    CHECK(x.src_id == 0x088);
    CHECK(x.txn_id == 0x999);
    CHECK(x.home_nid == 0x033);
    CHECK(x.opcode == 0x4);
    CHECK(x.resp_err == 0x1);
    CHECK(x.resp == 0x5);
    CHECK(x.data_source == 0xB);   // 8→4 截断
    CHECK(x.c_busy == 0x5);
    CHECK(x.dbid == 0x123);        // 16→12 截断
    CHECK(x.data_id == 0x1);
    CHECK(x.be == 0x12345678ULL);
    CHECK(x.data[2] == 0x6666ULL);
}

TEST_CASE("mapSnp：SnoopFlit→CHISNP") {
    SnoopFlit s{};
    s.qos = 0x7; s.src_id = 0x0CC; s.txn_id = 0x456; s.fwd_nid = 0x1BB;
    s.fwd_txn_id = 0x789; s.opcode = 0x11; s.addr = 0x1FFFFFFFFFFFULL & ((1ULL << 45) - 1);
    s.do_not_go_to_sd = true; s.ret_to_src = true;

    CHISNP x = mapSnp(s);
    CHECK(x.qos == 0x7);
    CHECK(x.src_id == 0x0CC);
    CHECK(x.txn_id == 0x456);
    CHECK(x.fwd_nid == 0x1BB);
    CHECK(x.fwd_txn_id == 0x789);
    CHECK(x.opcode == 0x11);
    CHECK(x.addr == ((1ULL << 45) - 1));
    CHECK(x.do_not_go_to_sd);
    CHECK(x.ret_to_src);
}

TEST_CASE("XscChiAdapter 模块：六通道组合直通（valid/bits/ready）") {
    XscChiAdapter ad;
    ad.elaborate();

    // L2→ZJ 三通道：valid/bits 映射 + ready 直通
    Valid<CHIREQ> q;
    q.valid      = true;
    q.bits.qos   = 0x9;
    q.bits.txn_id = 0x321;
    q.bits.snoop_me = true;
    ad.chi_tx_req.set(q);
    ad.zj_rx_req_rdy.set(true);
    ad.eval();
    CHECK(ad.zj_rx_req.get().valid);
    CHECK(ad.zj_rx_req.get().bits.qos == 0x9);
    CHECK(ad.zj_rx_req.get().bits.txn_id == 0x321);
    CHECK(ad.zj_rx_req.get().bits.excl);
    CHECK(ad.chi_tx_req_rdy.get());  // ready 直通

    ad.zj_rx_req_rdy.set(false);
    ad.eval();
    CHECK(!ad.chi_tx_req_rdy.get());

    // ZJ→L2 三通道
    Valid<SnoopFlit> snp;
    snp.valid       = true;
    snp.bits.opcode = 0x15;
    snp.bits.src_id = 0x0DD;
    ad.zj_tx_snp.set(snp);
    ad.chi_rx_snp_rdy.set(true);
    ad.eval();
    CHECK(ad.chi_rx_snp.get().valid);
    CHECK(ad.chi_rx_snp.get().bits.opcode == 0x15);
    CHECK(ad.chi_rx_snp.get().bits.src_id == 0x0DD);
    CHECK(ad.zj_tx_snp_rdy.get());

    // valid=0 时 bits 仍在映射（无所谓但确认纯组合无锁存）
    ad.chi_tx_req.set(Valid<CHIREQ>{});
    ad.eval();
    CHECK(!ad.zj_rx_req.get().valid);
}

}  // namespace
