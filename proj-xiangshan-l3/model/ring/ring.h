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
#include "model/ring_slot.h"
#include "model/wire_conn.h"
#include "model/zj_flit.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/arb.h"
#include "wolvicmod/prefab/dec.h"
#include "wolvicmod/prefab/queue.h"

namespace zj::ring {

using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Dec;
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

// ---------------- Ring ----------------

class Ring : public wolvicmod::Module {
public:
    IN(bool, clk);
    IN(uint8_t, ci);  // io_ci（cluster id，4b；单核恒 0）

    // ---- 边界端口（RTL ZRING 的 icn 端口；命名 <节点>_<rx|tx>_<通道>）----
    // n0/n2/n5/n7 HF：rx req(ERQ,HReqFlit)/resp/data/snoop；tx req(RReqFlit)/resp/data
    IN(Dec<HReqFlit>, n0_rx_req);
    OUT(bool, n0_rx_req_rdy);
    IN(Dec<RespFlit>, n0_rx_resp);
    OUT(bool, n0_rx_resp_rdy);
    IN(Dec<DataFlit>, n0_rx_data);
    OUT(bool, n0_rx_data_rdy);
    IN(Dec<SnoopFlit>, n0_rx_snoop);
    OUT(bool, n0_rx_snoop_rdy);
    OUT(Dec<RReqFlit>, n0_tx_req);
    IN(bool, n0_tx_req_rdy);
    OUT(Dec<RespFlit>, n0_tx_resp);
    IN(bool, n0_tx_resp_rdy);
    OUT(Dec<DataFlit>, n0_tx_data);
    IN(bool, n0_tx_data_rdy);

    IN(Dec<HReqFlit>, n2_rx_req);
    OUT(bool, n2_rx_req_rdy);
    IN(Dec<RespFlit>, n2_rx_resp);
    OUT(bool, n2_rx_resp_rdy);
    IN(Dec<DataFlit>, n2_rx_data);
    OUT(bool, n2_rx_data_rdy);
    IN(Dec<SnoopFlit>, n2_rx_snoop);
    OUT(bool, n2_rx_snoop_rdy);
    OUT(Dec<RReqFlit>, n2_tx_req);
    IN(bool, n2_tx_req_rdy);
    OUT(Dec<RespFlit>, n2_tx_resp);
    IN(bool, n2_tx_resp_rdy);
    OUT(Dec<DataFlit>, n2_tx_data);
    IN(bool, n2_tx_data_rdy);

    IN(Dec<HReqFlit>, n5_rx_req);
    OUT(bool, n5_rx_req_rdy);
    IN(Dec<RespFlit>, n5_rx_resp);
    OUT(bool, n5_rx_resp_rdy);
    IN(Dec<DataFlit>, n5_rx_data);
    OUT(bool, n5_rx_data_rdy);
    IN(Dec<SnoopFlit>, n5_rx_snoop);
    OUT(bool, n5_rx_snoop_rdy);
    OUT(Dec<RReqFlit>, n5_tx_req);
    IN(bool, n5_tx_req_rdy);
    OUT(Dec<RespFlit>, n5_tx_resp);
    IN(bool, n5_tx_resp_rdy);
    OUT(Dec<DataFlit>, n5_tx_data);
    IN(bool, n5_tx_data_rdy);

    IN(Dec<HReqFlit>, n7_rx_req);
    OUT(bool, n7_rx_req_rdy);
    IN(Dec<RespFlit>, n7_rx_resp);
    OUT(bool, n7_rx_resp_rdy);
    IN(Dec<DataFlit>, n7_rx_data);
    OUT(bool, n7_rx_data_rdy);
    IN(Dec<SnoopFlit>, n7_rx_snoop);
    OUT(bool, n7_rx_snoop_rdy);
    OUT(Dec<RReqFlit>, n7_tx_req);
    IN(bool, n7_tx_req_rdy);
    OUT(Dec<RespFlit>, n7_tx_resp);
    IN(bool, n7_tx_resp_rdy);
    OUT(Dec<DataFlit>, n7_tx_data);
    IN(bool, n7_tx_data_rdy);

