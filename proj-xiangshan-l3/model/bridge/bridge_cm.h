#pragma once

// BridgeCm 拍平（docs/perf-breakdown.md §16/§17 同构模式）：RTL 层次的 CM
// 子模块阵列在 C 模型里只是 for 循环——本头文件不再定义 Module，只承载两桥
// （SNodeAxiBridge / HiNodeAxiLiteBridge）共享的 CM 值类型与 per-entry 算法。
// 两个父桥各自持有 REG(std::array<CmSt<Tr>, kOutst>, cms) + 一条 update 循环
// （循环体调 CmLogic<Tr>::cmNext）+ 数组 wire（循环体调 CmLogic<Tr> 的组合
// 函数），两份实例化零重复逻辑。
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
#include "wolvicmod/prefab/valid.h"

namespace zj::bridge {

using namespace zj::chi;
using wolvicmod::prefab::Valid;

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

// ---------------- 拍平后的共享 CM 状态与算法 ----------------

// 一个 CM 的全部寄存器状态（原 BridgeCm<Tr>::st；字段默认值 = 原寄存器初值）
template <class Tr>
struct CmSt {
    using Info = typename Tr::Info;

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

    bool operator==(const CmSt&) const = default;
};

// io.info（Valid(info)）
template <class Tr>
struct CmInfoV {
    bool valid = false;
    typename Tr::Info info{};

    bool operator==(const CmInfoV&) const = default;
};

// per-entry 组合/次态算法（原 BridgeCm 构造体内的 assign/update lambda 逐字
// 转写为静态函数）。NBA/RegNext 语义由调用侧保证：一切判定读旧值 st。
template <class Tr>
struct CmLogic {
    using ReqT  = typename Tr::ReqT;
    using St    = CmSt<Tr>;
    using InfoV = CmInfoV<Tr>;
    static constexpr uint32_t kOutst = Tr::kOutst;
    using WkArr = std::array<WkV, kOutst>;

    // ---- wakeup 侦测（BaseCtrlMachine.scala:68-71；剔除自身）----
    static bool cmWakeupVld(const St& st, uint32_t idx, const WkArr& wk_in) {
        if (!st.valid) return false;
        for (uint32_t j = 0; j < kOutst; ++j)
            if (j != idx && wk_in[j].valid && Tr::tagMatch(wk_in[j].addr, st.info.addr))
                return true;
        return false;
    }
    // wakeupOut.valid（组合）
    static bool cmWakeupOutVld(const St& st) {
        const bool wakeup =
            Tr::kSn ? (st.d.wresp && st.d.rdata) : st.d.completed();
        return wakeup && st.valid && st.info.isSnooped;
    }
    static WkV cmWakeupOut(const St& st) {
        WkV o;
        o.valid = cmWakeupOutVld(st);
        o.addr  = st.info.addr;
        return o;
    }

    // ---- 回环响应（BaseCtrlMachine.scala:120-151）----
    // opcode 优先序：CompDBIDResp > ReadReceipt > DBIDResp > Comp
    static uint8_t cmRspOp(const St& st, bool& icnReceipt, bool& icnDbid, bool& icnComp) {
        const bool allowComp =
            st.info.ewa ? (Tr::dwtOf(st.info) ? st.u.wdata : true) : st.d.completed();
        const bool base = Tr::kSn ? st.buffer_allocated : true;
        icnReceipt = !st.u.receiptResp;
        icnDbid    = base && !st.u.dbidResp;
        icnComp    = allowComp && base && !st.u.comp;
        if (icnDbid && icnComp) return rsp_op::kCompDBIDResp;
        if (icnReceipt) return rsp_op::kReadReceipt;
        if (icnDbid) return rsp_op::kDBIDResp;
        if (icnComp) return rsp_op::kComp;
        return 0;
    }
    static Valid<RespFlit> cmTxResp(const St& st, uint32_t idx) {
        bool icnReceipt, icnDbid, icnComp;
        const uint8_t op = cmRspOp(st, icnReceipt, icnDbid, icnComp);
        Valid<RespFlit> d;
        d.valid = st.valid && (icnReceipt || icnDbid || icnComp);
        auto& b = d.bits;
        b.opcode  = op;
        b.qos     = st.info.qos;
        b.dbid    = uint16_t(idx);
        const bool dwtRoute = icnDbid && Tr::dwtOf(st.info);
        b.txn_id  = dwtRoute ? Tr::returnTxnIdOf(st.info) : st.info.txn_id;
        b.src_id  = 0;
        b.tgt_id  = dwtRoute ? Tr::returnNidOf(st.info) : st.info.src_id;
        b.resp    = 0;
        return d;
    }

