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
#include "wolvicmod/prefab/dec.h"
#include "wolvicmod/prefab/queue.h"

namespace zj::bridge {

using namespace zj::chi;
using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Dec;
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
    IN(Dec<RReqFlit>, rx_req);
    OUT(bool, rx_req_rdy);
    IN(Dec<RespFlit>, rx_resp);
    OUT(bool, rx_resp_rdy);
    IN(Dec<DataFlit>, rx_data);
    OUT(bool, rx_data_rdy);
    OUT(Dec<RespFlit>, tx_resp);
    IN(bool, tx_resp_rdy);
    OUT(Dec<DataFlit>, tx_data);
    IN(bool, tx_data_rdy);
    // ---- AXI master（cfgAXI）----
    OUT(Dec<axi::AWFlit>, axi_aw);
    IN(bool, axi_aw_rdy);
    OUT(Dec<axi::WFlit>, axi_w);
    IN(bool, axi_w_rdy);
    OUT(Dec<axi::ARFlit>, axi_ar);
    IN(bool, axi_ar_rdy);
    IN(Dec<axi::BFlit>, axi_b);
    OUT(bool, axi_b_rdy);
    IN(Dec<axi::RFlit>, axi_r);
    OUT(bool, axi_r_rdy);

    using RspArbT = CondVipArb<RespFlit, kOutst>;  // 宏参数含逗号，先取别名
    using AxArbT  = CondVipArb<axi::AxFlit, kOutst>;
    using AwQ     = Queue<uint8_t, kOutst>;
    using RdPipe  = Queue<DataFlit, 1, false, true>;
    MOD(RspArbT, rsp_arb);
    MOD(AxArbT, aw_arb);
    MOD(AxArbT, ar_arb);
    MOD(AwQ, aw_q);  // UInt(8.W) 一位热
    MOD(RdPipe, rd_pipe);  // readDataPipe

    // 成员持有（而非构造函数局部）：cosim harness 看门狗需构造后访问各 CM 内部态
    MOD_ARRAY(Cm, kOutst, cms);

    using BoolArr = std::array<bool, kOutst>;
    using WkArr   = std::array<WkV, kOutst>;
    using InfoArr = std::array<InfoV, kOutst>;
    using RspArr  = std::array<Dec<RespFlit>, kOutst>;
    using AxArr   = std::array<Dec<axi::AxFlit>, kOutst>;
    using WArr    = std::array<Dec<axi::WFlit>, kOutst>;

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