    // n1 CC：rx req(RReqFlit)/resp/data；tx req/resp/data/snoop(SnoopFlit)
    IN(Dec<RReqFlit>, n1_rx_req);
    OUT(bool, n1_rx_req_rdy);
    IN(Dec<RespFlit>, n1_rx_resp);
    OUT(bool, n1_rx_resp_rdy);
    IN(Dec<DataFlit>, n1_rx_data);
    OUT(bool, n1_rx_data_rdy);
    OUT(Dec<RReqFlit>, n1_tx_req);
    IN(bool, n1_tx_req_rdy);
    OUT(Dec<RespFlit>, n1_tx_resp);
    IN(bool, n1_tx_resp_rdy);
    OUT(Dec<DataFlit>, n1_tx_data);
    IN(bool, n1_tx_data_rdy);
    OUT(Dec<SnoopFlit>, n1_tx_snoop);
    IN(bool, n1_tx_snoop_rdy);

    // n3 RI：rx req(RReqFlit)/resp/data；tx resp/data
    IN(Dec<RReqFlit>, n3_rx_req);
    OUT(bool, n3_rx_req_rdy);
    IN(Dec<RespFlit>, n3_rx_resp);
    OUT(bool, n3_rx_resp_rdy);
    IN(Dec<DataFlit>, n3_rx_data);
    OUT(bool, n3_rx_data_rdy);
    OUT(Dec<RespFlit>, n3_tx_resp);
    IN(bool, n3_tx_resp_rdy);
    OUT(Dec<DataFlit>, n3_tx_data);
    IN(bool, n3_tx_data_rdy);

    // n4 HI：rx req(ERQ,HReqFlit)/resp/data；tx req(RReqFlit)/resp/data
    IN(Dec<HReqFlit>, n4_rx_req);
    OUT(bool, n4_rx_req_rdy);
    IN(Dec<RespFlit>, n4_rx_resp);
    OUT(bool, n4_rx_resp_rdy);
    IN(Dec<DataFlit>, n4_rx_data);
    OUT(bool, n4_rx_data_rdy);
    OUT(Dec<RReqFlit>, n4_tx_req);
    IN(bool, n4_tx_req_rdy);
    OUT(Dec<RespFlit>, n4_tx_resp);
    IN(bool, n4_tx_resp_rdy);
    OUT(Dec<DataFlit>, n4_tx_data);
    IN(bool, n4_tx_data_rdy);

    // n6 S：rx resp/data；tx req(ERQ,HReqFlit)/data
    IN(Dec<RespFlit>, n6_rx_resp);
    OUT(bool, n6_rx_resp_rdy);
    IN(Dec<DataFlit>, n6_rx_data);
    OUT(bool, n6_rx_data_rdy);
    OUT(Dec<HReqFlit>, n6_tx_req);
    IN(bool, n6_tx_req_rdy);
    OUT(Dec<DataFlit>, n6_tx_data);
    IN(bool, n6_tx_data_rdy);

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
                Dec<F> d   = q_deq;
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
            tap.inject     = Dec<F>{};
            tap.tap_sel_oh = std::array<bool, 2>{};
            injqOut        = nullptr;
        }
        if (!hasEj) tap.eject_rdy = false;
    }

    // RnRouter 译码（CC/RI 的 REQ 注入）：按地址改 TgtID，其余字段直通
    static Dec<RReqFlit> rnDecode(Dec<RReqFlit> in, uint8_t ci, const RnDec& dec) {
        const bool     device = (in.bits.mem_attr >> 1) & 1;  // MemAttr.device
        const uint64_t a      = in.bits.addr;
        uint8_t        tgtGid = dec.defaultHniGid;
        if (device && ((a >> 44) & 0xF) == ci && (a & dec.ccMask) == dec.ccBase) {
            tgtGid = dec.ccGid;
        } else if (!device) {
            for (int k = 0; k < 2; ++k)
                if (((a >> 12) & 1) == dec.friendBank[k]) {  // checkBank：addr[12]
                    tgtGid = dec.friendGid[k];
                    break;
                }
        }
        in.bits.tgt_id = uint16_t(tgtGid) << 3;
        return in;
    }

    // 四个通道的建站 helper（端口空指针 = 该方向不存在，与 STOP_TABLE 一致）
    void buildReqChan(int i, In<Dec<RReqFlit>>* rx, Out<bool>* rxRdy, Out<Dec<RReqFlit>>* tx,
                      In<bool>* txRdy, LaneEnds<RReqFlit>& lane);
    void buildRspChan(int i, In<Dec<RespFlit>>* rx, Out<bool>* rxRdy, Out<Dec<RespFlit>>* tx,
                      In<bool>* txRdy, LaneEnds<RespFlit>& lane);
    void buildDatChan(int i, In<Dec<DataFlit>>* rx, Out<bool>* rxRdy, Out<Dec<DataFlit>>* tx,
                      In<bool>* txRdy, LaneEnds<DataFlit>& lane);
    void buildHrqChan(int i, LaneEnds<HrqFlit>& lane, In<Dec<HReqFlit>>* erqIn,
                      Out<bool>* erqInRdy, In<Dec<SnoopFlit>>* snpIn, Out<bool>* snpInRdy,
                      Out<Dec<HReqFlit>>* erqOut, In<bool>* erqOutRdy,
                      Out<Dec<SnoopFlit>>* snpOut, In<bool>* snpOutRdy);
};

