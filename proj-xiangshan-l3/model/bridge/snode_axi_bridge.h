#pragma once

// SNodeAxiBridge：S 节点（gid6，mem_0）的 CHI-SN→AXI4 桥，对齐
// zhujiang/device/bridge/axi/AxiBridge.scala。kunminghu-v3：
// outstanding=64（ZhuJiangNoCTopology.scala MemoryOutstanding）、
// AXI id 6b / addr 48b / data 256b、compareTag=addr[37:6]（32KB 粒度保序）。
//
// 结构（AxiBridge.scala:35-148）：
//   rx_req（HReqFlit，ERQ）→ PickOneLow 选最低空闲 CM 入队；
//   64 项 CM 状态阵列（BridgeCm 已拍平为共享算法 CmLogic<CmST>，见
//   bridge_cm.h）：tx_resp 经 ConditionVipArbiter(qos>=) 合流；
//   aw/ar 各经 ConditionVipArbiter 合流；awQueue(64) 记录 aw 授权序 →
//   W 数据按序从 AxiDataBuffer 读出（AXI3 无 WID，W 必须按 AW 序）；
//   alloc 经 DataBufferAllocReqSelector（= CondVipArb + Queue(2)，
//   AllocSelector.scala）入 AxiDataBuffer；
//   axi.r → readDataPipe(Queue(1,pipe)) 组 CompData → tx_data；
//   axi.b 恒 ready 按 id 分发；同 tag 保序 waitNum 桥顶统计广播。
// ZJPerf/MbistPipeline/working（时钟门控）不建模（§3.4 约定）。
// 断言（opvec 合法性、awQueue 不满、wakeup 唯一、timer 不超时）转注释。

#include <array>
#include <cstdint>
#include <string>

#include "model/bridge/axi_data_buffer.h"
#include "model/bridge/axi_flit.h"
#include "model/bridge/bridge_cm.h"
#include "model/flit/zj_flit.h"
#include "prefab/xsarb.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"
#include "wolvicmod/prefab/queue.h"

namespace zj::bridge {

using namespace zj::chi;
using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Valid;
using wolvicmod::prefab::Queue;
using zj::prefab::CondVipArb;

// ---------------- CmST：S 桥 CM traits ----------------

struct CmST {
    static constexpr bool     kSn      = true;
    static constexpr uint32_t kOutst   = 64;
    static constexpr uint32_t kAllOnes = 63;  // Fill(6, 1)
    using ReqT = HReqFlit;

    // AxiCtrlInfo（mem=true；ioDataBits=0/withData=false）
    struct Info {
        uint64_t addr       = 0;  // [47:0]
        uint8_t  size       = 0;  // [2:0]
        uint16_t txn_id     = 0;  // [11:0]
        uint16_t src_id     = 0;  // [10:0]
        uint16_t return_nid = 0;  // [10:0]
        uint16_t return_txn = 0;  // [11:0]
        bool     dwt        = false;
        bool     ewa        = false;
        bool     device     = false;
        uint8_t  readCnt    = 0;
        uint8_t  qos        = 0;  // [3:0]
        bool     isSnooped  = false;

        bool operator==(const Info&) const = default;
    };

    static bool tagMatch(uint64_t a, uint64_t b) {  // addr[37:6]
        return ((a >> 6) & 0xFFFFFFFFULL) == ((b >> 6) & 0xFFFFFFFFULL);
    }
    static bool uCompleted(const UState& u) {
        return u.receiptResp && u.dbidResp && u.wdata && u.rdata && u.comp;
    }
    static bool     dwtOf(const Info& i) { return i.dwt; }
    static uint16_t returnNidOf(const Info& i) { return i.return_nid; }
    static uint16_t returnTxnIdOf(const Info& i) { return i.return_txn; }

    static void enqEntry(const ReqT& req, UState& u, DState& d, Info& info,
                         bool& bufferAllocated) {
        // IcnIoDevRsEntryCommon.enq + AxiRsEntry.enq
        info.addr       = req.addr;
        info.size       = req.size;
        info.txn_id     = req.txn_id;
        info.src_id     = req.src_id;
        info.return_nid = req.return_nid;
        info.return_txn = req.return_txn_id;
        info.dwt        = req.opcode != req_op::kReadNoSnp && req.snp_attr;  // DoDWT
        info.ewa        = req.mem_attr & 1;
        info.device     = (req.mem_attr >> 1) & 1;
        info.readCnt    = 0;
        info.isSnooped  = true;
        info.qos        = req.qos;
        decodeU(req, u);
        decodeD(req, d);
        bufferAllocated = req.opcode == req_op::kReadNoSnp;
    }

    static void decodeU(const ReqT& req, UState& u) {
        // ChiUpstreamOpVec.decode（合法 opcode：ReadNoSnp/WriteNoSnpPtl/
        // WriteNoSnpFull/WriteNoSnpFullCleanInv，Size<=6 —— RTL 断言）
        if (req.opcode == req_op::kReadNoSnp) {
            u.receiptResp = req.order == 0;
            u.dbidResp = true; u.wdata = true; u.rdata = false; u.comp = true;
        } else {
            u.receiptResp = true; u.dbidResp = false; u.wdata = false;
            u.rdata = true; u.comp = false;
        }
        u.compAck = !req.exp_comp_ack;  // S 不计入 completed
    }

    static void decodeD(const ReqT& req, DState& d) {
        if (req.opcode == req_op::kReadNoSnp) {
            d.waddr = true; d.raddr = false; d.wdata = true; d.wresp = true;
            d.rdata = false;
        } else {
            d.waddr = false; d.raddr = true; d.wdata = false; d.wresp = false;
            d.rdata = true;
        }
    }

