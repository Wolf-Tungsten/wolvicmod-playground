#pragma once

// SNodeAxiBridge：S 节点（gid6，mem_0）的 CHI-SN→AXI4 桥，对齐
// zhujiang/device/bridge/axi/AxiBridge.scala。kunminghu-v3：
// outstanding=64（ZhuJiangNoCTopology.scala MemoryOutstanding）、
// AXI id 6b / addr 48b / data 256b、compareTag=addr[37:6]（32KB 粒度保序）。
//
// 结构（AxiBridge.scala:35-148）：
//   rx_req（HReqFlit，ERQ）→ PickOneLow 选最低空闲 CM 入队；
//   64×BridgeCm<CmST>：tx_resp 经 ConditionVipArbiter(qos>=) 合流；
//   aw/ar 各经 ConditionVipArbiter 合流；awQueue(64) 记录 aw 授权序 →
//   W 数据按序从 AxiDataBuffer 读出（AXI3 无 WID，W 必须按 AW 序）；
//   alloc 经 DataBufferAllocReqSelector（= CondVipArb + Queue(2)，
//   AllocSelector.scala）入 AxiDataBuffer；
//   axi.r → readDataPipe(Queue(1,pipe)) 组 CompData → tx_data；
//   axi.b 恒 ready 按 id 分发；同 tag 保序 waitNum 桥顶统计广播。
// ZJPerf/MbistPipeline/working（时钟门控）不建模（§3.4 约定）。
// 断言（opvec 合法性、awQueue 不满、wakeup 唯一、timer 不超时）转注释。

#include <array>
#include <cstdint>
#include <string>

#include "model/bridge/axi_data_buffer.h"
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

// ---------------- CmST：S 桥 CM traits ----------------

struct CmST {
    static constexpr bool     kSn      = true;
    static constexpr uint32_t kOutst   = 64;
    static constexpr uint32_t kAllOnes = 63;  // Fill(6, 1)
    using ReqT = HReqFlit;

    // AxiCtrlInfo（mem=true；ioDataBits=0/withData=false）
    struct Info {
        uint64_t addr       = 0;  // [47:0]
        uint8_t  size       = 0;  // [2:0]
        uint16_t txn_id     = 0;  // [11:0]
        uint16_t src_id     = 0;  // [10:0]
        uint16_t return_nid = 0;  // [10:0]
        uint16_t return_txn = 0;  // [11:0]
        bool     dwt        = false;
        bool     ewa        = false;
        bool     device     = false;
        uint8_t  readCnt    = 0;
        uint8_t  qos        = 0;  // [3:0]
        bool     isSnooped  = false;

        bool operator==(const Info&) const = default;
    };

    static bool tagMatch(uint64_t a, uint64_t b) {  // addr[37:6]
        return ((a >> 6) & 0xFFFFFFFFULL) == ((b >> 6) & 0xFFFFFFFFULL);
    }
    static bool uCompleted(const UState& u) {
        return u.receiptResp && u.dbidResp && u.wdata && u.rdata && u.comp;
    }
    static bool     dwtOf(const Info& i) { return i.dwt; }
    static uint16_t returnNidOf(const Info& i) { return i.return_nid; }
    static uint16_t returnTxnIdOf(const Info& i) { return i.return_txn; }

    static void enqEntry(const ReqT& req, UState& u, DState& d, Info& info,
                         bool& bufferAllocated) {
        // IcnIoDevRsEntryCommon.enq + AxiRsEntry.enq
        info.addr       = req.addr;
        info.size       = req.size;
        info.txn_id     = req.txn_id;
        info.src_id     = req.src_id;
        info.return_nid = req.return_nid;
        info.return_txn = req.return_txn_id;
        info.dwt        = req.opcode != req_op::kReadNoSnp && req.snp_attr;  // DoDWT
        info.ewa        = req.mem_attr & 1;
        info.device     = (req.mem_attr >> 1) & 1;
        info.readCnt    = 0;
        info.isSnooped  = true;
        info.qos        = req.qos;
        decodeU(req, u);
        decodeD(req, d);
        bufferAllocated = req.opcode == req_op::kReadNoSnp;
    }

