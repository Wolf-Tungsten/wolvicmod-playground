#pragma once

// HiNodeAxiLiteBridge：HI 节点（gid4，defaultHni）的 CHI-HNI→AXI4 桥，对齐
// zhujiang/device/bridge/axilite/AxiLiteBridge.scala。kunminghu-v3：
// outstanding=8（AxiDeviceParams 默认）、AXI id 3b / addr 48b / data 256b
// （IoWrapper 传 busDataBits=cfgAxiDataBits=L3OuterBusWidth=256、tagOffset=3）、
// compareTag=addr[18:3]。
//
// 结构同 S 桥简化版（AxiLiteBridge.scala:33-141）：
//   8×BridgeCm<CmHiT>（CM 内带 64b 数据/8b 掩码，无 dataBuffer/alloc）；
//   tx_resp/aw/ar 各经 ConditionVipArbiter(qos>=) 合流；awQueue(8) 保序 →
//   W 直出（CM 侧驱动 slvData/slvMask/last=true，Mux1H 选择）；
//   rx.resp（CompAck）/rx.data 按 TxnID 分发且恒 ready；
//   icn.tx.req（ERQ）恒 invalid（AxiLiteBridge.scala:33-34），模型无此端口；
//   axi.r → readDataPipe(Queue(1,pipe)) 组 CompData → tx_data。
// **不可省略**（层次文档 §3.7）：coremark 的 UART MMIO 全走 CC→defaultHni→cfgAXI。

#include <array>
#include <cstdint>
#include <string>

#include "model/bridge/axi_flit.h"
#include "model/bridge/bridge_cm.h"
#include "model/flit/zj_flit.h"
#include "prefab/xsarb.h"
#include "wolvicmod/core/collect.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"
#include "wolvicmod/prefab/queue.h"

namespace zj::bridge {

using namespace zj::chi;
using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Valid;
using wolvicmod::prefab::Queue;
using zj::prefab::CondVipArb;

// ---------------- CmHiT：HI 桥 CM traits ----------------

struct CmHiT {
    static constexpr bool     kSn      = false;
    static constexpr uint32_t kOutst   = 8;
    static constexpr uint32_t kAllOnes = 7;  // Fill(3, 1)
    using ReqT = RReqFlit;

    // AxiLiteCtrlInfo（mem=false；ioDataBits=64/withData=true）
    struct Info {
        uint64_t addr      = 0;  // [47:0]
        uint8_t  size      = 0;  // [2:0]（RTL 断言 <=3）
        uint16_t txn_id    = 0;  // [11:0]
        uint16_t src_id    = 0;  // [10:0]
        bool     ewa       = false;
        bool     device    = false;
        uint8_t  readCnt   = 0;
        uint8_t  qos       = 0;  // [3:0]
        bool     isSnooped = false;
        uint64_t data      = 0;  // 写数据 64b（icn 256b 中按 addr[4:3] 抽取）
        uint8_t  mask      = 0;  // 写掩码 8b

        bool operator==(const Info&) const = default;
    };

    static bool tagMatch(uint64_t a, uint64_t b) {  // addr[18:3]
        return ((a >> 3) & 0xFFFFULL) == ((b >> 3) & 0xFFFFULL);
    }
    static bool uCompleted(const UState& u) {  // compAck 计入（sn=false）
        return u.receiptResp && u.dbidResp && u.wdata && u.rdata && u.compAck && u.comp;
    }
    static bool     dwtOf(const Info&) { return false; }        // mem=false
    static uint16_t returnNidOf(const Info&) { return 0; }      // getOrElse(0)
    static uint16_t returnTxnIdOf(const Info&) { return 0; }

    static void enqEntry(const ReqT& req, UState& u, DState& d, Info& info, bool&) {
        info.addr      = req.addr;
        info.size      = req.size;
        info.txn_id    = req.txn_id;
        info.src_id    = req.src_id;
        info.ewa       = req.mem_attr & 1;
        info.device    = (req.mem_attr >> 1) & 1;
        info.readCnt   = 0;
        info.isSnooped = true;
        info.qos       = req.qos;
        info.data      = 0;
        info.mask      = 0;
        decodeU(req, u);
        decodeD(req, d);
    }

    static void decodeU(const ReqT& req, UState& u) {
        // ChiUpstreamOpVec.decode（合法 opcode：ReadNoSnp/WriteNoSnpPtl —— RTL 断言）
        if (req.opcode == req_op::kReadNoSnp) {
            u.receiptResp = req.order == 0;
            u.dbidResp = true; u.wdata = true; u.rdata = false; u.comp = true;
        } else {
            u.receiptResp = true; u.dbidResp = false; u.wdata = false;
            u.rdata = true; u.comp = false;
        }
        u.compAck = !req.exp_comp_ack;
    }