    static axi::AxFlit mkAx(const Info& info, uint32_t idx) {
        axi::AxFlit f;
        f.id    = uint8_t(idx);
        f.addr  = info.addr;
        f.burst = 1;  // INCR
        f.size  = info.size > 5 ? 5 : info.size;  // busSize=5（256b）
        f.len   = info.size > 5 ? uint8_t((1u << (info.size - 5)) - 1) : 0;
        f.cache = uint8_t((!info.device) << 1 | info.ewa);  // Cat(0,0,!device,ewa)
        f.qos   = info.qos;
        return f;
    }

    static axi::WFlit mkW(const Info&) { return {}; }  // DontCare（内容不消费）
};

// ---------------- SNodeAxiBridge ----------------

class SNodeAxiBridge : public wolvicmod::Module {
public:
    static constexpr uint32_t kOutst = CmST::kOutst;
    using InfoV = CmInfoV<CmST>;

    IN(bool, clk);
    // ---- 环侧（DeviceIcnBundle S：rx=弹出输入、tx=注入输出）----
    IN(Valid<HReqFlit>, rx_req);
    OUT(bool, rx_req_rdy);
    IN(Valid<DataFlit>, rx_data);
    OUT(bool, rx_data_rdy);
    OUT(Valid<RespFlit>, tx_resp);
    IN(bool, tx_resp_rdy);
    OUT(Valid<DataFlit>, tx_data);
    IN(bool, tx_data_rdy);
    // ---- AXI master（memAXI）----
    OUT(Valid<axi::AWFlit>, axi_aw);
    IN(bool, axi_aw_rdy);
    OUT(Valid<axi::WFlit>, axi_w);
    IN(bool, axi_w_rdy);
    OUT(Valid<axi::ARFlit>, axi_ar);
    IN(bool, axi_ar_rdy);
    IN(Valid<axi::BFlit>, axi_b);
    OUT(bool, axi_b_rdy);
    IN(Valid<axi::RFlit>, axi_r);
    OUT(bool, axi_r_rdy);

    SNodeAxiBridge();

    // ---- testbench 白盒：cosim harness 看门狗/探针直达 ----
    using RspArbT  = CondVipArb<RespFlit, kOutst>;      // 宏参数含逗号，先取别名
    using AxArbT   = CondVipArb<axi::AxFlit, kOutst>;
    using AwQ      = Queue<uint64_t, kOutst>;
    MOD(RspArbT, rsp_arb);
    MOD(AxArbT, aw_arb);
    MOD(AxArbT, ar_arb);
    MOD(AwQ, aw_q);  // UInt(64.W) 一位热
    MOD(AxiDataBuffer, data_buf);
    // 拍平后的 CM 状态阵列（原 MOD_ARRAY(Cm) 的 cms[i].st）与 per-entry ar 探针
    using CmStArr = std::array<CmSt<CmST>, kOutst>;  // 宏参数含逗号，先取别名
    using AxArr   = std::array<Valid<axi::AxFlit>, kOutst>;
    REG(CmStArr, cms);
    WIRE(AxArr, ar_in);

private:
    using AllocArbT = CondVipArb<AllocReqBits, kOutst>;
    using AllocQ   = Queue<AllocReqBits, 2>;
    using RdPipe   = Queue<DataFlit, 1, false, true>;
    MOD(AllocArbT, alloc_sel);
    MOD(AllocQ, alloc_q);
    MOD(RdPipe, rd_pipe);  // readDataPipe

    using BoolArr = std::array<bool, kOutst>;
    using WkArr   = std::array<WkV, kOutst>;
    using InfoArr = std::array<InfoV, kOutst>;
    using RspArr  = std::array<Valid<RespFlit>, kOutst>;
    using AllocArr = std::array<Valid<AllocReqBits>, kOutst>;
    using WArr    = std::array<Valid<axi::WFlit>, kOutst>;
    using ReqArr  = std::array<Valid<HReqFlit>, kOutst>;
    using BArr    = std::array<Valid<axi::BFlit>, kOutst>;
    using DatArr  = std::array<Valid<DataFlit>, kOutst>;

    REG(BoolArr, tag_match);  // reqTagMatchVecReg

    // CM 阵列（拍平）组合输出数组
    WIRE(WkArr, wk_all);
    WIRE(InfoArr, info_all);
    WIRE(RspArr, rsp_in);
    WIRE(AxArr, aw_in);
    WIRE(AllocArr, alloc_in);
    WIRE(WArr, w_all);
    // per-entry 输入分发 / fire（原 CM 端口连接的数组化）
    WIRE(ReqArr, w_cm_req);          // rx_req × PickOneLow
    WIRE(DatArr, w_cm_rx_data);      // data_buf.to_cm 按 txn_id 分发
    WIRE(BArr, w_cm_b);              // axi_b 按 id 分发
    WIRE(BoolArr, w_cm_rd_fire);     // io.readDataFire
    WIRE(BoolArr, w_cm_rd_last);
    WIRE(BoolArr, w_cm_alloc_resp);  // dataBufferAlloc 响应按 idxOH 分发
    WIRE(BoolArr, w_rsp_fire);
    WIRE(BoolArr, w_aw_fire);
    WIRE(BoolArr, w_ar_fire);
    WIRE(BoolArr, w_w_fire);
    WIRE(BoolArr, w_alloc_fire);
    WIRE(BoolArr, free_lo);       // PickOneLow 一位热
    WIRE(BoolArr, tag_match_c);   // reqTagMatchVec（组合）
    WIRE(bool, w_any_free);
    WIRE(bool, w_req_fire);
    WIRE(uint32_t, w_wait_num);
    WIRE(bool, w_aw_out_fire);
    WIRE(bool, w_wsel_vld);
};

}  // namespace zj::bridge
