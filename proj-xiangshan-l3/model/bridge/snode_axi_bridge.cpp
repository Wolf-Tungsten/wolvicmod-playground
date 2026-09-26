#include "model/bridge/snode_axi_bridge.h"

#include <wolvicmod/wolvicmod.h>

namespace zj::bridge {

SNodeAxiBridge::SNodeAxiBridge() {
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

}  // namespace zj::bridge
