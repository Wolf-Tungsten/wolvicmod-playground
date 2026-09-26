#pragma once

// Ring：10 站环形 NoC（对齐 xijiang/Ring.scala + router/base/BaseRouter.scala +
// router/package.scala 的 RnRouter），拓扑 = kunminghu-v3 单核 DefaultConfig
// （XiangShan/src/main/scala/top/ZhuJiangNoCTopology.scala:25-39）。
//
// 站序（globalId = nodeId>>3）：
//   0 HF(b0p0) | 1 CC | 2 HF(b1p0) | 3 RI | 4 HI(defaultHni) |
//   5 HF(b1p1) | 6 S  | 7 HF(b0p1) | 8 M  | 9 P
//
// 每站每通道（REQ/RSP/DAT/HRQ）：有注入或弹出 → ChannelTap（双方向 tap +
// EjectBuffer + RR 合流），否则 → RingPipe 纯打拍。链路：rings(0).rx 来自左邻
// （idx-1）rings(0).tx，rings(1).rx 来自右邻（idx+1）（Ring.scala:23-26）。
// 注入路径（BaseRouter.scala:192-225）：icn 端口 →（CC/RI 的 REQ 先经 RnRouter
// 地址译码改 TgtID）→ InjQueue(2) → SrcID 盖章（nid 覆写、aid 保留）+ 静态
// 最短路选向（rightNodes/leftNodes，ZJParameters.scala:127-133）→ tap.inject。
// 弹出路径：tap.eject → icn 端口（HRQ 在 CC 侧转 SnoopFlit、S 侧转 HReqFlit）。
// HF 的 HRQ 注入 = ERQ+SNP 经 ResetRRArbiter(2) 合并（BaseRouter.scala:144-148）；
// HI 仅 ERQ 直连。
//
// M 节点两相复位按简化准则省略（全局同步复位、初始态=复位后态）；
// FlitMonitor/NodeRegister（tfbParams=None）不建模。
//
// 同名解包约定的例外说明：本站逻辑由表驱动helper构建，lambda 形参按**角色**
// 命名（rx/injq_deq 等），无法逐站对齐端口名；框架不强制此约定，此处以角色
// 名为准（README §3.9 约定靠评审维持，本文件即评审记录）。

#include <array>
#include <cstdint>
#include <string>
#include <tuple>

#include "model/ring/channel_tap.h"
#include "model/ring/hrq_flit.h"
#include "model/ring/ring_slot.h"
#include "model/flit/zj_flit.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/arb.h"
#include "wolvicmod/prefab/valid.h"
#include "wolvicmod/prefab/queue.h"

