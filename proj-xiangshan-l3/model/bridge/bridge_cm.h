#pragma once

// BridgeCm：两桥（SNodeAxiBridge / HiNodeAxiLiteBridge）共用的控制状态机，
// 对齐 zhujiang/device/bridge/BaseCtrlMachine.scala + 各自的
// *CtrlMachine.scala 与 package.scala（opvec/info/entry 类型参数 → Tr 转写）。
//
// 每 CM 跟踪一笔 CHI 事务的双侧进度：
//   u（ChiUpstreamOpVec，bridge/package.scala:12-49）：回环侧——ReadReceipt/
//     DBID(CompDBID)Resp/Comp 的发送，写数据/读数据/CompAck(HI) 的收取
//   d（DownstreamOpVec）：AXI 侧——aw/ar/w 发出、b/r 收取
// 事务完成 = u.completed && d.completed → valid 撤除。
//
// 同地址（tag 粒度）排序（BaseCtrlMachine.scala:68-86）：
//   入队当拍 waiting ← 全 1；次拍 ← waitNum（桥顶统计的"未完成同 tag 老 CM
//   数"，见桥顶 reqTagMatchVec）；之后每个同 tag 老 CM 的 wakeup 脉冲使
//   waiting-1（RTL 断言同拍至多一个，同 tag 完成严格串行）。aw/ar/alloc 的
//   发出要求 waiting==0。本 CM 下游完成（kSn: d.wresp&&d.rdata；HI:
//   d.completed）且 isSnooped 时发 1 拍 wakeupOut 脉冲并清 isSnooped。
//
// Tr（traits）承载两桥差异：kSn / kOutst / ReqT / Info / kTagShift/kTagBits /
// enqInfo / dwtOf / mkAx（aw/ar 字段）/ mkW（仅 HI 有实体内容）。

#include <array>
#include <cstdint>

#include "model/bridge/axi_flit.h"
#include "model/flit/zj_flit.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/dec.h"

namespace zj::bridge {

using namespace zj::chi;
using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Dec;

// wakeup 广播载荷（Valid(addr)）
struct WkV {
    bool     valid = false;
    uint64_t addr  = 0;

    bool operator==(const WkV&) const = default;
};

// AxiDownstreamOpVec（axi/package.scala:9-44，两桥同构）
struct DState {
    bool waddr = false, raddr = false, wdata = false, wresp = false, rdata = false;

    bool completed() const { return waddr && raddr && wdata && wresp && rdata; }

    bool operator==(const DState&) const = default;
};

// ChiUpstreamOpVec（bridge/package.scala:12-49；compAck 仅 HI 计入 completed）
struct UState {
    bool receiptResp = false, dbidResp = false, wdata = false, rdata = false;
    bool compAck = false, comp = false;

    bool operator==(const UState&) const = default;
};

// DataBufferAllocReq（S 桥，axi/AllocSelector.scala:7-11 + idxOH）。
// 注：RTL 中 idxOH 由 AllocSelector 内层 selArb 输入赋 1<<i（CM 侧 DontCare），
// 模型让 CM 直接带上（等价）。
struct AllocReqBits {
    uint64_t idx_oh         = 0;   // [63:0] 一位热
    uint8_t  qos            = 0;   // [3:0]
    uint8_t  size           = 0;   // [2:0]
    uint8_t  data_id_offset = 0;   // [1:0]

    bool operator==(const AllocReqBits&) const = default;
};

// rocket-chip MaskGen（util/Misc.scala:197）：size 对齐窗口（非字节精确）：
// size>=lgBytes 全拍使能；否则 window=1<<size 字节、shift=addr 按 window 向下
// 对齐（对拍实证：size=4@addr=2 → 0xFFFF 而非 0xFFFF<<2）
inline uint32_t maskGen(uint64_t addr, uint8_t size, uint32_t beatBytesLog2) {
    const uint32_t beatBytes = 1u << beatBytesLog2;
    if (size >= beatBytesLog2) return beatBytes >= 32 ? ~0u : ((1u << beatBytes) - 1);
    const uint32_t window = 1u << size;
    const uint32_t shift  = (uint32_t(addr) & (beatBytes - 1)) & ~(window - 1);
    return ((1u << window) - 1) << shift;
}

template <class Tr>
class BridgeCm : public wolvicmod::Module {
public:
    static constexpr uint32_t kOutst = Tr::kOutst;
    using ReqT  = typename Tr::ReqT;
    using Info  = typename Tr::Info;
    using WkArr = std::array<WkV, kOutst>;

