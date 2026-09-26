#include "model/home/hnf_stub.h"

#include <wolvicmod/wolvicmod.h>

namespace zj::home {

HnfStub::HnfStub() {
    // 消费侧：req 入池（满反压）；resp/data 直接丢弃
    hnx_rx_req_rdy.assign().reads(st) = [](auto src) {
        auto [st] = src;
        return st.count < kMax;
    };
    hnx_rx_resp_rdy = true;
    hnx_rx_data_rdy = true;

    // 产出侧：队头到期 → 先 RSP(Comp) 后 DAT(CompData)
    w_head_due.assign().reads(st) = [](auto src) {
        auto [st] = src;
        return st.count > 0 && st.entries[0].due <= st.cycle;
    };
    hnx_tx_resp.assign().reads(st, w_head_due) = [](auto src) {
        auto [st, w_head_due] = src;
        Valid<RespFlit> d;
        d.valid = w_head_due && !st.entries[0].sent_rsp;
        d.bits  = RespFlit{};
        if (d.valid) {
            d.bits.opcode = kOpComp;
            d.bits.tgt_id = st.entries[0].tgt_id;
            d.bits.txn_id = st.entries[0].txn_id;
        }
        return d;
    };
    hnx_tx_data.assign().reads(st, w_head_due) = [](auto src) {
        auto [st, w_head_due] = src;
        Valid<DataFlit> d;
        d.valid = w_head_due && st.entries[0].sent_rsp;
        d.bits  = DataFlit{};
        if (d.valid) {
            d.bits.opcode = kOpCompData;
            d.bits.tgt_id = st.entries[0].tgt_id;
            d.bits.txn_id = st.entries[0].txn_id;
        }
        return d;
    };
    hnx_tx_snoop = Valid<SnoopFlit>{};
    hnx_tx_erq   = Valid<HReqFlit>{};

    w_rsp_fire.assign().reads(hnx_tx_resp, hnx_tx_resp_rdy) = [](auto src) {
        auto [hnx_tx_resp, hnx_tx_resp_rdy] = src;
        return hnx_tx_resp.valid && hnx_tx_resp_rdy;
    };
    w_dat_fire.assign().reads(hnx_tx_data, hnx_tx_data_rdy) = [](auto src) {
        auto [hnx_tx_data, hnx_tx_data_rdy] = src;
        return hnx_tx_data.valid && hnx_tx_data_rdy;
    };
    w_req_fire.assign().reads(hnx_rx_req, hnx_rx_req_rdy) = [](auto src) {
        auto [hnx_rx_req, hnx_rx_req_rdy] = src;
        return hnx_rx_req.valid && hnx_rx_req_rdy;
    };

    // 状态阵列纪律：单条 Update 漏斗
    st.update().on(posedge(clk)).reads(st, hnx_rx_req, w_req_fire, w_rsp_fire, w_dat_fire) =
        [](auto src) {
            auto [st, hnx_rx_req, w_req_fire, w_rsp_fire, w_dat_fire] = src;
            St next = st;
            next.cycle = st.cycle + 1;
            if (w_req_fire) {
                Entry e;
                e.valid  = true;
                e.due    = st.cycle + kLatency;
                e.tgt_id = hnx_rx_req.bits.src_id;
                e.txn_id = hnx_rx_req.bits.txn_id;
                next.entries[st.count] = e;
                next.count             = st.count + 1;
            }
            if (w_rsp_fire) next.entries[0].sent_rsp = true;
            if (w_dat_fire) {  // 出队头（紧凑左移）
                for (uint32_t i = 1; i < st.count; ++i) next.entries[i - 1] = st.entries[i];
                next.entries[st.count - 1] = Entry{};
                next.count                 = st.count - 1;
            }
            return next;
        };
}

}  // namespace zj::home