    static void decodeD(const ReqT& req, DState& d) {
        if (req.opcode == req_op::kReadNoSnp) {
            d.waddr = true; d.raddr = false; d.wdata = true; d.wresp = true;
            d.rdata = false;
        } else {
            d.waddr = false; d.raddr = true; d.wdata = false; d.wresp = false;
            d.rdata = true;
        }
    }

    // 写数据/掩码抽取（BaseCtrlMachine.scala:106-113；icn 256b 按 addr[4:3] 选
    // 64b/8b 块）
    static void extractData(uint64_t addr, const DataFlit& f, Info& info) {
        const uint32_t idx = (addr >> 3) & 3;
        info.data          = f.data[idx];
        info.mask          = uint8_t(f.be >> (idx * 8)) & 0xFF;
    }

    static axi::AxFlit mkAx(const Info& info, uint32_t idx) {
        axi::AxFlit f;
        f.id    = uint8_t(idx);
        f.addr  = info.addr;
        f.burst = 1;  // INCR
        f.len   = 0;
        f.size  = info.size;
        f.cache = 0;
        f.qos   = info.qos;
        return f;
    }

    static axi::WFlit mkW(const Info& info) {  // slvData/slvMask（BaseCM:153-157）
        axi::WFlit f;
        for (auto& w : f.data) w = info.data;  // Fill(4, data64)
        f.strb = maskGen(info.addr, info.size, 5);  // busDataBytes=32
        f.last = true;
        return f;
    }
};

// ---------------- HiNodeAxiLiteBridge ----------------

class HiNodeAxiLiteBridge : public wolvicmod::Module {
public:
    static constexpr uint32_t kOutst = CmHiT::kOutst;
    using Cm    = BridgeCm<CmHiT>;
    using InfoV = typename Cm::InfoV;

    IN(bool, clk);
    IN(uint16_t, node_id);  // RTL nodeId（= 0x20）
    // ---- 环侧（DeviceIcnBundle HI：rx=弹出输入、tx=注入输出）----
    IN(Valid<RReqFlit>, rx_req);
    OUT(bool, rx_req_rdy);
    IN(Valid<RespFlit>, rx_resp);
    OUT(bool, rx_resp_rdy);
    IN(Valid<DataFlit>, rx_data);
    OUT(bool, rx_data_rdy);
    OUT(Valid<RespFlit>, tx_resp);
    IN(bool, tx_resp_rdy);
    OUT(Valid<DataFlit>, tx_data);
    IN(bool, tx_data_rdy);
    // ---- AXI master（cfgAXI）----
    OUT(Valid<axi::AWFlit>, axi_aw);
    IN(bool, axi_aw_rdy);
    OUT(Valid<axi::WFlit>, axi_w);
    IN(bool, axi_w_rdy);
    OUT(Valid<axi::ARFlit>, axi_ar);
    IN(bool, axi_ar_rdy);
    IN(Valid<axi::BFlit>, axi_b);
    OUT(bool, axi_b_rdy);
    IN(Valid<axi::RFlit>, axi_r);
    OUT(bool, axi_r_rdy);

    HiNodeAxiLiteBridge();

    // ---- testbench 白盒：cosim harness 看门狗/探针直达 ----
    using RspArbT = CondVipArb<RespFlit, kOutst>;  // 宏参数含逗号，先取别名
    using AxArbT  = CondVipArb<axi::AxFlit, kOutst>;
    using AwQ     = Queue<uint8_t, kOutst>;
    MOD(RspArbT, rsp_arb);
    MOD(AxArbT, aw_arb);
    MOD(AxArbT, ar_arb);
    MOD(AwQ, aw_q);  // UInt(8.W) 一位热
    MOD_ARRAY(Cm, kOutst, cms);

private:
    using RdPipe  = Queue<DataFlit, 1, false, true>;
    MOD(RdPipe, rd_pipe);  // readDataPipe

    using BoolArr = std::array<bool, kOutst>;
    using WkArr   = std::array<WkV, kOutst>;
    using InfoArr = std::array<InfoV, kOutst>;
    using RspArr  = std::array<Valid<RespFlit>, kOutst>;
    using AxArr   = std::array<Valid<axi::AxFlit>, kOutst>;
    using WArr    = std::array<Valid<axi::WFlit>, kOutst>;

    REG(BoolArr, tag_match);

    WIRE(WkArr, wk_all);
    WIRE(InfoArr, info_all);
    WIRE(RspArr, rsp_in);
    WIRE(AxArr, aw_in);
    WIRE(AxArr, ar_in);
    WIRE(WArr, w_all);
    WIRE(BoolArr, free_lo);
    WIRE(BoolArr, tag_match_c);
    WIRE(bool, w_any_free);
    WIRE(bool, w_req_fire);
    WIRE(uint32_t, w_wait_num);
    WIRE(bool, w_aw_out_fire);
    WIRE(bool, w_wsel_vld);
};

}  // namespace zj::bridge