    struct St {
        bool     valid   = false;  // RegInit(false.B)
        uint32_t waiting = 0;
        UState   u;
        DState   d;
        Info     info{};
        bool     alloc_issued      = false;  // S：allocReqIssued
        bool     buffer_allocated  = false;  // S：state.bufferAllocated
        bool     wk_vld_reg        = false;  // RegNext(wakeupValid)
        uint32_t wk_num_reg        = 0;      // RegEnable(PopCount, wakeupValid)
        bool     wait_set_en       = false;  // RegNext(rx.req.fire)

        bool operator==(const St&) const = default;
    };

    struct InfoV {  // io.info（Valid(info)）
        bool valid = false;
        Info info{};

        bool operator==(const InfoV&) const = default;
    };

    IN(bool, clk);
    IN(uint32_t, idx);
    // CHI 侧
    IN(Dec<ReqT>, rx_req);
    OUT(bool, rx_req_rdy);
    IN(Dec<RespFlit>, rx_resp);  // 仅 HI 消费（CompAck）；S 桥顶 tie invalid
    IN(Dec<DataFlit>, rx_data);  // rdy 恒 true（RTL 直连，不出口）
    OUT(Dec<RespFlit>, tx_resp);
    IN(bool, tx_resp_rdy);
    // AXI 侧
    OUT(Dec<axi::AWFlit>, axi_aw);
    IN(bool, axi_aw_rdy);
    OUT(Dec<axi::ARFlit>, axi_ar);
    IN(bool, axi_ar_rdy);
    OUT(Dec<axi::WFlit>, axi_w);
    IN(bool, axi_w_rdy);
    IN(Dec<axi::BFlit>, axi_b);  // rdy 恒 true
    // 桥顶广播
    IN(bool, rd_fire);  // io.readDataFire = axi.r.fire && id===idx
    IN(bool, rd_last);
    IN(uint32_t, wait_num);
    IN(WkArr, wk_in);
    OUT(WkV, wakeup_out);
    OUT(InfoV, info_out);
    // S：dataBufferAlloc（HI 不消费，CM 内 tie invalid）
    OUT(Dec<AllocReqBits>, alloc_req);
    IN(bool, alloc_req_rdy);
    IN(bool, alloc_resp);

    BridgeCm();

