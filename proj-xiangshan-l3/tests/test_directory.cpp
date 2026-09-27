// Directory / DirectoryBase 单测：PLRU 纯函数、复位横扫窗口、读 miss/命中、
// wriNoHit 分配+回读、sf reservation 挤占、Directory 顶层读联动与写分发。
// 时序预期值推导见 docs/dongjiang-semantics.md §3（对拍 harness 做全量验证）。

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

#include "model/dj/directory.h"
#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace zj::dj;
using namespace prefabtest;

namespace {

using SfDir = DirectoryBase<DirSfCfg>;

// sf 侧构造地址：catAddr 即 {bank, tag, set, dirBank} 重组（offset=0）
uint64_t sfAddr(uint32_t bank, uint32_t tag, uint32_t set, uint32_t db) {
    return catAddr(bank, tag, set, DirSfCfg::kSetBits, db);
}

void skipReset(SfDir& d) {  // sf 横扫 4+1024 拍 + rst_done 1 拍，富余推进
    for (int i = 0; i < 1100; ++i) cycle(d);
    d.read.set({false, {}});
    d.write.set({false, {}});
    d.unlock.set({false, 0});
    comb(d);
}

TEST_CASE("dj PLRU: 对齐 rocket-chip PLRUTest 真值表（2..6 路）") {
    // 真值表逐字来自 rocket-chip util/Replacement.scala PLRUTest（同一源码）
    static const uint8_t repl2[] = {0, 1};
    static const uint8_t repl3[] = {0, 1, 2, 2};
    static const uint8_t repl4[] = {0, 1, 0, 1, 2, 2, 3, 3};
    static const uint8_t repl5[] = {0, 1, 0, 1, 2, 2, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4};
    static const uint8_t repl6[] = {0, 1, 0, 1, 2, 2, 3, 3, 0, 1, 0, 1, 2, 2, 3, 3,
                                    4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5};
    for (uint32_t s = 0; s < 2; ++s) CHECK(plruReplaceWay(s, 2) == repl2[s]);
    for (uint32_t s = 0; s < 4; ++s) CHECK(plruReplaceWay(s, 3) == repl3[s]);
    for (uint32_t s = 0; s < 8; ++s) CHECK(plruReplaceWay(s, 4) == repl4[s]);
    for (uint32_t s = 0; s < 16; ++s) CHECK(plruReplaceWay(s, 5) == repl5[s]);
    for (uint32_t s = 0; s < 32; ++s) CHECK(plruReplaceWay(s, 6) == repl6[s]);
    // 4 路 get_next_state 全表（state × touchWay）
    static const uint8_t next4[8][4] = {
        {5, 4, 2, 0}, {5, 4, 3, 1}, {7, 6, 2, 0}, {7, 6, 3, 1},
        {5, 4, 2, 0}, {5, 4, 3, 1}, {7, 6, 2, 0}, {7, 6, 3, 1},
    };
    for (uint32_t s = 0; s < 8; ++s)
        for (uint32_t w = 0; w < 4; ++w) CHECK(plruNextState(s, w, 4) == next4[s][w]);
    // 16 路：状态位不越界 + 路径不变量（touch 某路后，该路所在半区变新）
    uint32_t st = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t way = plruReplaceWay(st, 16);
        CHECK(way < 16);
        st = plruNextState(st, way, 16);
        CHECK(st < (1u << 15));
    }
}

TEST_CASE("dj DirectoryBase(sf): 复位横扫窗口内 read_rdy 拉低，结束后恢复") {
    SfDir d;
    d.elaborate();
    d.clk_en.set(true);
    d.cfg_bank_id.set(0);
    d.dir_bank.set(0);
    d.read.set({false, {}});
    d.write.set({false, {}});
    d.unlock.set({false, 0});
    // sf SRAM 横扫 = 4 + 1024 拍；末笔横扫写重装 intvCnt（interval=2 → 再 1 拍）；
    // 三个 ready 同拍为真后 rst_done 再锁存 1 拍
    uint64_t firstRdy = 0;
    for (uint64_t c = 0; c < 1100; ++c) {
        comb(d);
        if (d.read_rdy.get() && firstRdy == 0) firstRdy = c + 1;  // 1 起始计数
        edge(d);
    }
    CHECK(firstRdy == 4 + 1024 + 3);
}