    // ---- AXI 发出（*CtrlMachine.scala；waiting==0 门控）----
    static Valid<axi::AxFlit> cmAxiAw(const St& st, uint32_t idx) {
        return Valid<axi::AxFlit>{
            st.valid && !st.d.waddr && st.u.wdata && st.waiting == 0,
            Tr::mkAx(st.info, idx)};
    }
    static Valid<axi::AxFlit> cmAxiAr(const St& st, uint32_t idx) {
        return Valid<axi::AxFlit>{st.valid && !st.d.raddr && st.waiting == 0,
                                  Tr::mkAx(st.info, idx)};
    }
    static Valid<axi::WFlit> cmAxiW(const St& st) {
        return Valid<axi::WFlit>{st.valid && st.d.waddr && !st.d.wdata && st.u.wdata &&
                                     st.waiting == 0,
                                 Tr::mkW(st.info)};
    }

    // ---- dataBufferAlloc（S；HI 恒 invalid，父桥不消费）----
    static Valid<AllocReqBits> cmAllocReq(const St& st, uint32_t idx) {
        Valid<AllocReqBits> d;
        if constexpr (Tr::kSn) {
            d.valid = st.valid && !st.alloc_issued && !st.buffer_allocated &&
                      st.waiting == 0;
            d.bits.idx_oh         = uint64_t{1} << idx;
            d.bits.qos            = st.info.qos;
            d.bits.size           = st.info.size;
            d.bits.data_id_offset = uint8_t((st.info.addr >> 5) & 1) << 1;  // dw=256
        }
        return d;
    }

    // ---- info_out ----
    static InfoV cmInfoOut(const St& st) { return InfoV{st.valid, st.info}; }

    // ---- 状态漏斗（原 st.update lambda；next 初值 = st）----
    static St cmNext(const St& st, uint32_t idx, bool reqFire, const ReqT& reqBits,
                     const Valid<RespFlit>& rxResp, const Valid<DataFlit>& rxData,
                     const Valid<axi::BFlit>& axiB, bool rdFire, bool rdLast,
                     uint32_t waitNum, const WkArr& wkIn, bool respFire, bool awFire,
                     bool arFire, bool wFire, bool allocFire, bool allocResp) {
        St next = st;
        // valid（BaseCtrlMachine.scala:55）
        const bool allDone = Tr::uCompleted(st.u) && st.d.completed();
        next.valid       = st.valid ? !allDone : reqFire;
        // waiting（:72-80）
        if (reqFire)
            next.waiting = Tr::kAllOnes;
        else if (st.wait_set_en)
            next.waiting = waitNum;
        else if (st.wk_vld_reg) {
            // RTL 断言 wk_num_reg===1（同 tag 完成严格串行）
            next.waiting = st.waiting - 1;
        }
        next.wk_vld_reg  = cmWakeupVld(st, idx, wkIn);
        next.wait_set_en = reqFire;
        // alloc_issued（S，AxiBridgeCtrlMachine.scala:51-55）
        if (reqFire)
            next.alloc_issued = false;
        else if (allocFire)
            next.alloc_issued = true;

        if (reqFire) {
            // payloadEnqNext：u/d/info 整体覆盖（IcnIoDevRsEntryCommon.enq）
            Tr::enqEntry(reqBits, next.u, next.d, next.info, next.buffer_allocated);
        } else if (st.valid) {
            // payloadMiscNext：事件累积
            if (allocResp) next.buffer_allocated = true;
            if (rdFire) {  // u/d.rdata + readCnt（BaseCM:94-97 + 各 CM）
                next.u.rdata      = rdLast || st.u.rdata;
                next.d.rdata      = rdLast || st.d.rdata;
                next.info.readCnt = st.info.readCnt + 1;
            }
            if constexpr (!Tr::kSn) {  // HI：rx.resp（CompAck）
                if (rxResp.valid)
                    next.u.compAck =
                        (rxResp.bits.opcode == rsp_op::kCompAck) || st.u.compAck;
            }
            if (rxData.valid) {
                if constexpr (!Tr::kSn) {  // HI：64b 数据/掩码抽取（BaseCM:106-113）
                    Tr::extractData(st.info.addr, rxData.bits, next.info);
                }
                const uint8_t op = rxData.bits.opcode;
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
            if (axiB.valid) next.d.wresp = true;
            if (awFire) next.d.waddr = true;
            if (arFire) next.d.raddr = true;
            if (wFire) next.d.wdata = true;
            if (respFire) {  // BaseCtrlMachine.scala:147-151
                bool icnReceipt, icnDbid, icnComp;
                const uint8_t rspOp = cmRspOp(st, icnReceipt, icnDbid, icnComp);
                next.u.receiptResp = (rspOp == rsp_op::kReadReceipt) || st.u.receiptResp;
                next.u.dbidResp    = (rspOp == rsp_op::kDBIDResp ||
                                      rspOp == rsp_op::kCompDBIDResp) ||
                                     st.u.dbidResp;
                next.u.comp        = (rspOp == rsp_op::kComp ||
                                      rspOp == rsp_op::kCompDBIDResp) ||
                                     st.u.comp;
            }
            if (cmWakeupOutVld(st)) next.info.isSnooped = false;
        }
        return next;
    }
};

}  // namespace zj::bridge
