// 项目侧 SRAM 模板单测：SpSram / DpSram（xs-utils 模板拍级对齐）。
// 覆盖：读延迟拍数精确（1 拍 / Directory 3 拍 / BeatStorage 5 拍 / 双口 2 拍）；
// intvCnt 回压间隔；掩码写；双口同拍读写同址时写优先（bypass）/读旧值
// （no-bypass）；ShouldReset 的复位横扫回压与零初值。

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <prefab/prefab.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace zj::prefab;
using namespace prefabtest;

namespace {

template <class T, uint32_t Ways>
Dec<SramReqBits<T, Ways>> wrReq(uint32_t addr, uint32_t mask, std::array<T, Ways> data) {
    return {true, {true, addr, mask, data}};
}

template <class T, uint32_t Ways>
Dec<SramReqBits<T, Ways>> rdReq(uint32_t addr) {
    return {true, {false, addr, 0, {}}};
}

template <class T, uint32_t Ways>
Dec<SramReqBits<T, Ways>> idleReq() {
    return {false, {}};
}

TEST_CASE("zj SpSram(1,1): 基本读写，读延迟 1 拍") {
    SpSram<uint32_t, 16, 1, 1, 1> top;  // Setup=1 Latency=1，kReadDelay=1
    top.elaborate();
    top.req.set(idleReq<uint32_t, 1>());

    // 拍 0：写 addr3 = 0xAAAA
    top.req.set(wrReq<uint32_t, 1>(3, 0, {0xAAAA}));
    comb(top);
    CHECK(top.req_rdy.get() == true);   // kInterval=1：恒 ready
    CHECK(top.resp.get().valid == false);
    edge(top);
    // 拍 1：读 addr3
    top.req.set(rdReq<uint32_t, 1>(3));
    comb(top);
    CHECK(top.resp.get().valid == false);
    edge(top);
    // 拍 2：resp 到达
    top.req.set(idleReq<uint32_t, 1>());
    comb(top);
    CHECK(top.resp.get().valid == true);
    CHECK(top.resp.get().bits.data[0] == 0xAAAA);
    edge(top);
    comb(top);
    CHECK(top.resp.get().valid == false);
    edge(top);
}

TEST_CASE("zj SpSram(1,2,+outreg)（Directory 配置）: 读延迟恰好 3 拍、intv 回压") {
    SpSram<uint32_t, 16, 2, 1, 2, false, true> top;  // kReadDelay=3, kInterval=2
    top.elaborate();
    top.req.set(idleReq<uint32_t, 2>());

    // 拍 0：写 addr5 mask=0b11 data{111,222}
    top.req.set(wrReq<uint32_t, 2>(5, 0b11, {111, 222}));
    comb(top);
    CHECK(top.req_rdy.get() == true);
    edge(top);
    // 拍 1：intv 回压
    comb(top);
    CHECK(top.req_rdy.get() == false);
    edge(top);
    // 拍 2：读 addr5
    top.req.set(rdReq<uint32_t, 2>(5));
    comb(top);
    CHECK(top.req_rdy.get() == true);
    CHECK(top.resp.get().valid == false);
    edge(top);
    // 拍 3、4：未到（恰好 3 拍才到）
    top.req.set(idleReq<uint32_t, 2>());
    comb(top);
    CHECK(top.resp.get().valid == false);
    edge(top);
    comb(top);
    CHECK(top.resp.get().valid == false);
    edge(top);
    // 拍 5：resp 到达
    comb(top);
    CHECK(top.resp.get().valid == true);
    CHECK(top.resp.get().bits.data[0] == 111);
    CHECK(top.resp.get().bits.data[1] == 222);
    edge(top);
}

TEST_CASE("zj SpSram(1,2,+outreg): way 掩码写") {
    SpSram<uint32_t, 16, 2, 1, 2, false, true> top;
    top.elaborate();
    top.req.set(idleReq<uint32_t, 2>());
    cycle(top);

    // 写 addr6 mask=0b01 data{333,444}：仅 way0 落 333，way1 保持
    top.req.set(wrReq<uint32_t, 2>(6, 0b01, {333, 444}));
    cycle(top);
    top.req.set(idleReq<uint32_t, 2>());
    cycle(top);
    top.req.set(rdReq<uint32_t, 2>(6));
    cycle(top);
    top.req.set(idleReq<uint32_t, 2>());
    cycle(top);
    cycle(top);
    comb(top);
    CHECK(top.resp.get().valid == true);
    CHECK(top.resp.get().bits.data[0] == 333);
    CHECK(top.resp.get().bits.data[1] == 0);   // 掩码位未写
    edge(top);

    // 写 addr7 mask=0b10 data{555,666}：仅 way1 落 666
    top.req.set(wrReq<uint32_t, 2>(7, 0b10, {555, 666}));
    cycle(top);
    top.req.set(idleReq<uint32_t, 2>());
    cycle(top);
    top.req.set(rdReq<uint32_t, 2>(7));
    cycle(top);
    top.req.set(idleReq<uint32_t, 2>());
    cycle(top);
    cycle(top);
    comb(top);
    CHECK(top.resp.get().valid == true);
    CHECK(top.resp.get().bits.data[0] == 0);
    CHECK(top.resp.get().bits.data[1] == 666);
    edge(top);
}

TEST_CASE("zj SpSram(2,2,+outreg)（BeatStorage 配置）: 读延迟恰好 5 拍") {
    SpSram<uint32_t, 16, 1, 2, 2, false, true> top;  // kIsc=2, kReadDelay=5
    top.elaborate();
    top.req.set(idleReq<uint32_t, 1>());

    // 拍 0：写 addr9 = 0xBEEF（kIsc=2：写提交推迟 1 拍）
    top.req.set(wrReq<uint32_t, 1>(9, 0, {0xBEEF}));
    comb(top);
    CHECK(top.req_rdy.get() == true);
    edge(top);
    // 拍 1：intv 回压（kInterval=2）
    comb(top);
    CHECK(top.req_rdy.get() == false);
    edge(top);
    // 拍 2：读 addr9
    top.req.set(rdReq<uint32_t, 1>(9));
    comb(top);
    CHECK(top.req_rdy.get() == true);
    edge(top);
    // 拍 3..6：未到（恰好 5 拍才到）
    top.req.set(idleReq<uint32_t, 1>());
    for (int c = 0; c < 4; ++c) {
        comb(top);
        CHECK(top.resp.get().valid == false);
        edge(top);
    }
    // 拍 7：resp 到达（fire 于拍 2 → 2+5）
    comb(top);
    CHECK(top.resp.get().valid == true);
    CHECK(top.resp.get().bits.data[0] == 0xBEEF);
    edge(top);
}

TEST_CASE("zj DpSram(1,1,bypass,+outreg)（replArray 配置）: 读 2 拍、同拍读写同址写优先") {
    DpSram<uint32_t, 16, 1, true, 1, 1, false, true> top;  // kReadDelay=2
    top.elaborate();
    top.wreq.set(idleReq<uint32_t, 1>());
    top.rreq.set({false, 0});

    // 拍 0：写 addr7 = 0x1234
    top.wreq.set(wrReq<uint32_t, 1>(7, 0, {0x1234}));
    comb(top);
    CHECK(top.wreq_rdy.get() == true);
    CHECK(top.rreq_rdy.get() == true);   // R/W 计数器独立
    edge(top);
    // 拍 1：读 addr7
    top.wreq.set(idleReq<uint32_t, 1>());
    top.rreq.set({true, 7});
    comb(top);
    CHECK(top.rresp.get().valid == false);
    edge(top);
    // 拍 2：未到
    top.rreq.set({false, 0});
    comb(top);
    CHECK(top.rresp.get().valid == false);
    edge(top);
    // 拍 3：resp 到达
    comb(top);
    CHECK(top.rresp.get().valid == true);
    CHECK(top.rresp.get().bits.data[0] == 0x1234);
    edge(top);

    // 同拍读写同址（addr9）：bypass → 读侧拿到新写数据（写优先）
    top.wreq.set(wrReq<uint32_t, 1>(9, 0, {0x9999}));
    top.rreq.set({true, 9});
    comb(top);
    CHECK(top.wreq_rdy.get() == true);
    CHECK(top.rreq_rdy.get() == true);
    edge(top);
    top.wreq.set(idleReq<uint32_t, 1>());
    top.rreq.set({false, 0});
    comb(top);
    CHECK(top.rresp.get().valid == false);
    edge(top);
    comb(top);
    CHECK(top.rresp.get().valid == true);
    CHECK(top.rresp.get().bits.data[0] == 0x9999);   // 新数据而非旧值 0
    edge(top);
}

TEST_CASE("zj DpSram(1,1,nobypass,+outreg): 同拍读写同址读新值（对齐 Verilator 下件）") {
    DpSram<uint32_t, 16, 1, false, 1, 1, false, true> top;
    top.elaborate();
    top.wreq.set(idleReq<uint32_t, 1>());
    top.rreq.set({false, 0});

    // 拍 0：写 addr4 = 111
    top.wreq.set(wrReq<uint32_t, 1>(4, 0, {111}));
    cycle(top);
    // 拍 1：同拍写 addr4 = 222 + 读 addr4 → 读侧见新值 222（firtool 对
    // SyncReadMem 的下件是"读地址打拍、下拍组合取"，写在 fire 拍末已提交；
    // chisel 语义上属 Undefined RDW，这里对齐 Verilator golden）
    top.wreq.set(wrReq<uint32_t, 1>(4, 0, {222}));
    top.rreq.set({true, 4});
    cycle(top);
    top.wreq.set(idleReq<uint32_t, 1>());
    top.rreq.set({false, 0});
    cycle(top);
    comb(top);
    CHECK(top.rresp.get().valid == true);
    CHECK(top.rresp.get().bits.data[0] == 222);   // 新值
    edge(top);
    // 再读 addr4：已是新值 222
    top.rreq.set({true, 4});
    cycle(top);
    top.rreq.set({false, 0});
    cycle(top);
    comb(top);
    CHECK(top.rresp.get().valid == true);
    CHECK(top.rresp.get().bits.data[0] == 222);
    edge(top);
}

TEST_CASE("zj SpSram(ShouldReset): 复位横扫期间 req_rdy 拉低，之后读出零初值") {
    SpSram<uint32_t, 4, 1, 1, 1, false, false, true> top;  // kRstCycles=4+4=8
    top.elaborate();
    top.req.set(idleReq<uint32_t, 1>());

    // 拍 0..7：横扫进行（4 拍 resetHold + 4 set 写零），req_rdy 拉低
    for (int c = 0; c < 8; ++c) {
        comb(top);
        CHECK(top.req_rdy.get() == false);
        edge(top);
    }
    // 拍 8：ready 恢复；读未写过的 addr2 → 零初值（Mem 零初始化=复位横扫结果）
    comb(top);
    CHECK(top.req_rdy.get() == true);
    top.req.set(rdReq<uint32_t, 1>(2));
    edge(top);
    top.req.set(idleReq<uint32_t, 1>());
    comb(top);
    CHECK(top.resp.get().valid == true);
    CHECK(top.resp.get().bits.data[0] == 0);
    edge(top);
}

}  // namespace