    HiNodeAxiLiteBridge() {
        rsp_arb.clk = clk;
        aw_arb.clk = clk;
        ar_arb.clk = clk;
        aw_q.clk = clk;
        rd_pipe.clk = clk;
        for (uint32_t i = 0; i < kOutst; ++i) {
            Cm& cm                = cms[i];
            cm.clk                = clk;
            cm.idx                = i;
            // HI 无 alloc：req 已在 CM 内 tie invalid；rdy/resp 按 §4.2 须有驱动
            cm.alloc_req_rdy      = false;
            cm.alloc_resp         = false;
        }

        wolvicmod::collectPorts(wk_all, cms, [](Cm& cm) -> Out<WkV>& { return cm.wakeup_out; });
        wolvicmod::collectPorts(info_all, cms, [](Cm& cm) -> Out<InfoV>& { return cm.info_out; });
        wolvicmod::collectPorts(rsp_in, cms, [](Cm& cm) -> Out<Dec<RespFlit>>& { return cm.tx_resp; });
        wolvicmod::collectPorts(aw_in, cms, [](Cm& cm) -> Out<Dec<axi::AxFlit>>& { return cm.axi_aw; });
        wolvicmod::collectPorts(ar_in, cms, [](Cm& cm) -> Out<Dec<axi::AxFlit>>& { return cm.axi_ar; });
        wolvicmod::collectPorts(w_all, cms,
                             [](Cm& cm) -> Out<Dec<axi::WFlit>>& { return cm.axi_w; });

        rsp_arb.in = rsp_in;
        aw_arb.in = aw_in;
        ar_arb.in = ar_in;
        tx_resp = rsp_arb.out;
        rsp_arb.out_rdy = tx_resp_rdy;
        axi_aw = aw_arb.out;
        aw_arb.out_rdy = axi_aw_rdy;
        axi_ar = ar_arb.out;
        ar_arb.out_rdy = axi_ar_rdy;
        for (uint32_t i = 0; i < kOutst; ++i) {
            Cm& cm = cms[i];
            cm.wk_in = wk_all;
            cm.wait_num = w_wait_num;
            cm.tx_resp_rdy.assign().reads(rsp_arb.in_rdy) = [i](auto src) {
                auto [rsp_arb_in_rdy] = src;
                return rsp_arb_in_rdy[i];
            };
            cm.axi_aw_rdy.assign().reads(aw_arb.in_rdy) = [i](auto src) {
                auto [aw_arb_in_rdy] = src;
                return aw_arb_in_rdy[i];
            };
            cm.axi_ar_rdy.assign().reads(ar_arb.in_rdy) = [i](auto src) {
                auto [ar_arb_in_rdy] = src;
                return ar_arb_in_rdy[i];
            };
        }

        // ---- PickOneLow 入队 ----
        free_lo.assign().reads(info_all) = [](auto src) {
            auto [info_all] = src;
            std::array<bool, kOutst> oh{};
            for (uint32_t i = 0; i < kOutst; ++i)
                if (!info_all[i].valid) {
                    oh[i] = true;
                    break;
                }
            return oh;
        };
        w_any_free.assign().reads(info_all) = [](auto src) {
            auto [info_all] = src;
            for (uint32_t i = 0; i < kOutst; ++i)
                if (!info_all[i].valid) return true;
            return false;
        };
        rx_req_rdy = w_any_free;
        w_req_fire.assign().reads(rx_req, w_any_free) = [](auto src) {
            auto [rx_req, w_any_free] = src;
            return rx_req.valid && w_any_free;
        };
        for (uint32_t i = 0; i < kOutst; ++i) {
            Cm& cm = cms[i];
            cm.rx_req.assign().reads(rx_req, free_lo) = [i](auto src) {
                auto [rx_req, free_lo] = src;
                Dec<RReqFlit> d;
                d.valid = rx_req.valid && free_lo[i];
                d.bits  = rx_req.bits;
                return d;
            };
        }

        // ---- 同 tag 保序统计 ----
        tag_match_c.assign().reads(info_all, wk_all, rx_req) = [](auto src) {
            auto [info_all, wk_all, rx_req] = src;
            std::array<bool, kOutst> m{};
            for (uint32_t i = 0; i < kOutst; ++i)
                m[i] = info_all[i].valid && !wk_all[i].valid &&
                       info_all[i].info.isSnooped &&
                       CmHiT::tagMatch(info_all[i].info.addr, rx_req.bits.addr);
            return m;
        };
        tag_match.update().on(posedge(clk)).en(w_req_fire).reads(tag_match_c) = [](auto src) {
            auto [tag_match_c] = src;
            return tag_match_c;
        };
        w_wait_num.assign().reads(tag_match) = [](auto src) -> uint32_t {
            auto [tag_match] = src;
            uint32_t n = 0;
            for (uint32_t i = 0; i < kOutst; ++i) n += tag_match[i] ? 1 : 0;
            return n;
        };

        // ---- awQueue：aw 授权序 → W 直出（AxiLiteBridge.scala:69-80）----
        w_aw_out_fire.assign().reads(aw_arb.out, axi_aw_rdy) = [](auto src) {
            auto [aw_arb_out, axi_aw_rdy] = src;
            return aw_arb_out.valid && axi_aw_rdy;
        };
        aw_q.enq.assign().reads(aw_arb.chosen, w_aw_out_fire) = [](auto src) {
            auto [aw_arb_chosen, w_aw_out_fire] = src;
            Dec<uint8_t> d;
            d.valid = w_aw_out_fire;
            d.bits  = uint8_t(uint8_t{1} << aw_arb_chosen);
            return d;
        };
        w_wsel_vld.assign().reads(aw_q.deq, w_all) = [](auto src) {
            auto [aw_q_deq, w_all] = src;
            for (uint32_t i = 0; i < kOutst; ++i)
                if (((aw_q_deq.bits >> i) & 1) && w_all[i].valid) return true;
            return false;
        };
        axi_w.assign().reads(aw_q.deq, w_all, w_wsel_vld) = [](auto src) {
            auto [aw_q_deq, w_all, w_wsel_vld] = src;
            Dec<axi::WFlit> d;
            d.valid = aw_q_deq.valid && w_wsel_vld;
            for (uint32_t i = 0; i < kOutst; ++i)
                if ((aw_q_deq.bits >> i) & 1) d.bits = w_all[i].bits;  // Mux1H
            return d;
        };
        aw_q.deq_rdy.assign().reads(axi_w_rdy, w_wsel_vld) = [](auto src) {
            auto [axi_w_rdy, w_wsel_vld] = src;
            return axi_w_rdy && w_wsel_vld;
        };
        for (uint32_t i = 0; i < kOutst; ++i) {
            Cm& cm = cms[i];
            cm.axi_w_rdy.assign().reads(axi_w_rdy, aw_q.deq) = [i](auto src) {
                auto [axi_w_rdy, aw_q_deq] = src;
                return axi_w_rdy && aw_q_deq.valid && ((aw_q_deq.bits >> i) & 1);
            };
        }

        // ---- rx.resp / rx.data 按 TxnID 分发（恒 ready）；axi.b 按 id 分发 ----
        rx_resp_rdy = true;
        rx_data_rdy = true;
        axi_b_rdy = true;
        for (uint32_t i = 0; i < kOutst; ++i) {
            Cm& cm = cms[i];
            cm.rx_resp.assign().reads(rx_resp) = [i](auto src) {
                auto [rx_resp] = src;
                Dec<RespFlit> d;
                d.valid = rx_resp.valid && (rx_resp.bits.txn_id & (kOutst - 1)) == i;
                d.bits  = rx_resp.bits;
                return d;
            };
            cm.rx_data.assign().reads(rx_data) = [i](auto src) {
                auto [rx_data] = src;
                Dec<DataFlit> d;
                d.valid = rx_data.valid && (rx_data.bits.txn_id & (kOutst - 1)) == i;
                d.bits  = rx_data.bits;
                return d;
            };
            cm.axi_b.assign().reads(axi_b) = [i](auto src) {
                auto [axi_b] = src;
                Dec<axi::BFlit> d;
                d.valid = axi_b.valid && (axi_b.bits.id & (kOutst - 1)) == i;
                d.bits  = axi_b.bits;
                return d;
            };
        }

        // ---- axi.r → readDataPipe → tx_data（AxiLiteBridge.scala:112-141）----
        rd_pipe.enq.assign().reads(axi_r, info_all, node_id) = [](auto src) {
            auto [axi_r, info_all, node_id] = src;
            const auto& cs = info_all[axi_r.bits.id & (kOutst - 1)].info;  // ctrlSel
            Dec<DataFlit> d;
            d.valid           = axi_r.valid;
            d.bits            = DataFlit{};
            d.bits.data       = axi_r.bits.data;  // Fill(dw/busDataBits=1, data)
            d.bits.opcode     = dat_op::kCompData;
            d.bits.data_id    = uint8_t(((cs.addr >> 5) & 1) << 1);
            d.bits.txn_id     = cs.txn_id;
            d.bits.src_id     = 0;
            d.bits.dbid       = axi_r.bits.id & (kOutst - 1);
            d.bits.home_nid   = node_id;
            d.bits.tgt_id     = cs.src_id;
            d.bits.resp       = 0x2;
            d.bits.resp_err   = axi_r.bits.resp & 0x3;
            d.bits.qos        = cs.qos;
            d.bits.be         = maskGen(cs.addr, cs.size, 5);
            return d;
        };
        axi_r_rdy = rd_pipe.enq_rdy;
        tx_data = rd_pipe.deq;
        rd_pipe.deq_rdy = tx_data_rdy;
        for (uint32_t i = 0; i < kOutst; ++i) {
            Cm& cm = cms[i];
            cm.rd_fire.assign().reads(axi_r, rd_pipe.enq_rdy) = [i](auto src) {
                auto [axi_r, rd_pipe_enq_rdy] = src;
                return axi_r.valid && rd_pipe_enq_rdy && (axi_r.bits.id & (kOutst - 1)) == i;
            };
            cm.rd_last.assign().reads(axi_r) = [i](auto src) {
                auto [axi_r] = src;
                return axi_r.bits.last;
            };
        }
    }
};

}  // namespace zj::bridge