    // ---- testbench 白盒：cosim harness 看门狗 dump ----
    REG(St, st);

private:
    WIRE(bool, w_req_fire);
    WIRE(bool, w_resp_fire);
    WIRE(bool, w_aw_fire);
    WIRE(bool, w_ar_fire);
    WIRE(bool, w_w_fire);
    WIRE(bool, w_alloc_fire);
    WIRE(bool, w_wk_vld);     // wakeupValid（组合）
    WIRE(bool, w_wk_out_v);   // io.wakeupOut.valid
    WIRE(bool, w_allow_comp);
    WIRE(bool, w_icn_receipt);
    WIRE(bool, w_icn_dbid);
    WIRE(bool, w_icn_comp);
    WIRE(uint8_t, w_rsp_op);
};

template <class Tr>
BridgeCm<Tr>::BridgeCm() {
        rx_req_rdy.assign().reads(st) = [](auto src) {
            auto [st] = src;
            return !st.valid;
        };
        w_req_fire.assign().reads(rx_req, st) = [](auto src) {
            auto [rx_req, st] = src;
            return rx_req.valid && !st.valid;
        };

        // ---- wakeup 侦测（BaseCtrlMachine.scala:68-71；剔除自身）----
        w_wk_vld.assign().reads(wk_in, st, idx) = [](auto src) {
            auto [wk_in, st, idx] = src;
            if (!st.valid) return false;
            for (uint32_t j = 0; j < kOutst; ++j)
                if (j != idx && wk_in[j].valid && Tr::tagMatch(wk_in[j].addr, st.info.addr))
                    return true;
            return false;
        };
        w_wk_out_v.assign().reads(st) = [](auto src) {
            auto [st] = src;
            const bool wakeup =
                Tr::kSn ? (st.d.wresp && st.d.rdata) : st.d.completed();
            return wakeup && st.valid && st.info.isSnooped;
        };
        wakeup_out.assign().reads(st, w_wk_out_v) = [](auto src) {
            auto [st, w_wk_out_v] = src;
            WkV o;
            o.valid = w_wk_out_v;
            o.addr  = st.info.addr;
            return o;
        };

        // ---- 回环响应（BaseCtrlMachine.scala:120-151）----
        w_allow_comp.assign().reads(st) = [](auto src) {
            auto [st] = src;
            if (!st.info.ewa) return st.d.completed();
            return Tr::dwtOf(st.info) ? st.u.wdata : true;
        };
        w_icn_receipt.assign().reads(st) = [](auto src) {
            auto [st] = src;
            return !st.u.receiptResp;
        };
        w_icn_dbid.assign().reads(st) = [](auto src) {
            auto [st] = src;
            const bool base = Tr::kSn ? st.buffer_allocated : true;
            return base && !st.u.dbidResp;
        };
        w_icn_comp.assign().reads(st, w_allow_comp) = [](auto src) {
            auto [st, w_allow_comp] = src;
            const bool base = Tr::kSn ? st.buffer_allocated : true;
            return w_allow_comp && base && !st.u.comp;
        };
        // opcode 优先序：CompDBIDResp > ReadReceipt > DBIDResp > Comp
        w_rsp_op.assign().reads(w_icn_receipt, w_icn_dbid, w_icn_comp) = [](auto src) -> uint8_t {
            auto [w_icn_receipt, w_icn_dbid, w_icn_comp] = src;
            if (w_icn_dbid && w_icn_comp) return rsp_op::kCompDBIDResp;
            if (w_icn_receipt) return rsp_op::kReadReceipt;
            if (w_icn_dbid) return rsp_op::kDBIDResp;
            if (w_icn_comp) return rsp_op::kComp;
            return 0;
        };
        tx_resp.assign().reads(st, idx, w_icn_receipt, w_icn_dbid, w_icn_comp, w_rsp_op) =
            [](auto src) {
                auto [st, idx, w_icn_receipt, w_icn_dbid, w_icn_comp, w_rsp_op] = src;
                Dec<RespFlit> d;
                d.valid = st.valid && (w_icn_receipt || w_icn_dbid || w_icn_comp);
                auto& b = d.bits;
                b.opcode  = w_rsp_op;
                b.qos     = st.info.qos;
                b.dbid    = uint16_t(idx);
                const bool dwtRoute = w_icn_dbid && Tr::dwtOf(st.info);
                b.txn_id  = dwtRoute ? Tr::returnTxnIdOf(st.info) : st.info.txn_id;
                b.src_id  = 0;
                b.tgt_id  = dwtRoute ? Tr::returnNidOf(st.info) : st.info.src_id;
                b.resp    = 0;
                return d;
            };
        w_resp_fire.assign().reads(tx_resp, tx_resp_rdy) = [](auto src) {
            auto [tx_resp, tx_resp_rdy] = src;
            return tx_resp.valid && tx_resp_rdy;
        };

        // ---- AXI 发出（*CtrlMachine.scala；waiting==0 门控）----
        axi_aw.assign().reads(st, idx) = [](auto src) {
            auto [st, idx] = src;
            Dec<axi::AWFlit> d;
            d.valid = st.valid && !st.d.waddr && st.u.wdata && st.waiting == 0;
            d.bits  = Tr::mkAx(st.info, idx);
            return d;
        };
        axi_ar.assign().reads(st, idx) = [](auto src) {
            auto [st, idx] = src;
            Dec<axi::ARFlit> d;
            d.valid = st.valid && !st.d.raddr && st.waiting == 0;
            d.bits  = Tr::mkAx(st.info, idx);
            return d;
        };
        axi_w.assign().reads(st, idx) = [](auto src) {
            auto [st, idx] = src;
            Dec<axi::WFlit> d;
            d.valid = st.valid && st.d.waddr && !st.d.wdata && st.u.wdata &&
                      st.waiting == 0;
            d.bits  = Tr::mkW(st.info);
            return d;
        };
        w_aw_fire.assign().reads(axi_aw, axi_aw_rdy) = [](auto src) {
            auto [axi_aw, axi_aw_rdy] = src;
            return axi_aw.valid && axi_aw_rdy;
        };
        w_ar_fire.assign().reads(axi_ar, axi_ar_rdy) = [](auto src) {
            auto [axi_ar, axi_ar_rdy] = src;
            return axi_ar.valid && axi_ar_rdy;
        };
        w_w_fire.assign().reads(axi_w, axi_w_rdy) = [](auto src) {
            auto [axi_w, axi_w_rdy] = src;
            return axi_w.valid && axi_w_rdy;
        };

        // ---- dataBufferAlloc（S）----
        if constexpr (Tr::kSn) {
            alloc_req.assign().reads(st, idx) = [](auto src) {
                auto [st, idx] = src;
                Dec<AllocReqBits> d;
                d.valid = st.valid && !st.alloc_issued && !st.buffer_allocated &&
                          st.waiting == 0;
                d.bits.idx_oh         = uint64_t{1} << idx;
                d.bits.qos            = st.info.qos;
                d.bits.size           = st.info.size;
                d.bits.data_id_offset = uint8_t((st.info.addr >> 5) & 1) << 1;  // dw=256
                return d;
            };
            w_alloc_fire.assign().reads(alloc_req, alloc_req_rdy) = [](auto src) {
                auto [alloc_req, alloc_req_rdy] = src;
                return alloc_req.valid && alloc_req_rdy;
            };
        } else {
            alloc_req   = Dec<AllocReqBits>{};
            w_alloc_fire = false;
        }

        // ---- info_out / 完成判定 ----
        info_out.assign().reads(st) = [](auto src) {
            auto [st] = src;
            InfoV o;
            o.valid = st.valid;
            o.info  = st.info;
            return o;
        };

        // ---- 状态漏斗 ----
        st.update().on(posedge(clk))
            .reads(st, idx, rx_req, rx_resp, rx_data, axi_b, rd_fire, rd_last, wait_num,
                   w_req_fire, w_resp_fire, w_aw_fire, w_ar_fire, w_w_fire, w_alloc_fire,
                   w_wk_vld, w_wk_out_v, w_rsp_op, alloc_resp) = [](auto src) {
                auto [st, idx, rx_req, rx_resp, rx_data, axi_b, rd_fire, rd_last, wait_num,
                      w_req_fire, w_resp_fire, w_aw_fire, w_ar_fire, w_w_fire, w_alloc_fire,
                      w_wk_vld, w_wk_out_v, w_rsp_op, alloc_resp] = src;
                St next = st;
                // valid（BaseCtrlMachine.scala:55）
                const bool allDone = Tr::uCompleted(st.u) && st.d.completed();
                next.valid       = st.valid ? !allDone : w_req_fire;
                // waiting（:72-80）
                if (w_req_fire)
                    next.waiting = Tr::kAllOnes;
                else if (st.wait_set_en)
                    next.waiting = wait_num;
                else if (st.wk_vld_reg) {
                    // RTL 断言 wk_num_reg===1（同 tag 完成严格串行）
                    next.waiting = st.waiting - 1;
                }
                next.wk_vld_reg  = w_wk_vld;
                next.wait_set_en = w_req_fire;
                // alloc_issued（S，AxiBridgeCtrlMachine.scala:51-55）
                if (w_req_fire)
                    next.alloc_issued = false;
                else if (w_alloc_fire)
                    next.alloc_issued = true;

                if (w_req_fire) {
                    // payloadEnqNext：u/d/info 整体覆盖（IcnIoDevRsEntryCommon.enq）
                    Tr::enqEntry(rx_req.bits, next.u, next.d, next.info,
                                 next.buffer_allocated);
                } else if (st.valid) {
                    // payloadMiscNext：事件累积
                    if (alloc_resp) next.buffer_allocated = true;
                    if (rd_fire) {  // u/d.rdata + readCnt（BaseCM:94-97 + 各 CM）
                        next.u.rdata      = rd_last || st.u.rdata;
                        next.d.rdata      = rd_last || st.d.rdata;
                        next.info.readCnt = st.info.readCnt + 1;
                    }
                    if constexpr (!Tr::kSn) {  // HI：rx.resp（CompAck）
                        if (rx_resp.valid)
                            next.u.compAck =
                                (rx_resp.bits.opcode == rsp_op::kCompAck) || st.u.compAck;
                    }
                    if (rx_data.valid) {
                        if constexpr (!Tr::kSn) {  // HI：64b 数据/掩码抽取（BaseCM:106-113）
                            Tr::extractData(st.info.addr, rx_data.bits, next.info);
                        }
                        const uint8_t op = rx_data.bits.opcode;
                        next.u.wdata = (op == dat_op::kNCBWrDataCompAck ||
                                        op == dat_op::kNonCopyBackWriteData ||
                                        op == dat_op::kWriteDataCancel) ||
                                       st.u.wdata;
                        if constexpr (!Tr::kSn) {
                            next.u.compAck = (op == dat_op::kNCBWrDataCompAck ||
                                              op == dat_op::kWriteDataCancel) ||
                                             st.u.compAck;
                        } else {  // S：WriteDataCancel 直接补齐下游（AxiBridgeCM:110-114）
                            if (op == dat_op::kWriteDataCancel) {
                                next.d.waddr = true;
                                next.d.wdata = true;
                                next.d.wresp = true;
                            }
                        }
                    }
                    if (axi_b.valid) next.d.wresp = true;
                    if (w_aw_fire) next.d.waddr = true;
                    if (w_ar_fire) next.d.raddr = true;
                    if (w_w_fire) next.d.wdata = true;
                    if (w_resp_fire) {  // BaseCtrlMachine.scala:147-151
                        next.u.receiptResp = (w_rsp_op == rsp_op::kReadReceipt) || st.u.receiptResp;
                        next.u.dbidResp    = (w_rsp_op == rsp_op::kDBIDResp ||
                                              w_rsp_op == rsp_op::kCompDBIDResp) ||
                                             st.u.dbidResp;
                        next.u.comp        = (w_rsp_op == rsp_op::kComp ||
                                              w_rsp_op == rsp_op::kCompDBIDResp) ||
                                             st.u.comp;
                    }
                    if (w_wk_out_v) next.info.isSnooped = false;
                }
                return next;
            };
    }

}  // namespace zj::bridge