    static void decodeU(const ReqT& req, UState& u) {
        // ChiUpstreamOpVec.decode（合法 opcode：ReadNoSnp/WriteNoSnpPtl/
        // WriteNoSnpFull/WriteNoSnpFullCleanInv，Size<=6 —— RTL 断言）
        if (req.opcode == req_op::kReadNoSnp) {
            u.receiptResp = req.order == 0;
            u.dbidResp = true; u.wdata = true; u.rdata = false; u.comp = true;
        } else {
            u.receiptResp = true; u.dbidResp = false; u.wdata = false;
            u.rdata = true; u.comp = false;
        }
        u.compAck = !req.exp_comp_ack;  // S 不计入 completed
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

    static axi::AxFlit mkAx(const Info& info, uint32_t idx) {
        axi::AxFlit f;
        f.id    = uint8_t(idx);
        f.addr  = info.addr;
        f.burst = 1;  // INCR
        f.size  = info.size > 5 ? 5 : info.size;  // busSize=5（256b）
        f.len   = info.size > 5 ? uint8_t((1u << (info.size - 5)) - 1) : 0;
        f.cache = uint8_t((!info.device) << 1 | info.ewa);  // Cat(0,0,!device,ewa)
        f.qos   = info.qos;
        return f;
    }

    static axi::WFlit mkW(const Info&) { return {}; }  // DontCare（内容不消费）
};

// ---------------- SNodeAxiBridge ----------------

class SNodeAxiBridge : public wolvicmod::Module {
public:
    static constexpr uint32_t kOutst = CmST::kOutst;
    using Cm    = BridgeCm<CmST>;
    using InfoV = typename Cm::InfoV;

    IN(bool, clk);
    // ---- 环侧（DeviceIcnBundle S：rx=弹出输入、tx=注入输出）----
    IN(Dec<HReqFlit>, rx_req);
    OUT(bool, rx_req_rdy);
    IN(Dec<DataFlit>, rx_data);
    OUT(bool, rx_data_rdy);
    OUT(Dec<RespFlit>, tx_resp);
    IN(bool, tx_resp_rdy);
    OUT(Dec<DataFlit>, tx_data);
    IN(bool, tx_data_rdy);
    // ---- AXI master（memAXI）----
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

    using RspArbT  = CondVipArb<RespFlit, kOutst>;      // 宏参数含逗号，先取别名
    using AxArbT   = CondVipArb<axi::AxFlit, kOutst>;
    using AllocArbT = CondVipArb<AllocReqBits, kOutst>;
    using AllocQ   = Queue<AllocReqBits, 2>;
    using AwQ      = Queue<uint64_t, kOutst>;
    using RdPipe   = Queue<DataFlit, 1, false, true>;
    MOD(RspArbT, rsp_arb);
    MOD(AxArbT, aw_arb);
    MOD(AxArbT, ar_arb);
    MOD(AllocArbT, alloc_sel);
    MOD(AllocQ, alloc_q);
    MOD(AwQ, aw_q);  // UInt(64.W) 一位热
    MOD(AxiDataBuffer, data_buf);
    MOD(RdPipe, rd_pipe);  // readDataPipe

    // 成员持有（而非构造函数局部）：cosim harness 看门狗需构造后访问各 CM 内部态
    MOD_ARRAY(Cm, kOutst, cms);

    using BoolArr = std::array<bool, kOutst>;
    using WkArr   = std::array<WkV, kOutst>;
    using InfoArr = std::array<InfoV, kOutst>;
    using RspArr  = std::array<Dec<RespFlit>, kOutst>;
    using AxArr   = std::array<Dec<axi::AxFlit>, kOutst>;
    using AllocArr = std::array<Dec<AllocReqBits>, kOutst>;
    using WArr    = std::array<Dec<axi::WFlit>, kOutst>;

    REG(BoolArr, tag_match);  // reqTagMatchVecReg

    WIRE(WkArr, wk_all);
    WIRE(InfoArr, info_all);
    WIRE(RspArr, rsp_in);
    WIRE(AxArr, aw_in);
    WIRE(AxArr, ar_in);
    WIRE(AllocArr, alloc_in);
    WIRE(WArr, w_all);
    WIRE(BoolArr, free_lo);       // PickOneLow 一位热
    WIRE(BoolArr, tag_match_c);   // reqTagMatchVec（组合）
    WIRE(bool, w_any_free);
    WIRE(bool, w_req_fire);
    WIRE(uint32_t, w_wait_num);
    WIRE(bool, w_aw_out_fire);
    WIRE(bool, w_wsel_vld);

