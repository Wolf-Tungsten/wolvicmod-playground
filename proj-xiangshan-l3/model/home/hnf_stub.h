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
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::home {

using namespace zj::chi;
using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Valid;

class HnfStub : public wolvicmod::Module {
public:
    IN(bool, clk);
    // hnx 侧（接 HomeShell hnx_*：rx=eject 输入、tx=inject 输出）
    IN(Valid<RReqFlit>, hnx_rx_req);
    OUT(bool, hnx_rx_req_rdy);
    IN(Valid<RespFlit>, hnx_rx_resp);
    OUT(bool, hnx_rx_resp_rdy);
    IN(Valid<DataFlit>, hnx_rx_data);
    OUT(bool, hnx_rx_data_rdy);
    OUT(Valid<RespFlit>, hnx_tx_resp);
    IN(bool, hnx_tx_resp_rdy);
    OUT(Valid<DataFlit>, hnx_tx_data);
    IN(bool, hnx_tx_data_rdy);
    OUT(Valid<SnoopFlit>, hnx_tx_snoop);
    IN(bool, hnx_tx_snoop_rdy);
    OUT(Valid<HReqFlit>, hnx_tx_erq);
    IN(bool, hnx_tx_erq_rdy);

    HnfStub();

private:
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

    REG(St, st);

    WIRE(bool, w_head_due);
    WIRE(bool, w_rsp_fire);
    WIRE(bool, w_dat_fire);
    WIRE(bool, w_req_fire);
};

}  // namespace zj::home