// 纯连线接线 detail::wireConn 已上移到 model/wire_conn.h（zj::detail）。

inline void Ring::buildReqChan(int i, In<Dec<RReqFlit>>* rx, Out<bool>* rxRdy,
                               Out<Dec<RReqFlit>>* tx, In<bool>* txRdy,
                               LaneEnds<RReqFlit>& lane) {
    const StopSpec&                 sp = kStopTable[i];
    ChannelTap<RReqFlit, 5, false>* tap;
    Queue<RReqFlit, 2>*             injq;
    buildChannel(i, lane, sp.injReq, sp.ejReq, tap, injq, "req");
    if (injq != nullptr) {
        if (sp.dec.enabled) {  // CC/RI：RnRouter 地址译码改 TgtID
            const RnDec dec = sp.dec;
            injq->enq.assign().reads(*rx, ci) = [dec](auto src) {
                auto [rx, ci] = src;
                return rnDecode(rx, ci, dec);
            };
        } else {
            detail::wireConn(injq->enq, *rx);
        }
        detail::wireConn(*rxRdy, injq->enq_rdy);
    }
    if (tap != nullptr && sp.ejReq) {
        detail::wireConn(*tx, tap->eject);
        detail::wireConn(tap->eject_rdy, *txRdy);
    }
}

inline void Ring::buildRspChan(int i, In<Dec<RespFlit>>* rx, Out<bool>* rxRdy,
                               Out<Dec<RespFlit>>* tx, In<bool>* txRdy,
                               LaneEnds<RespFlit>& lane) {
    const StopSpec&                sp = kStopTable[i];
    ChannelTap<RespFlit, 3, false>* tap;
    Queue<RespFlit, 2>*            injq;
    buildChannel(i, lane, sp.injRsp, sp.ejRsp, tap, injq, "rsp");
    if (injq != nullptr) {
        detail::wireConn(injq->enq, *rx);
        detail::wireConn(*rxRdy, injq->enq_rdy);
    }
    if (tap != nullptr && sp.ejRsp) {
        detail::wireConn(*tx, tap->eject);
        detail::wireConn(tap->eject_rdy, *txRdy);
    }
}

inline void Ring::buildDatChan(int i, In<Dec<DataFlit>>* rx, Out<bool>* rxRdy,
                               Out<Dec<DataFlit>>* tx, In<bool>* txRdy,
                               LaneEnds<DataFlit>& lane) {
    const StopSpec&               sp = kStopTable[i];
    ChannelTap<DataFlit, 3, true>* tap;
    Queue<DataFlit, 2>*           injq;
    buildChannel(i, lane, sp.injDat, sp.ejDat, tap, injq, "dat");
    if (injq != nullptr) {
        detail::wireConn(injq->enq, *rx);
        detail::wireConn(*rxRdy, injq->enq_rdy);
    }
    if (tap != nullptr && sp.ejDat) {
        detail::wireConn(*tx, tap->eject);
        detail::wireConn(tap->eject_rdy, *txRdy);
    }
}

