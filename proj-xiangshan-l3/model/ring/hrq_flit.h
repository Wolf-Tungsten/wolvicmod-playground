#pragma once

// HrqFlit：HRQ 环通道（128b 车道）的载荷类型。RTL 中该车道跑压平的
// RingFlit(ringHrqFlitBits=128)，承载 HReqFlit(128b) 或 SnoopFlit(113b 零扩展)
// ——BaseRouter.scala:143-153 注入侧 ERQ/SNP 经 connIcn(checkWidth=false) 合并。
// 本模型用超集 struct（两种 flit 字段并集 + is_snp 判别），四种环车道
// （REQ/RSP/DAT/HRQ）于是都有 .tgt_id/.src_id/.txn_id/.qos 字段可直接访问，
// ChannelTap 等环模块可按同一形态泛型。路由/EjectBuffer 只读这四个字段
// （+DAT 的 data_id），两种载荷在这些字段上的布局完全一致（都在 LSB 端
// 同序对齐），与 RTL 行为等价。

#include <cstdint>

#include "model/zj_flit.h"

namespace zj::chi {

struct HrqFlit {
    // 公共字段
    uint8_t  qos     = 0;   // [3:0]
    uint16_t tgt_id  = 0;   // [10:0]
    uint16_t src_id  = 0;
    uint16_t txn_id  = 0;   // [11:0]
    uint8_t  opcode  = 0;   // hreq 7b / snp 5b
    uint64_t addr    = 0;   // hreq 48b / snp 45b
    bool     is_snp  = false;
    // 仅 ERQ（HReqFlit）
    uint16_t return_nid    = 0;  // [10:0]
    uint16_t return_txn_id = 0;  // [11:0]
    uint8_t  size          = 0;  // [2:0]
    uint8_t  order         = 0;  // [1:0]
    uint8_t  mem_attr      = 0;  // [3:0]
    bool     snp_attr      = false;
    bool     excl          = false;
    bool     exp_comp_ack  = false;
    // 仅 SNP（SnoopFlit）
    uint16_t fwd_nid         = 0;  // [10:0]
    uint16_t fwd_txn_id      = 0;  // [11:0]
    bool     do_not_go_to_sd = false;
    bool     ret_to_src      = false;

    static HrqFlit fromHreq(const HReqFlit& f) {
        HrqFlit h;
        h.qos = f.qos; h.tgt_id = f.tgt_id; h.src_id = f.src_id; h.txn_id = f.txn_id;
        h.opcode = f.opcode; h.addr = f.addr; h.is_snp = false;
        h.return_nid = f.return_nid; h.return_txn_id = f.return_txn_id;
        h.size = f.size; h.order = f.order; h.mem_attr = f.mem_attr;
        h.snp_attr = f.snp_attr; h.excl = f.excl; h.exp_comp_ack = f.exp_comp_ack;
        return h;
    }
    static HrqFlit fromSnp(const SnoopFlit& f) {
        HrqFlit h;
        h.qos = f.qos; h.tgt_id = f.tgt_id; h.src_id = f.src_id; h.txn_id = f.txn_id;
        h.opcode = f.opcode; h.addr = f.addr; h.is_snp = true;
        h.fwd_nid = f.fwd_nid; h.fwd_txn_id = f.fwd_txn_id;
        h.do_not_go_to_sd = f.do_not_go_to_sd; h.ret_to_src = f.ret_to_src;
        return h;
    }
    HReqFlit toHreq() const {  // 调用方保证 !is_snp（RTL 侧是类型静态保证）
        HReqFlit f;
        f.qos = qos; f.tgt_id = tgt_id; f.src_id = src_id; f.txn_id = txn_id;
        f.opcode = opcode; f.addr = addr;
        f.return_nid = return_nid; f.return_txn_id = return_txn_id;
        f.size = size; f.order = order; f.mem_attr = mem_attr;
        f.snp_attr = snp_attr; f.excl = excl; f.exp_comp_ack = exp_comp_ack;
        return f;
    }
    SnoopFlit toSnp() const {  // 调用方保证 is_snp
        SnoopFlit f;
        f.qos = qos; f.tgt_id = tgt_id; f.src_id = src_id; f.txn_id = txn_id;
        f.opcode = opcode; f.addr = addr;
        f.fwd_nid = fwd_nid; f.fwd_txn_id = fwd_txn_id;
        f.do_not_go_to_sd = do_not_go_to_sd; f.ret_to_src = ret_to_src;
        return f;
    }

    bool operator==(const HrqFlit&) const = default;
};

}  // namespace zj::chi