namespace zj::ring {

using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Valid;
using wolvicmod::prefab::Queue;
using wolvicmod::prefab::RRArb;
using namespace zj::chi;

enum NodeKind : uint8_t { kHF, kCC, kRI, kHI, kS, kM, kP };

// RnRouter（router/package.scala:11-35）译码参数：仅 CC/RI 的 REQ 注入用
struct RnDec {
    bool     enabled = false;
    uint8_t  ccGid   = 0;      // CC 自身（device 窗口完成者）
    uint64_t ccBase  = 0;      // devAddr=addr[43:0] 窗口 (base, mask)
    uint64_t ccMask  = 0;
    uint8_t  friendGid[2]  = {0, 0};  // HF friends（每 bank 最近者）
    uint8_t  friendBank[2] = {0, 0};
    uint8_t  defaultHniGid = 0;
};

struct StopSpec {
    uint8_t  gid = 0;
    NodeKind kind = kP;
    uint8_t  bankId = 0;
    // 通道存在性（Node.scala:137-153 injects/ejects 经 RouterHelper 映射到环通道）
    bool injReq = false, injRsp = false, injDat = false, injHrq = false;
    bool ejReq = false, ejRsp = false, ejDat = false, ejHrq = false;
    bool hrqInjArb = false;  // HF：ERQ+SNP 两源 RR 仲裁；HI：仅 ERQ 直连
    RnDec  dec;
    uint16_t rightMask = 0, leftMask = 0;  // 按 gid 置位的方向表
};

// 方向表（ZJParameters.scala:127-133）：右侧 = 环序后继的前 half 项，
// 左侧 = 其余；half = 站序奇偶 ? 5 : 4（10 站环，邻站 9 个）。
constexpr std::pair<uint16_t, uint16_t> dirMasks(int i) {
    int ns[9];
    int k = 0;
    for (int j = i + 1; j < 10; ++j) ns[k++] = j;
    for (int j = 0; j < i; ++j) ns[k++] = j;
    const int half = (i % 2 == 1) ? 5 : 4;
    uint16_t r = 0, l = 0;
    for (int j = 0; j < half; ++j) r |= uint16_t(1u << ns[j]);
    for (int j = half; j < 9; ++j) l |= uint16_t(1u << ns[j]);
    return {r, l};
}

constexpr std::array<StopSpec, 10> buildStopTable() {
    std::array<StopSpec, 10> t{};
    constexpr NodeKind kinds[10] = {kHF, kCC, kHF, kRI, kHI, kHF, kS, kHF, kM, kP};
    constexpr uint8_t  banks[10] = {0, 0, 1, 0, 0, 1, 0, 0, 0, 0};
    for (int i = 0; i < 10; ++i) {
        t[i].gid    = uint8_t(i);
        t[i].kind   = kinds[i];
        t[i].bankId = banks[i];
        auto [r, l]     = dirMasks(i);
        t[i].rightMask  = r;
        t[i].leftMask   = l;
    }
    for (auto& s : t) {
        switch (s.kind) {
            case kHF:
                s.ejReq = true;
                s.injRsp = true; s.ejRsp = true;
                s.injDat = true; s.ejDat = true;
                s.injHrq = true; s.hrqInjArb = true;  // ERQ+SNP
                break;
            case kCC:
                s.injReq = true; s.ejReq = true;
                s.injRsp = true; s.ejRsp = true;
                s.injDat = true; s.ejDat = true;
                s.ejHrq = true;  // SNP 弹出
                break;
            case kRI:
                s.injReq = true;
                s.injRsp = true; s.ejRsp = true;
                s.injDat = true; s.ejDat = true;
                break;
            case kHI:
                s.ejReq = true;
                s.injRsp = true; s.ejRsp = true;
                s.injDat = true; s.ejDat = true;
                s.injHrq = true;  // 仅 ERQ
                break;
            case kS:
                s.injRsp = true;
                s.injDat = true; s.ejDat = true;
                s.ejHrq = true;  // ERQ 弹出
                break;
            default: break;
        }
    }
    // RnRouter 译码（CC/RI）：completers = [CC 自身] + friends[HF0(b0), HF2(b1)]，
    // 兜底 defaultHni = HI(gid4)。CC device 窗口：devAddr & mask == base
    // （Node.scala:103：addrSets = (ccId<<20, lanAddrMask^cpuAddrMask)，
    // lanAddrBits=44、cpuSpaceBits=20 → mask = addr[43:20] 全 1）
    RnDec dec;
    dec.enabled        = true;
    dec.ccGid          = 1;
    dec.ccBase         = 0;
    dec.ccMask         = 0xFFFFFF00000ULL;  // addr[43:20]
    dec.friendGid[0]   = 0;
    dec.friendBank[0]  = 0;
    dec.friendGid[1]   = 2;
    dec.friendBank[1]  = 1;
    dec.defaultHniGid  = 4;
    t[1].dec = dec;
    t[3].dec = dec;
    return t;
}

inline constexpr std::array<StopSpec, 10> kStopTable = buildStopTable();

// 弹出 tag 方向表查询：tgt.router（gid）在 right/left 掩码中
inline std::array<bool, 2> tapSelOf(uint16_t tgt_id, const StopSpec& sp) {
    const uint8_t tgtGid = uint8_t(tgt_id >> 3);
    return {bool((sp.rightMask >> tgtGid) & 1), bool((sp.leftMask >> tgtGid) & 1)};
}

// 站边界端口索引视图：每站 icn 端口的指针视图（nullptr = 该站无此端口；
// 实体仍由模块持有、按 n<gid>_* 命名注册）。构建循环与父模块（ZjL3）共用。
// rx_erq/tx_erq 即端口名 *_rx_req/*_tx_req 中载荷为 HReqFlit（ERQ）的那些。
struct StopIO {
    In<Valid<RReqFlit>>*   rx_req       = nullptr;
    Out<bool>*           rx_req_rdy   = nullptr;
    Out<Valid<RReqFlit>>*  tx_req       = nullptr;
    In<bool>*            tx_req_rdy   = nullptr;
    In<Valid<HReqFlit>>*   rx_erq       = nullptr;
    Out<bool>*           rx_erq_rdy   = nullptr;
    Out<Valid<HReqFlit>>*  tx_erq       = nullptr;
    In<bool>*            tx_erq_rdy   = nullptr;
    In<Valid<RespFlit>>*   rx_resp      = nullptr;
    Out<bool>*           rx_resp_rdy  = nullptr;
    Out<Valid<RespFlit>>*  tx_resp      = nullptr;
    In<bool>*            tx_resp_rdy  = nullptr;
    In<Valid<DataFlit>>*   rx_data      = nullptr;
    Out<bool>*           rx_data_rdy  = nullptr;
    Out<Valid<DataFlit>>*  tx_data      = nullptr;
    In<bool>*            tx_data_rdy  = nullptr;
    In<Valid<SnoopFlit>>*  rx_snoop     = nullptr;
    Out<bool>*           rx_snoop_rdy = nullptr;
    Out<Valid<SnoopFlit>>* tx_snoop     = nullptr;
    In<bool>*            tx_snoop_rdy = nullptr;
};

// ---------------- Ring ----------------

class Ring : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(uint8_t, ci);  // io_ci（cluster id，4b；单核恒 0）