TEST_CASE("dj DirectoryBase(sf): 读 miss 4 拍出响应，命中 way0，请求隔 2 拍") {
    SfDir d;
    d.elaborate();
    d.clk_en.set(true);
    d.cfg_bank_id.set(0);
    d.dir_bank.set(0);
    skipReset(d);

    const uint64_t a = sfAddr(0, 0x123, 7, 0);
    d.read.set({true, {a, hnIdxOf(0, 0, 0)}});
    comb(d);
    CHECK(d.read_rdy.get() == true);
    edge(d);  // d0
    comb(d);
    CHECK(d.read_rdy.get() == false);  // 隔 2 拍
    CHECK(d.resp.get().valid == false);
    d.read.set({false, {}});
    edge(d);  // d1
    comb(d);
    CHECK(d.read_rdy.get() == true);  // 恢复
    CHECK(d.resp.get().valid == false);
    edge(d);  // d2
    comb(d);
    CHECK(d.resp.get().valid == false);
    edge(d);  // d3
    comb(d);  // d4
    CHECK(d.resp.get().valid == true);
    CHECK(d.resp.get().bits.hit == false);
    CHECK(d.resp.get().bits.toRepl == false);
    CHECK(d.resp.get().bits.wayOH == 0x1);
    CHECK(d.resp.get().bits.meta == 0);
    CHECK(d.resp.get().bits.hnTxnID == hnIdxOf(0, 0, 0));
    edge(d);
    comb(d);
    CHECK(d.resp.get().valid == false);  // 单拍脉冲
    edge(d);
}

TEST_CASE("dj DirectoryBase(sf): wriNoHit 出 wResp(toRepl) 并回读命中") {
    SfDir d;
    d.elaborate();
    d.clk_en.set(true);
    d.cfg_bank_id.set(0);
    d.dir_bank.set(0);
    skipReset(d);

    const uint64_t a = sfAddr(0, 0x55, 9, 0);
    DirWrReq wr;
    wr.addr = a;
    wr.wayOH = 0;          // wriNoHit：wayOH 无用
    wr.meta = 1;           // sf：valid
    wr.hnIdx = hnIdxOf(0, 1, 2);
    wr.hit = false;
    wr.directAlloc = false;
    d.write.set({true, wr});
    comb(d);
    CHECK(d.write_rdy.get() == true);
    edge(d);  // d0
    d.write.set({false, {}});
    for (int i = 0; i < 3; ++i) {
        comb(d);
        CHECK(d.resp.get().valid == false);
        edge(d);
    }
    comb(d);  // d4
    CHECK(d.resp.get().valid == true);
    CHECK(d.resp.get().bits.toRepl == true);
    CHECK(d.resp.get().bits.hit == false);
    CHECK(d.resp.get().bits.wayOH == 0x1);
    CHECK(d.resp.get().bits.hnTxnID == hnIdxOf(0, 1, 2));
    // victim 地址 = bank + 牺牲 way 的 tag（way0 从未写入，tag=0）+ set + dirBank
    CHECK(d.resp.get().bits.addr == catAddr(0, 0, 9, DirSfCfg::kSetBits, 0));
    edge(d);
    comb(d);
    CHECK(d.resp.get().valid == false);
    edge(d);

    // 回读同址：命中（d4 分配写回已提交）
    d.read.set({true, {a, hnIdxOf(0, 1, 2)}});
    comb(d);
    edge(d);
    d.read.set({false, {}});
    for (int i = 0; i < 3; ++i) {
        comb(d);
        edge(d);
    }
    comb(d);
    CHECK(d.resp.get().valid == true);
    CHECK(d.resp.get().bits.toRepl == false);
    CHECK(d.resp.get().bits.hit == true);
    CHECK(d.resp.get().bits.wayOH == 0x1);
    CHECK(d.resp.get().bits.meta == 1);
    edge(d);

    // 同 hnIdx unlock（读命中已在 d3 置锁）→ 不挂起即可
    d.unlock.set({true, hnIdxOf(0, 1, 2)});
    cycle(d);
    d.unlock.set({false, 0});
    comb(d);
}

