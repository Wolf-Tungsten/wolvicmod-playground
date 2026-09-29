#include "model/bridge/snode_axi_bridge.h"

#include <wolvicmod/wolvicmod.h>

namespace zj::bridge {

// 拍平建模（bridge_cm.h）：64 项 CM 状态并入 REG(cms) 一条 update 循环，
// per-entry 组合输出数组化为 Array wire 直喂仲裁器，per-entry fire 从仲裁器
// in_rdy 数组直读；仲裁器/队列/data_buf 子模块不变。

SNodeAxiBridge::SNodeAxiBridge() {
    using CmL = CmLogic<CmST>;
    rsp_arb.clk = clk;
    aw_arb.clk = clk;
    ar_arb.clk = clk;
    alloc_sel.clk = clk;
    alloc_q.clk = clk;
    aw_q.clk = clk;
    data_buf.clk = clk;
    rd_pipe.clk = clk;

    // ---- CM 阵列：per-entry 组合输出（原 combine(cms, port)）----
    wk_all.assign().reads(cms) = [](auto src) {
        auto [cms] = src;
        WkArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) o[i] = CmL::cmWakeupOut(cms[i]);
        return o;
    };
    info_all.assign().reads(cms) = [](auto src) {
        auto [cms] = src;
        InfoArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) o[i] = CmL::cmInfoOut(cms[i]);
        return o;
    };
    rsp_in.assign().reads(cms) = [](auto src) {
        auto [cms] = src;
        RspArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) o[i] = CmL::cmTxResp(cms[i], i);
        return o;
    };
    aw_in.assign().reads(cms) = [](auto src) {
        auto [cms] = src;
        AxArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) o[i] = CmL::cmAxiAw(cms[i], i);
        return o;
    };
    ar_in.assign().reads(cms) = [](auto src) {
        auto [cms] = src;
        AxArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) o[i] = CmL::cmAxiAr(cms[i], i);
        return o;
    };
    alloc_in.assign().reads(cms) = [](auto src) {
        auto [cms] = src;
        AllocArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) o[i] = CmL::cmAllocReq(cms[i], i);
        return o;
    };
    w_all.assign().reads(cms) = [](auto src) {
        auto [cms] = src;
        WArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) o[i] = CmL::cmAxiW(cms[i]);
        return o;
    };

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
    // per-entry fire = 出口 valid && 仲裁器 in_rdy[i]
    w_rsp_fire.assign().reads(rsp_in, rsp_arb.in_rdy) = [](auto src) {
        auto [rsp_in, rsp_arb_in_rdy] = src;
        BoolArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) o[i] = rsp_in[i].valid && rsp_arb_in_rdy[i];
        return o;
    };
    w_aw_fire.assign().reads(aw_in, aw_arb.in_rdy) = [](auto src) {
        auto [aw_in, aw_arb_in_rdy] = src;
        BoolArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) o[i] = aw_in[i].valid && aw_arb_in_rdy[i];
        return o;
    };
    w_ar_fire.assign().reads(ar_in, ar_arb.in_rdy) = [](auto src) {
        auto [ar_in, ar_arb_in_rdy] = src;
        BoolArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) o[i] = ar_in[i].valid && ar_arb_in_rdy[i];
        return o;
    };
    w_alloc_fire.assign().reads(alloc_in, alloc_sel.in_rdy) = [](auto src) {
        auto [alloc_in, alloc_sel_in_rdy] = src;
        BoolArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) o[i] = alloc_in[i].valid && alloc_sel_in_rdy[i];
        return o;
    };

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
    w_cm_req.assign().reads(rx_req, free_lo) = [](auto src) {
        auto [rx_req, free_lo] = src;
        ReqArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) {
            o[i].valid = rx_req.valid && free_lo[i];
            o[i].bits  = rx_req.bits;
        }
        return o;
    };

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
    w_cm_alloc_resp.assign().reads(alloc_q.deq, data_buf.alloc_rdy) = [](auto src) {
        auto [alloc_q_deq, data_buf_alloc_rdy] = src;
        BoolArr o{};
        for (uint32_t i = 0; i < kOutst; ++i)
            o[i] = alloc_q_deq.valid && data_buf_alloc_rdy &&
                   ((alloc_q_deq.bits.idx_oh >> i) & 1);
        return o;
    };

    // ---- awQueue：aw 授权序 → W 选择（AxiBridge.scala:79-90）----
    w_aw_out_fire.assign().reads(aw_arb.out, axi_aw_rdy) = [](auto src) {
        auto [aw_arb_out, axi_aw_rdy] = src;
        return aw_arb_out.valid && axi_aw_rdy;
    };
    aw_q.enq.assign().reads(aw_arb.chosen, w_aw_out_fire) = [](auto src) {
        auto [aw_arb_chosen, w_aw_out_fire] = src;
        Valid<uint64_t> d;
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
        Valid<uint64_t> d;
        d.valid = aw_q_deq.valid && w_wsel_vld;
        d.bits  = aw_q_deq.bits;
        return d;
    };
    aw_q.deq_rdy.assign().reads(data_buf.from_cm_rdy, w_wsel_vld) = [](auto src) {
        auto [data_buf_from_cm_rdy, w_wsel_vld] = src;
        return data_buf_from_cm_rdy && w_wsel_vld;
    };
    w_w_fire.assign().reads(w_all, data_buf.from_cm_rdy, aw_q.deq) = [](auto src) {
        auto [w_all, data_buf_from_cm_rdy, aw_q_deq] = src;
        BoolArr o{};
        for (uint32_t i = 0; i < kOutst; ++i)
            o[i] = w_all[i].valid && data_buf_from_cm_rdy && aw_q_deq.valid &&
                   ((aw_q_deq.bits >> i) & 1);
        return o;
    };

    // ---- axi.b / toCmDat / 环侧写数据分发 ----
    axi_b_rdy = true;
    data_buf.icn = rx_data;
    rx_data_rdy = data_buf.icn_rdy;
    axi_w = data_buf.axi_w;
    data_buf.axi_w_rdy = axi_w_rdy;
    w_cm_b.assign().reads(axi_b) = [](auto src) {
        auto [axi_b] = src;
        BArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) {
            o[i].valid = axi_b.valid && axi_b.bits.id == i;
            o[i].bits  = axi_b.bits;
        }
        return o;
    };
    w_cm_rx_data.assign().reads(data_buf.to_cm) = [](auto src) {
        auto [data_buf_to_cm] = src;
        DatArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) {
            o[i].valid = data_buf_to_cm.valid &&
                         (data_buf_to_cm.bits.txn_id & (kOutst - 1)) == i;
            o[i].bits  = data_buf_to_cm.bits;
        }
        return o;
    };

    // ---- axi.r → readDataPipe → tx_data（AxiBridge.scala:119-148）----
    rd_pipe.enq.assign().reads(axi_r, info_all) = [](auto src) {
        auto [axi_r, info_all] = src;
        const auto& cs = info_all[axi_r.bits.id & (kOutst - 1)].info;  // ctrlSel
        Valid<DataFlit> d;
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
    w_cm_rd_fire.assign().reads(axi_r, rd_pipe.enq_rdy) = [](auto src) {
        auto [axi_r, rd_pipe_enq_rdy] = src;
        BoolArr o{};
        for (uint32_t i = 0; i < kOutst; ++i)
            o[i] = axi_r.valid && rd_pipe_enq_rdy && (axi_r.bits.id & (kOutst - 1)) == i;
        return o;
    };
    w_cm_rd_last.assign().reads(axi_r) = [](auto src) {
        auto [axi_r] = src;
        BoolArr o{};
        for (uint32_t i = 0; i < kOutst; ++i) o[i] = axi_r.bits.last;
        return o;
    };

    // ---- CM 状态阵列：一条 update 循环算全数组 next（一切判定读旧值）----
    // S 无 rx.resp（tie invalid）
    // 静止门（§23 续）：候选 = ∃有效项 || ∃wk_vld_reg/wait_set_en 未清 || 本拍
    // 新入队。旁路排查：cmNext 的 payload 事件累积全部在 st.valid 分支内，
    // wk_all 广播命中也以本项 valid 为前提（无 comp_ack_hit 类旁路）；
    // wk_vld_reg=RegNext(cmWakeupVld) 自清——项完成（valid 撤）与被打拍唤醒
    // 同拍时 valid 已非而 wk_vld_reg 仍置位，waiting 照常 -1，漏掉会丢这次
    // 递减（validD1 同款）；wait_set_en=RegNext(reqFire) 同理自清（置位时项
    // 必 valid，列入作保守冗余）。
    w_cms_any.assign().reads(cms, w_cm_req) = [](auto src) {
        auto [cms, cm_req] = src;
        for (uint32_t i = 0; i < kOutst; ++i)
            if (cms[i].valid || cms[i].wk_vld_reg || cms[i].wait_set_en || cm_req[i].valid)
                return true;
        return false;
    };
    cms.update().on(posedge(clk)).en(w_cms_any)
        .reads(cms, w_cm_req, w_cm_rx_data, w_cm_b, w_cm_rd_fire, w_cm_rd_last,
               w_wait_num, wk_all, w_rsp_fire, w_aw_fire, w_ar_fire, w_w_fire,
               w_alloc_fire, w_cm_alloc_resp) = [](auto src) {
            auto [cms, cm_req, cm_rx_data, cm_b, cm_rd_fire, cm_rd_last, wait_num,
                  wk_all, rsp_fire, aw_fire, ar_fire, w_fire, alloc_fire,
                  alloc_resp] = src;
            CmStArr n = cms;
            for (uint32_t i = 0; i < kOutst; ++i)
                n[i] = CmL::cmNext(cms[i], i, cm_req[i].valid, cm_req[i].bits,
                                   Valid<RespFlit>{}, cm_rx_data[i], cm_b[i],
                                   cm_rd_fire[i], cm_rd_last[i], wait_num, wk_all,
                                   rsp_fire[i], aw_fire[i], ar_fire[i], w_fire[i],
                                   alloc_fire[i], alloc_resp[i]);
            return n;
        };
}

}  // namespace zj::bridge