    // ---- 边界端口（RTL ZRING 的 icn 端口；命名 <节点>_<rx|tx>_<通道>）----
    // n0/n2/n5/n7 HF：rx req(ERQ,HReqFlit)/resp/data/snoop；tx req(RReqFlit)/resp/data
    IN(Valid<HReqFlit>, n0_rx_req);
    OUT(bool, n0_rx_req_rdy);
    IN(Valid<RespFlit>, n0_rx_resp);
    OUT(bool, n0_rx_resp_rdy);
    IN(Valid<DataFlit>, n0_rx_data);
    OUT(bool, n0_rx_data_rdy);
    IN(Valid<SnoopFlit>, n0_rx_snoop);
    OUT(bool, n0_rx_snoop_rdy);
    OUT(Valid<RReqFlit>, n0_tx_req);
    IN(bool, n0_tx_req_rdy);
    OUT(Valid<RespFlit>, n0_tx_resp);
    IN(bool, n0_tx_resp_rdy);
    OUT(Valid<DataFlit>, n0_tx_data);
    IN(bool, n0_tx_data_rdy);

    IN(Valid<HReqFlit>, n2_rx_req);
    OUT(bool, n2_rx_req_rdy);
    IN(Valid<RespFlit>, n2_rx_resp);
    OUT(bool, n2_rx_resp_rdy);
    IN(Valid<DataFlit>, n2_rx_data);
    OUT(bool, n2_rx_data_rdy);
    IN(Valid<SnoopFlit>, n2_rx_snoop);
    OUT(bool, n2_rx_snoop_rdy);
    OUT(Valid<RReqFlit>, n2_tx_req);
    IN(bool, n2_tx_req_rdy);
    OUT(Valid<RespFlit>, n2_tx_resp);
    IN(bool, n2_tx_resp_rdy);
    OUT(Valid<DataFlit>, n2_tx_data);
    IN(bool, n2_tx_data_rdy);

    IN(Valid<HReqFlit>, n5_rx_req);
    OUT(bool, n5_rx_req_rdy);
    IN(Valid<RespFlit>, n5_rx_resp);
    OUT(bool, n5_rx_resp_rdy);
    IN(Valid<DataFlit>, n5_rx_data);
    OUT(bool, n5_rx_data_rdy);
    IN(Valid<SnoopFlit>, n5_rx_snoop);
    OUT(bool, n5_rx_snoop_rdy);
    OUT(Valid<RReqFlit>, n5_tx_req);
    IN(bool, n5_tx_req_rdy);
    OUT(Valid<RespFlit>, n5_tx_resp);
    IN(bool, n5_tx_resp_rdy);
    OUT(Valid<DataFlit>, n5_tx_data);
    IN(bool, n5_tx_data_rdy);