inline void Ring::buildHrqChan(int i, LaneEnds<HrqFlit>& lane, In<Dec<HReqFlit>>* erqIn,
                               Out<bool>* erqInRdy, In<Dec<SnoopFlit>>* snpIn,
                               Out<bool>* snpInRdy, Out<Dec<HReqFlit>>* erqOut,
                               In<bool>* erqOutRdy, Out<Dec<SnoopFlit>>* snpOut,
                               In<bool>* snpOutRdy) {
    const StopSpec&               sp = kStopTable[i];
    const std::string             pfx = "n" + std::to_string(i) + "_";
    ChannelTap<HrqFlit, 5, false>* tap;
    Queue<HrqFlit, 2>*            injq;
    buildChannel(i, lane, sp.injHrq, sp.ejHrq, tap, injq, "hrq");
    if (injq != nullptr) {
        if (sp.hrqInjArb) {
            // HF：ERQ+SNP 经 ResetRRArbiter 合并（BaseRouter.scala:144-148）
            auto& arb = createChildModule<RRArb<HrqFlit, 2>>(pfx + "hrq_arb");
            arb.clk = clk;
            arb.in.assign().reads(*erqIn, *snpIn) = [](auto src) {
                auto [erq, snp] = src;
                std::array<Dec<HrqFlit>, 2> arr;
                arr[0].valid = erq.valid;
                arr[0].bits  = HrqFlit::fromHreq(erq.bits);
                arr[1].valid = snp.valid;
                arr[1].bits  = HrqFlit::fromSnp(snp.bits);
                return arr;
            };
            erqInRdy->assign().reads(arb.in_rdy) = [](auto src) { return std::get<0>(src)[0]; };
            snpInRdy->assign().reads(arb.in_rdy) = [](auto src) { return std::get<0>(src)[1]; };
            detail::wireConn(injq->enq, arb.out);
            arb.out_rdy.assign().reads(injq->enq_rdy) = [](auto src) { return std::get<0>(src); };
        } else {
            // HI：仅 ERQ 直连（BaseRouter.scala:149-150）
            injq->enq.assign().reads(*erqIn) = [](auto src) {
                auto [erq] = src;
                Dec<HrqFlit> d;
                d.valid = erq.valid;
                d.bits  = HrqFlit::fromHreq(erq.bits);
                return d;
            };
            detail::wireConn(*erqInRdy, injq->enq_rdy);
        }
    }
    if (tap != nullptr && sp.ejHrq) {
        if (snpOut != nullptr) {
            // CC：HRQ 车道 → SnoopFlit（RingFlit(128) 截断为 113b，字段 LSB 对齐）
            snpOut->assign().reads(tap->eject) = [](auto src) {
                auto [tap_eject] = src;
                Dec<SnoopFlit> d;
                d.valid = tap_eject.valid;
                d.bits  = tap_eject.bits.toSnp();
                return d;
            };
            detail::wireConn(tap->eject_rdy, *snpOutRdy);
        } else {
            // S：HRQ 车道 → HReqFlit（ERQ）
            erqOut->assign().reads(tap->eject) = [](auto src) {
                auto [tap_eject] = src;
                Dec<HReqFlit> d;
                d.valid = tap_eject.valid;
                d.bits  = tap_eject.bits.toHreq();
                return d;
            };
            detail::wireConn(tap->eject_rdy, *erqOutRdy);
        }
    }
}