    SNodeAxiBridge() {
        rsp_arb.clk = clk;
        aw_arb.clk = clk;
        ar_arb.clk = clk;
        alloc_sel.clk = clk;
        alloc_q.clk = clk;
        aw_q.clk = clk;
        data_buf.clk = clk;
        rd_pipe.clk = clk;
        for (uint32_t i = 0; i < kOutst; ++i) {
            Cm& cm     = cms[i];
            cm.clk     = clk;
            cm.idx     = i;
            cm.rx_resp = Dec<RespFlit>{};  // S 无 rx.resp（tie invalid）
        }

        // ---- 汇聚 ----
        wolvicmod::combine(wk_all, cms, [](Cm& cm) -> Out<WkV>& { return cm.wakeup_out; });
        wolvicmod::combine(info_all, cms, [](Cm& cm) -> Out<InfoV>& { return cm.info_out; });
        wolvicmod::combine(rsp_in, cms, [](Cm& cm) -> Out<Dec<RespFlit>>& { return cm.tx_resp; });
        wolvicmod::combine(aw_in, cms, [](Cm& cm) -> Out<Dec<axi::AxFlit>>& { return cm.axi_aw; });
        wolvicmod::combine(ar_in, cms, [](Cm& cm) -> Out<Dec<axi::AxFlit>>& { return cm.axi_ar; });
        wolvicmod::combine(alloc_in, cms,
                             [](Cm& cm) -> Out<Dec<AllocReqBits>>& { return cm.alloc_req; });
        wolvicmod::combine(w_all, cms,
                             [](Cm& cm) -> Out<Dec<axi::WFlit>>& { return cm.axi_w; });

        // ---- 仲裁合流 ----
        rsp_arb.in = rsp_in;
        aw_arb.in = aw_in;
        ar_arb.in = ar_in;
        alloc_sel.in = alloc_in;
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
            cm.alloc_req_rdy.assign().reads(alloc_sel.in_rdy) = [i](auto src) {
                auto [alloc_sel_in_rdy] = src;
                return alloc_sel_in_rdy[i];
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
                Dec<HReqFlit> d;
                d.valid = rx_req.valid && free_lo[i];
                d.bits  = rx_req.bits;
                return d;
            };
        }

