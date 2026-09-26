#include "model/bridge/hinode_axilite_bridge.h"

#include <wolvicmod/wolvicmod.h>

namespace zj::bridge {

HiNodeAxiLiteBridge::HiNodeAxiLiteBridge() {
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

    wolvicmod::combine(wk_all, cms, [](Cm& cm) -> Out<WkV>& { return cm.wakeup_out; });
    wolvicmod::combine(info_all, cms, [](Cm& cm) -> Out<InfoV>& { return cm.info_out; });
    wolvicmod::combine(rsp_in, cms, [](Cm& cm) -> Out<Dec<RespFlit>>& { return cm.tx_resp; });
    wolvicmod::combine(aw_in, cms, [](Cm& cm) -> Out<Dec<axi::AxFlit>>& { return cm.axi_aw; });
    wolvicmod::combine(ar_in, cms, [](Cm& cm) -> Out<Dec<axi::AxFlit>>& { return cm.axi_ar; });
    wolvicmod::combine(w_all, cms,
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

}  // namespace zj::bridge
