#pragma once

// HnfStub：DongJiang（HNF 本体）的 P2 行为桩——只为打通 L2↔环↔桩通路，
// P3 由 DongJiang 全量模型替换。**不追求协议完备与周期精确**（验收只要求
// 通路连通 + trace 可重放到 CC 边界）：
//   - 收 REQ：入 16 项等待池，固定 kLatency 拍后先回 RSP(Comp) 再回
//     DAT(CompData)（tgt=req.SrcID、txn 保持、数据全 0）
//   - 收 RSP/DAT（CompAck 等）：直接消费丢弃
//   - 不发 SNP/ERQ（恒 invalid）
// 等待池满 → req_rdy=0 反压，验证反压链路。

#include <array>
#include <cstdint>

#include "model/flit/zj_flit.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/dec.h"

namespace zj::home {

using namespace zj::chi;
using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Dec;

class HnfStub : public wolvicmod::Module {
public:
    static constexpr uint32_t kMax     = 16;
    static constexpr uint32_t kLatency = 8;
    // CHI opcode（zhujiang/chi/Opcode.scala）：RSP.Comp=0x04，DAT.CompData=0x04
    static constexpr uint8_t kOpComp     = 0x04;
    static constexpr uint8_t kOpCompData = 0x04;

    struct Entry {
        bool     valid    = false;
        bool     sent_rsp = false;
        uint64_t due      = 0;
        uint16_t tgt_id   = 0;
        uint16_t txn_id   = 0;

        bool operator==(const Entry&) const = default;
    };

    struct St {
        uint64_t             cycle = 0;
        uint32_t             count = 0;  // 紧凑存储：entries[0..count-1]
        std::array<Entry, kMax> entries{};

        bool operator==(const St&) const = default;
    };

    IN(bool, clk);
    // hnx 侧（接 HomeShell hnx_*：rx=eject 输入、tx=inject 输出）
    IN(Dec<RReqFlit>, hnx_rx_req);
    OUT(bool, hnx_rx_req_rdy);
    IN(Dec<RespFlit>, hnx_rx_resp);
    OUT(bool, hnx_rx_resp_rdy);
    IN(Dec<DataFlit>, hnx_rx_data);
    OUT(bool, hnx_rx_data_rdy);
    OUT(Dec<RespFlit>, hnx_tx_resp);
    IN(bool, hnx_tx_resp_rdy);
    OUT(Dec<DataFlit>, hnx_tx_data);
    IN(bool, hnx_tx_data_rdy);
    OUT(Dec<SnoopFlit>, hnx_tx_snoop);
    IN(bool, hnx_tx_snoop_rdy);
    OUT(Dec<HReqFlit>, hnx_tx_erq);
    IN(bool, hnx_tx_erq_rdy);

    REG(St, st);

    WIRE(bool, w_head_due);
    WIRE(bool, w_rsp_fire);
    WIRE(bool, w_dat_fire);
    WIRE(bool, w_req_fire);

    HnfStub() {
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
            Dec<RespFlit> d;
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
            Dec<DataFlit> d;
            d.valid = w_head_due && st.entries[0].sent_rsp;
            d.bits  = DataFlit{};
            if (d.valid) {
                d.bits.opcode = kOpCompData;
                d.bits.tgt_id = st.entries[0].tgt_id;
                d.bits.txn_id = st.entries[0].txn_id;
            }
            return d;
        };
        hnx_tx_snoop = Dec<SnoopFlit>{};
        hnx_tx_erq   = Dec<HReqFlit>{};

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
};

}  // namespace zj::home