    IN(Valid<HReqFlit>, n7_rx_req);
    OUT(bool, n7_rx_req_rdy);
    IN(Valid<RespFlit>, n7_rx_resp);
    OUT(bool, n7_rx_resp_rdy);
    IN(Valid<DataFlit>, n7_rx_data);
    OUT(bool, n7_rx_data_rdy);
    IN(Valid<SnoopFlit>, n7_rx_snoop);
    OUT(bool, n7_rx_snoop_rdy);
    OUT(Valid<RReqFlit>, n7_tx_req);
    IN(bool, n7_tx_req_rdy);
    OUT(Valid<RespFlit>, n7_tx_resp);
    IN(bool, n7_tx_resp_rdy);
    OUT(Valid<DataFlit>, n7_tx_data);
    IN(bool, n7_tx_data_rdy);

    // n1 CC：rx req(RReqFlit)/resp/data；tx req/resp/data/snoop(SnoopFlit)
    IN(Valid<RReqFlit>, n1_rx_req);
    OUT(bool, n1_rx_req_rdy);
    IN(Valid<RespFlit>, n1_rx_resp);
    OUT(bool, n1_rx_resp_rdy);
    IN(Valid<DataFlit>, n1_rx_data);
    OUT(bool, n1_rx_data_rdy);
    OUT(Valid<RReqFlit>, n1_tx_req);
    IN(bool, n1_tx_req_rdy);
    OUT(Valid<RespFlit>, n1_tx_resp);
    IN(bool, n1_tx_resp_rdy);
    OUT(Valid<DataFlit>, n1_tx_data);
    IN(bool, n1_tx_data_rdy);
    OUT(Valid<SnoopFlit>, n1_tx_snoop);
    IN(bool, n1_tx_snoop_rdy);

    // n3 RI：rx req(RReqFlit)/resp/data；tx resp/data
    IN(Valid<RReqFlit>, n3_rx_req);
    OUT(bool, n3_rx_req_rdy);
    IN(Valid<RespFlit>, n3_rx_resp);
    OUT(bool, n3_rx_resp_rdy);
    IN(Valid<DataFlit>, n3_rx_data);
    OUT(bool, n3_rx_data_rdy);
    OUT(Valid<RespFlit>, n3_tx_resp);
    IN(bool, n3_tx_resp_rdy);
    OUT(Valid<DataFlit>, n3_tx_data);
    IN(bool, n3_tx_data_rdy);

    // n4 HI：rx req(ERQ,HReqFlit)/resp/data；tx req(RReqFlit)/resp/data
    IN(Valid<HReqFlit>, n4_rx_req);
    OUT(bool, n4_rx_req_rdy);
    IN(Valid<RespFlit>, n4_rx_resp);
    OUT(bool, n4_rx_resp_rdy);
    IN(Valid<DataFlit>, n4_rx_data);
    OUT(bool, n4_rx_data_rdy);
    OUT(Valid<RReqFlit>, n4_tx_req);
    IN(bool, n4_tx_req_rdy);
    OUT(Valid<RespFlit>, n4_tx_resp);
    IN(bool, n4_tx_resp_rdy);
    OUT(Valid<DataFlit>, n4_tx_data);
    IN(bool, n4_tx_data_rdy);

    // n6 S：rx resp/data；tx req(ERQ,HReqFlit)/data
    IN(Valid<RespFlit>, n6_rx_resp);
    OUT(bool, n6_rx_resp_rdy);
    IN(Valid<DataFlit>, n6_rx_data);
    OUT(bool, n6_rx_data_rdy);
    OUT(Valid<HReqFlit>, n6_tx_req);
    IN(bool, n6_tx_req_rdy);
    OUT(Valid<DataFlit>, n6_tx_data);
    IN(bool, n6_tx_data_rdy);

    // 站边界端口索引视图（构造期填充，见 ctor；nullptr = 该站无此端口）
    std::array<StopIO, 10> stops{};

    Ring();

private:
    // 链路端点表（构造期局部使用）：每通道每站每方向的 rx/tx 端口
    template <class F>
    struct LaneEnds {
        std::array<In<RingSlot<F>>*, 10>  rx0{}, rx1{};
        std::array<Out<RingSlot<F>>*, 10> tx0{}, tx1{};
    };