        // ---- 同 tag 保序统计（AxiBridge.scala:92-98）----
        tag_match_c.assign().reads(info_all, wk_all, rx_req) = [](auto src) {
            auto [info_all, wk_all, rx_req] = src;
            std::array<bool, kOutst> m{};
            for (uint32_t i = 0; i < kOutst; ++i)
                m[i] = info_all[i].valid && !wk_all[i].valid &&
                       info_all[i].info.isSnooped &&
                       CmST::tagMatch(info_all[i].info.addr, rx_req.bits.addr);
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

        // ---- allocSel → Queue(2) → dataBuffer；resp 按 idxOH 分发 ----
        alloc_q.enq = alloc_sel.out;
        alloc_sel.out_rdy = alloc_q.enq_rdy;
        data_buf.alloc = alloc_q.deq;
        alloc_q.deq_rdy = data_buf.alloc_rdy;
        for (uint32_t i = 0; i < kOutst; ++i) {
            Cm& cm = cms[i];
            cm.alloc_resp.assign().reads(alloc_q.deq, data_buf.alloc_rdy) =
                [i](auto src) {
                    auto [alloc_q_deq, data_buf_alloc_rdy] = src;
                    return alloc_q_deq.valid && data_buf_alloc_rdy &&
                           ((alloc_q_deq.bits.idx_oh >> i) & 1);
                };
        }

        // ---- awQueue：aw 授权序 → W 选择（AxiBridge.scala:79-90）----
        w_aw_out_fire.assign().reads(aw_arb.out, axi_aw_rdy) = [](auto src) {
            auto [aw_arb_out, axi_aw_rdy] = src;
            return aw_arb_out.valid && axi_aw_rdy;
        };
        aw_q.enq.assign().reads(aw_arb.chosen, w_aw_out_fire) = [](auto src) {
            auto [aw_arb_chosen, w_aw_out_fire] = src;
            Dec<uint64_t> d;
            d.valid = w_aw_out_fire;  // RTL 断言 awQueue 不满（每 CM 至多一笔 AW）
            d.bits  = uint64_t{1} << aw_arb_chosen;
            return d;
        };
        w_wsel_vld.assign().reads(aw_q.deq, w_all) = [](auto src) {
            auto [aw_q_deq, w_all] = src;
            for (uint32_t i = 0; i < kOutst; ++i)
                if (((aw_q_deq.bits >> i) & 1) && w_all[i].valid) return true;
            return false;
        };
        data_buf.from_cm.assign().reads(aw_q.deq, w_wsel_vld) = [](auto src) {
            auto [aw_q_deq, w_wsel_vld] = src;
            Dec<uint64_t> d;
            d.valid = aw_q_deq.valid && w_wsel_vld;
            d.bits  = aw_q_deq.bits;
            return d;
        };
        aw_q.deq_rdy.assign().reads(data_buf.from_cm_rdy, w_wsel_vld) = [](auto src) {
            auto [data_buf_from_cm_rdy, w_wsel_vld] = src;
            return data_buf_from_cm_rdy && w_wsel_vld;
        };
        for (uint32_t i = 0; i < kOutst; ++i) {
            Cm& cm = cms[i];
            cm.axi_w_rdy.assign().reads(data_buf.from_cm_rdy, aw_q.deq) = [i](auto src) {
                auto [data_buf_from_cm_rdy, aw_q_deq] = src;
                return data_buf_from_cm_rdy && aw_q_deq.valid &&
                       ((aw_q_deq.bits >> i) & 1);
            };
        }

        // ---- axi.b / toCmDat / 环侧写数据分发 ----
        axi_b_rdy = true;
        data_buf.icn = rx_data;
        rx_data_rdy = data_buf.icn_rdy;
        axi_w = data_buf.axi_w;
        data_buf.axi_w_rdy = axi_w_rdy;
        for (uint32_t i = 0; i < kOutst; ++i) {
            Cm& cm = cms[i];
            cm.axi_b.assign().reads(axi_b) = [i](auto src) {
                auto [axi_b] = src;
                Dec<axi::BFlit> d;
                d.valid = axi_b.valid && axi_b.bits.id == i;
                d.bits  = axi_b.bits;
                return d;
            };
            cm.rx_data.assign().reads(data_buf.to_cm) = [i](auto src) {
                auto [data_buf_to_cm] = src;
                Dec<DataFlit> d;
                d.valid = data_buf_to_cm.valid &&
                          (data_buf_to_cm.bits.txn_id & (kOutst - 1)) == i;
                d.bits  = data_buf_to_cm.bits;
                return d;
            };
        }

        // ---- axi.r → readDataPipe → tx_data（AxiBridge.scala:119-148）----
        rd_pipe.enq.assign().reads(axi_r, info_all) = [](auto src) {
            auto [axi_r, info_all] = src;
            const auto& cs = info_all[axi_r.bits.id & (kOutst - 1)].info;  // ctrlSel
            Dec<DataFlit> d;
            d.valid           = axi_r.valid;
            d.bits            = DataFlit{};
            d.bits.data       = axi_r.bits.data;
            d.bits.opcode     = dat_op::kCompData;
            d.bits.data_id    = uint8_t(((cs.addr >> 5) & 1) << 1) + uint8_t(cs.readCnt << 1);
            d.bits.txn_id     = cs.return_txn;
            d.bits.src_id     = 0;
            d.bits.tgt_id     = cs.return_nid;
            d.bits.home_nid   = cs.src_id;
            d.bits.dbid       = cs.txn_id;
            d.bits.resp       = 0x2;
            d.bits.resp_err   = axi_r.bits.resp & 0x3;
            d.bits.qos        = cs.qos;
            d.bits.be         = cs.size == 6 ? 0xFFFFFFFFULL : maskGen(cs.addr, cs.size, 5);
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
