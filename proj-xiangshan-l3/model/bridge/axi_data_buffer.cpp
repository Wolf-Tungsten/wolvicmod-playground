#include "model/bridge/axi_data_buffer.h"

#include <wolvicmod/wolvicmod.h>

namespace zj::bridge {

AxiDataBuffer::AxiDataBuffer() {
    stage1.clk = clk;
    stage2.clk = clk;

    // ---- 分配（freelist.req/resp）----
    w_req_num.assign().reads(alloc) = [](auto src) -> uint32_t {
        auto [alloc] = src;
        const uint8_t size = alloc.bits.size;
        return size > 5 ? (1u << (size - 5)) : 1u;  // dataWidthInBytesShift=5
    };
    alloc_rdy.assign().reads(st, w_req_num) = [](auto src) {
        auto [st, w_req_num] = src;
        return st.avail >= w_req_num;
    };
    w_alloc_fire.assign().reads(alloc, alloc_rdy) = [](auto src) {
        auto [alloc, alloc_rdy] = src;
        return alloc.valid && alloc_rdy;
    };

    // ---- 写数据接收 ----
    // icn_rdy：SRAM 写口恒 ready（dual-port、无 ShouldReset/interval），
    // RTL 中 icn.ready = dataRam.io.w.req.ready 恒 1（对拍实证）
    icn_rdy = true;

    // ---- toCmDat 通知（1 拍寄存链）----
    to_cm.assign().reads(st) = [](auto src) {
        auto [st] = src;
        const uint32_t  txn = st.rx_bits_reg.txn_id & (kSize - 1);
        const CtrlEntry& ce  = st.ctrl[txn];
        Valid<DataFlit> d;
        d.valid = st.rx_vld_reg &&
                  (ce.recv_cnt == ce.recv_max + 1 ||
                   st.rx_bits_reg.opcode == dat_op::kWriteDataCancel);
        d.bits = st.rx_bits_reg;
        return d;
    };
    w_cancel.assign().reads(st) = [](auto src) {
        auto [st] = src;
        return st.rx_vld_reg && st.rx_bits_reg.opcode == dat_op::kWriteDataCancel;
    };

    // ---- 读出（W 通道）----
    w_allow_new.assign().reads(st, w_release) = [](auto src) {
        auto [st, w_release] = src;
        return w_release || !st.tx_req_vld;
    };
    from_cm_rdy = w_allow_new;
    // readDataReq：valid=txReqValid（不经 ready 门控），fire 受 stage1 反压
    stage1.enq.assign().reads(st) = [](auto src) {
        auto [st] = src;
        Valid<S1Bits> d;
        d.valid     = st.tx_req_vld;
        d.bits.set  = st.tx_ctrl.buf[st.tx_cnt & 1];
        d.bits.last = st.tx_cnt == st.tx_ctrl.recv_max;
        return d;
    };
    w_rdr_last.assign().reads(st) = [](auto src) {
        auto [st] = src;
        return st.tx_cnt == st.tx_ctrl.recv_max;
    };
    w_rdr_fire.assign().reads(st, stage1.enq_rdy) = [](auto src) {
        auto [st, stage1_enq_rdy] = src;
        return st.tx_req_vld && stage1_enq_rdy;
    };
    w_release.assign().reads(w_rdr_fire, w_rdr_last) = [](auto src) {
        auto [w_rdr_fire, w_rdr_last] = src;
        return w_rdr_fire && w_rdr_last;
    };
    // stage1 → stage2：组合取 RAM
    stage1.deq_rdy = stage2.enq_rdy;
    stage2.enq.assign().reads(stage1.deq, st) = [](auto src) {
        auto [stage1_deq, st] = src;
        Valid<axi::WFlit> d;
        d.valid      = stage1_deq.valid;
        d.bits.data  = st.data_ram[stage1_deq.bits.set];
        d.bits.strb  = st.mask_ram[stage1_deq.bits.set];
        d.bits.last  = stage1_deq.bits.last;
        return d;
    };
    axi_w = stage2.deq;
    stage2.deq_rdy = axi_w_rdy;

    // ---- 状态漏斗 ----
    st.update().on(posedge(clk))
        .reads(st, alloc, w_req_num, w_alloc_fire, icn, from_cm, w_rdr_fire,
               w_rdr_last, w_release, w_cancel, w_allow_new) = [](auto src) {
            auto [st, alloc, w_req_num, w_alloc_fire, icn, from_cm, w_rdr_fire,
                  w_rdr_last, w_release, w_cancel, w_allow_new] = src;
            St next = st;

            // ---- freelist 头尾与 avail（req.fire || relValid 时）----
            const uint32_t alloc_num = w_alloc_fire ? w_req_num : 0;
            if (w_alloc_fire || st.rel_cnt > 0) {
                // 槽号回表：rel_entries[k] → fl[tail+k]
                for (uint32_t k = 0; k < st.rel_cnt; ++k)
                    next.fl[(st.tail.value + k) % kSize] = st.rel_entries[k];
                next.head  = st.head + alloc_num;
                next.tail  = st.tail + st.rel_cnt;
                next.avail = st.avail + st.rel_cnt - alloc_num;
            }

            // ---- 回收延迟链：本拍 release/cancel → 次拍生效 ----
            uint32_t cnt = 0;
            if (w_release) {
                for (uint32_t i = 0; i <= st.tx_ctrl.recv_max; ++i)
                    next.rel_entries[cnt++] = st.tx_ctrl.buf[i];
            }
            if (w_cancel) {
                const CtrlEntry& ce = st.ctrl[st.rx_bits_reg.txn_id & (kSize - 1)];
                for (uint32_t i = 0; i <= ce.recv_max; ++i)
                    next.rel_entries[cnt++] = ce.buf[i];
            }
            next.rel_cnt = cnt;

            // ---- ctrl 表（elsewhen 优先序：alloc > release > cancel）----
            for (uint32_t i = 0; i < kSize; ++i) {
                if (w_alloc_fire && ((alloc.bits.idx_oh >> i) & 1)) {
                    next.ctrl_valid[i] = true;
                } else if (w_release && ((st.tx_bits >> i) & 1)) {
                    next.ctrl_valid[i] = false;
                } else if (w_cancel && (st.rx_bits_reg.txn_id & (kSize - 1)) == i) {
                    next.ctrl_valid[i] = false;
                }
            }
            if (w_alloc_fire) {
                for (uint32_t i = 0; i < kSize; ++i) {
                    if ((alloc.bits.idx_oh >> i) & 1) {
                        CtrlEntry& ce     = next.ctrl[i];
                        ce.buf[0]         = st.fl[st.head.value];
                        ce.buf[1]         = st.fl[(st.head.value + 1) % kSize];
                        ce.recv_max       = uint8_t(w_req_num - 1);
                        ce.data_id_offset = alloc.bits.data_id_offset;
                        ce.recv_cnt       = 0;
                    }
                }
            }
            // recvCnt 累计（RTL 的 when/elsewhen 是逐 idx：alloc 命中才遮蔽
            // 同 idx 的 icn 累计；icn.fire 对 cancel 同样累计）
            for (uint32_t i = 0; i < kSize; ++i) {
                const bool allocHit =
                    w_alloc_fire && ((alloc.bits.idx_oh >> i) & 1);
                const bool icnHit = icn.valid && (icn.bits.txn_id & (kSize - 1)) == i;
                if (!allocHit && icnHit)
                    next.ctrl[i].recv_cnt = st.ctrl[i].recv_cnt + 1;
            }

            // ---- 写数据落 RAM ----
            if (icn.valid && icn.bits.opcode != dat_op::kWriteDataCancel) {
                const uint32_t  txn    = icn.bits.txn_id & (kSize - 1);
                const CtrlEntry& ce     = st.ctrl[txn];
                const uint32_t  buf_idx = ((icn.bits.data_id - ce.data_id_offset) >> 1) & 1;
                const uint16_t  slot    = ce.buf[buf_idx];
                next.data_ram[slot]     = icn.bits.data;
                next.mask_ram[slot]     = icn.bits.be;
            }

            // ---- icn 接收寄存 ----
            next.rx_vld_reg = icn.valid;
            if (icn.valid) next.rx_bits_reg = icn.bits;

            // ---- tx 管线 ----
            if (w_allow_new) next.tx_req_vld = from_cm.valid;
            if (w_allow_new && from_cm.valid) {  // fromCmDat.fire
                next.tx_bits = from_cm.bits;
                next.tx_cnt  = 0;
                for (uint32_t i = 0; i < kSize; ++i)
                    if ((from_cm.bits >> i) & 1) next.tx_ctrl = st.ctrl[i];
            } else if (w_rdr_fire) {
                next.tx_cnt = st.tx_cnt + 1;
            }
            return next;
        };
}

}  // namespace zj::bridge