    // 通道构建：有 tap（ChannelTap）或纯打拍（RingPipe×2）；注入侧含
    // InjQueue(2) + SrcID 盖章 + 选向；弹出侧由调用方接边界端口。
    template <class F, uint32_t EjDepth, bool IsDat>
    void buildChannel(int i, LaneEnds<F>& lane, bool hasInj, bool hasEj,
                      ChannelTap<F, EjDepth, IsDat>*& tapOut, Queue<F, 2>*& injqOut,
                      const char* chn) {
        const StopSpec& sp  = kStopTable[i];
        const std::string pfx = "n" + std::to_string(i) + "_" + chn + "_";
        if (!hasInj && !hasEj) {
            auto& p0 = createChildModule<RingPipe<F>>(pfx + "pipe0");
            auto& p1 = createChildModule<RingPipe<F>>(pfx + "pipe1");
            p0.clk = clk;
            p1.clk = clk;
            lane.rx0[i] = &p0.rx;
            lane.rx1[i] = &p1.rx;
            lane.tx0[i] = &p0.tx;
            lane.tx1[i] = &p1.tx;
            tapOut      = nullptr;
            injqOut     = nullptr;
            return;
        }
        auto& tap = createChildModule<ChannelTap<F, EjDepth, IsDat>>(pfx + "tap");
        tap.clk = clk;
        tap.match_tag = uint16_t(sp.gid << 3);
        lane.rx0[i] = &tap.rx0;
        lane.rx1[i] = &tap.rx1;
        lane.tx0[i] = &tap.tx0;
        lane.tx1[i] = &tap.tx1;
        tapOut      = &tap;
        if (hasInj) {
            auto& q = createChildModule<Queue<F, 2>>(pfx + "injq");
            q.clk = clk;
            // SrcID 盖章（BaseRouter.scala:208-210：nid 覆写、aid 保留）+ 注入
            const uint16_t nidBase = uint16_t(sp.gid << 3);
            tap.inject.assign().reads(q.deq) = [nidBase](auto src) {
                auto [q_deq] = src;
                Valid<F> d   = q_deq;
                d.bits.src_id = nidBase | (q_deq.bits.src_id & 0x7);
                return d;
            };
            // 静态最短路选向（BaseRouter.scala:199-200）
            tap.tap_sel_oh.assign().reads(q.deq) = [&sp](auto src) {
                auto [q_deq] = src;
                return tapSelOf(q_deq.bits.tgt_id, sp);
            };
            q.deq_rdy.assign().reads(tap.inject_rdy) = [](auto src) {
                auto [tap_inject_rdy] = src;
                return tap_inject_rdy;
            };
            injqOut = &q;
        } else {
            tap.inject     = Valid<F>{};
            tap.tap_sel_oh = std::array<bool, 2>{};
            injqOut        = nullptr;
        }
        if (!hasEj) tap.eject_rdy = false;
    }

    // RnRouter 译码（CC/RI 的 REQ 注入）：按地址改 TgtID，其余字段直通
    static Valid<RReqFlit> rnDecode(Valid<RReqFlit> in, uint8_t ci, const RnDec& dec);

    // 四个通道的建站 helper（端口空指针 = 该方向不存在，与 STOP_TABLE 一致）
    void buildReqChan(int i, In<Valid<RReqFlit>>* rx, Out<bool>* rxRdy, Out<Valid<RReqFlit>>* tx,
                      In<bool>* txRdy, LaneEnds<RReqFlit>& lane);
    void buildRspChan(int i, In<Valid<RespFlit>>* rx, Out<bool>* rxRdy, Out<Valid<RespFlit>>* tx,
                      In<bool>* txRdy, LaneEnds<RespFlit>& lane);
    void buildDatChan(int i, In<Valid<DataFlit>>* rx, Out<bool>* rxRdy, Out<Valid<DataFlit>>* tx,
                      In<bool>* txRdy, LaneEnds<DataFlit>& lane);
    void buildHrqChan(int i, LaneEnds<HrqFlit>& lane, In<Valid<HReqFlit>>* erqIn,
                      Out<bool>* erqInRdy, In<Valid<SnoopFlit>>* snpIn, Out<bool>* snpInRdy,
                      Out<Valid<HReqFlit>>* erqOut, In<bool>* erqOutRdy,
                      Out<Valid<SnoopFlit>>* snpOut, In<bool>* snpOutRdy);
};


}  // namespace zj::ring