TEST_CASE("dj DirectoryBase(sf): reservation 挤占——同集第二个 miss 避让已预留 way") {
    SfDir d;
    d.elaborate();
    d.clk_en.set(true);
    d.cfg_bank_id.set(0);
    d.dir_bank.set(0);
    skipReset(d);

    const uint64_t a = sfAddr(0, 0x10, 33, 0);
    const uint64_t b = sfAddr(0, 0x20, 33, 0);  // 同 sf set
    // t：读 A（hnIdx slot(0,1)）miss → d3(t+3) 置 reservation{set33, way0}
    d.read.set({true, {a, hnIdxOf(0, 0, 1)}});
    cycle(d);  // t
    d.read.set({false, {}});
    cycle(d);  // t+1
    // t+2：读 B（不同 slot）——B 的 d3(t+5) 在 A 的 d3(t+3) 之后，reservation 生效
    d.read.set({true, {b, hnIdxOf(0, 0, 2)}});
    cycle(d);  // t+2
    d.read.set({false, {}});
    comb(d);  // t+3
    CHECK(d.resp.get().valid == false);
    edge(d);
    comb(d);  // t+4：A 的响应
    CHECK(d.resp.get().valid == true);
    CHECK(d.resp.get().bits.hit == false);
    CHECK(d.resp.get().bits.wayOH == 0x1);
    edge(d);
    comb(d);  // t+5
    CHECK(d.resp.get().valid == false);
    edge(d);
    comb(d);  // t+6：B 的响应——way0 被 A 预留 → 选 way1
    CHECK(d.resp.get().valid == true);
    CHECK(d.resp.get().bits.hit == false);
    CHECK(d.resp.get().bits.wayOH == 0x2);
    edge(d);
}

TEST_CASE("dj Directory: 顶层读联动与 wResp 路由") {
    Directory d;
    d.elaborate();
    d.clk_en.set(true);
    d.cfg_bank_id.set(0);
    d.read_0.set({false, {}});
    d.read_1.set({false, {}});
    d.write.set({false, {}});
    d.unlock.set({false, 0});
    for (int i = 0; i < 9000; ++i) cycle(d);  // llc 横扫 4+8192

    // bank0 读：llc+sf 同拍进、同拍出
    const uint64_t a = catAddr(0, 0x77, 5, DirLlcCfg::kSetBits, 0);
    d.read_0.set({true, {a, hnIdxOf(0, 0, 0)}});
    comb(d);
    CHECK(d.read_0_rdy.get() == true);
    CHECK(d.read_1_rdy.get() == true);  // bank1 空闲
    edge(d);
    d.read_0.set({false, {}});
    for (int i = 0; i < 3; ++i) {
        comb(d);
        CHECK(d.rresp_0.get().valid == false);
        edge(d);
    }
    comb(d);
    CHECK(d.rresp_0.get().valid == true);
    CHECK(d.rresp_0.get().bits.llc.hit == false);
    CHECK(d.rresp_0.get().bits.llc.wayOH == 0x1);
    CHECK(d.rresp_0.get().bits.sf.hit == false);
    CHECK(d.rresp_0.get().bits.sf.wayOH == 0x1);
    CHECK(d.wresp_llc.get().valid == false);
    CHECK(d.wresp_sf.get().valid == false);
    edge(d);
    comb(d);
    CHECK(d.rresp_0.get().valid == false);
    edge(d);

    // 写 llc 侧（wriNoHit 到 bank0）：wresp_llc 出、wresp_sf 不出
    DirWrBoth wb;
    wb.llcValid = true;
    wb.llc.addr = a;
    wb.llc.meta = 3;  // UC
    wb.llc.hnIdx = hnIdxOf(0, 0, 0);
    wb.llc.hit = false;
    wb.llc.directAlloc = false;
    d.write.set({true, wb});
    comb(d);
    CHECK(d.write_rdy.get() == true);
    edge(d);
    d.write.set({false, {}});
    for (int i = 0; i < 3; ++i) {
        comb(d);
        CHECK(d.wresp_llc.get().valid == false);
        edge(d);
    }
    comb(d);
    CHECK(d.wresp_llc.get().valid == true);
    // victim 地址：牺牲 way0 的 tag 从未写入（SRAM 零初值）→ tag=0
    CHECK(d.wresp_llc.get().bits.addr == catAddr(0, 0, 5, DirLlcCfg::kSetBits, 0));
    CHECK(d.wresp_llc.get().bits.hnTxnID == hnIdxOf(0, 0, 0));
    CHECK(d.wresp_sf.get().valid == false);
    edge(d);
    comb(d);
    CHECK(d.wresp_llc.get().valid == false);
    edge(d);
}

}  // namespace