inline Ring::Ring() {
    LaneEnds<RReqFlit> reqLane;
    LaneEnds<RespFlit> rspLane;
    LaneEnds<DataFlit> datLane;
    LaneEnds<HrqFlit>  hrqLane;

    // 边界端口指针表（按站索引；nullptr = 无此端口）
    std::array<In<Dec<RReqFlit>>*, 10>  reqRx{};
    std::array<Out<bool>*, 10>          reqRxRdy{};
    std::array<Out<Dec<RReqFlit>>*, 10> reqTx{};
    std::array<In<bool>*, 10>           reqTxRdy{};
    std::array<In<Dec<HReqFlit>>*, 10>  erqRx{};
    std::array<Out<bool>*, 10>          erqRxRdy{};
    std::array<Out<Dec<HReqFlit>>*, 10> erqTx{};
    std::array<In<bool>*, 10>           erqTxRdy{};
    std::array<In<Dec<RespFlit>>*, 10>  rspRx{};
    std::array<Out<bool>*, 10>          rspRxRdy{};
    std::array<Out<Dec<RespFlit>>*, 10> rspTx{};
    std::array<In<bool>*, 10>           rspTxRdy{};
    std::array<In<Dec<DataFlit>>*, 10>  datRx{};
    std::array<Out<bool>*, 10>          datRxRdy{};
    std::array<Out<Dec<DataFlit>>*, 10> datTx{};
    std::array<In<bool>*, 10>           datTxRdy{};
    std::array<In<Dec<SnoopFlit>>*, 10>  snpRx{};
    std::array<Out<bool>*, 10>           snpRxRdy{};
    std::array<Out<Dec<SnoopFlit>>*, 10> snpTx{};
    std::array<In<bool>*, 10>            snpTxRdy{};

    reqRx[1] = &n1_rx_req;  reqRxRdy[1] = &n1_rx_req_rdy;
    reqRx[3] = &n3_rx_req;  reqRxRdy[3] = &n3_rx_req_rdy;
    reqTx[0] = &n0_tx_req;  reqTxRdy[0] = &n0_tx_req_rdy;
    reqTx[1] = &n1_tx_req;  reqTxRdy[1] = &n1_tx_req_rdy;
    reqTx[2] = &n2_tx_req;  reqTxRdy[2] = &n2_tx_req_rdy;
    reqTx[4] = &n4_tx_req;  reqTxRdy[4] = &n4_tx_req_rdy;
    reqTx[5] = &n5_tx_req;  reqTxRdy[5] = &n5_tx_req_rdy;
    reqTx[7] = &n7_tx_req;  reqTxRdy[7] = &n7_tx_req_rdy;

    erqRx[0] = &n0_rx_req;  erqRxRdy[0] = &n0_rx_req_rdy;
    erqRx[2] = &n2_rx_req;  erqRxRdy[2] = &n2_rx_req_rdy;
    erqRx[4] = &n4_rx_req;  erqRxRdy[4] = &n4_rx_req_rdy;
    erqRx[5] = &n5_rx_req;  erqRxRdy[5] = &n5_rx_req_rdy;
    erqRx[7] = &n7_rx_req;  erqRxRdy[7] = &n7_rx_req_rdy;
    erqTx[6] = &n6_tx_req;  erqTxRdy[6] = &n6_tx_req_rdy;

    rspRx[0] = &n0_rx_resp; rspRxRdy[0] = &n0_rx_resp_rdy;
    rspRx[1] = &n1_rx_resp; rspRxRdy[1] = &n1_rx_resp_rdy;
    rspRx[2] = &n2_rx_resp; rspRxRdy[2] = &n2_rx_resp_rdy;
    rspRx[3] = &n3_rx_resp; rspRxRdy[3] = &n3_rx_resp_rdy;
    rspRx[4] = &n4_rx_resp; rspRxRdy[4] = &n4_rx_resp_rdy;
    rspRx[5] = &n5_rx_resp; rspRxRdy[5] = &n5_rx_resp_rdy;
    rspRx[6] = &n6_rx_resp; rspRxRdy[6] = &n6_rx_resp_rdy;
    rspRx[7] = &n7_rx_resp; rspRxRdy[7] = &n7_rx_resp_rdy;
    rspTx[0] = &n0_tx_resp; rspTxRdy[0] = &n0_tx_resp_rdy;
    rspTx[1] = &n1_tx_resp; rspTxRdy[1] = &n1_tx_resp_rdy;
    rspTx[2] = &n2_tx_resp; rspTxRdy[2] = &n2_tx_resp_rdy;
    rspTx[3] = &n3_tx_resp; rspTxRdy[3] = &n3_tx_resp_rdy;
    rspTx[4] = &n4_tx_resp; rspTxRdy[4] = &n4_tx_resp_rdy;
    rspTx[5] = &n5_tx_resp; rspTxRdy[5] = &n5_tx_resp_rdy;
    rspTx[7] = &n7_tx_resp; rspTxRdy[7] = &n7_tx_resp_rdy;

    datRx[0] = &n0_rx_data; datRxRdy[0] = &n0_rx_data_rdy;
    datRx[1] = &n1_rx_data; datRxRdy[1] = &n1_rx_data_rdy;
    datRx[2] = &n2_rx_data; datRxRdy[2] = &n2_rx_data_rdy;
    datRx[3] = &n3_rx_data; datRxRdy[3] = &n3_rx_data_rdy;
    datRx[4] = &n4_rx_data; datRxRdy[4] = &n4_rx_data_rdy;
    datRx[5] = &n5_rx_data; datRxRdy[5] = &n5_rx_data_rdy;
    datRx[6] = &n6_rx_data; datRxRdy[6] = &n6_rx_data_rdy;
    datRx[7] = &n7_rx_data; datRxRdy[7] = &n7_rx_data_rdy;
    datTx[0] = &n0_tx_data; datTxRdy[0] = &n0_tx_data_rdy;
    datTx[1] = &n1_tx_data; datTxRdy[1] = &n1_tx_data_rdy;
    datTx[2] = &n2_tx_data; datTxRdy[2] = &n2_tx_data_rdy;
    datTx[3] = &n3_tx_data; datTxRdy[3] = &n3_tx_data_rdy;
    datTx[4] = &n4_tx_data; datTxRdy[4] = &n4_tx_data_rdy;
    datTx[5] = &n5_tx_data; datTxRdy[5] = &n5_tx_data_rdy;
    datTx[6] = &n6_tx_data; datTxRdy[6] = &n6_tx_data_rdy;
    datTx[7] = &n7_tx_data; datTxRdy[7] = &n7_tx_data_rdy;

    snpRx[0] = &n0_rx_snoop; snpRxRdy[0] = &n0_rx_snoop_rdy;
    snpRx[2] = &n2_rx_snoop; snpRxRdy[2] = &n2_rx_snoop_rdy;
    snpRx[5] = &n5_rx_snoop; snpRxRdy[5] = &n5_rx_snoop_rdy;
    snpRx[7] = &n7_rx_snoop; snpRxRdy[7] = &n7_rx_snoop_rdy;
    snpTx[1] = &n1_tx_snoop; snpTxRdy[1] = &n1_tx_snoop_rdy;

    // 建站
    for (int i = 0; i < 10; ++i) {
        buildReqChan(i, reqRx[i], reqRxRdy[i], reqTx[i], reqTxRdy[i], reqLane);
        buildRspChan(i, rspRx[i], rspRxRdy[i], rspTx[i], rspTxRdy[i], rspLane);
        buildDatChan(i, datRx[i], datRxRdy[i], datTx[i], datTxRdy[i], datLane);
        buildHrqChan(i, hrqLane, erqRx[i], erqRxRdy[i], snpRx[i], snpRxRdy[i], erqTx[i],
                     erqTxRdy[i], snpTx[i], snpTxRdy[i]);
    }

    // 链路（Ring.scala:23-26）：rings(0).rx ← 左邻 rings(0).tx；rings(1).rx ← 右邻
    for (int i = 0; i < 10; ++i) {
        const int l = (i + 9) % 10, r = (i + 1) % 10;
        detail::wireConn(*reqLane.rx0[i], *reqLane.tx0[l]);
        detail::wireConn(*reqLane.rx1[i], *reqLane.tx1[r]);
        detail::wireConn(*rspLane.rx0[i], *rspLane.tx0[l]);
        detail::wireConn(*rspLane.rx1[i], *rspLane.tx1[r]);
        detail::wireConn(*datLane.rx0[i], *datLane.tx0[l]);
        detail::wireConn(*datLane.rx1[i], *datLane.tx1[r]);
        detail::wireConn(*hrqLane.rx0[i], *hrqLane.tx0[l]);
        detail::wireConn(*hrqLane.rx1[i], *hrqLane.tx1[r]);
    }
}

}  // namespace zj::ring
