#pragma once

// AxiDataBuffer：S 桥写数据缓冲，对齐
// zhujiang/device/bridge/axi/AxiDataBuffer.scala（Freelist/Ram/AxiDataBuffer
// 三合一身）。kunminghu-v3：ctrlSize=bufferSize=64、dw=256（maxAllocOnce=2）。
//
// 结构：
//   freelist：64 槽循环空闲表（head=分配侧、tail=回收侧 + avail 计数）。
//     分配响应组合返回（resp.valid = req.fire）：buf(i)=fl[head+i]、
//     recvMax=reqNum-1、dataIdOffset 随请求。回收经"恒 ready MimoQueue"
//     ——行为等价于 1 拍延迟寄存（分析见下），槽号按 (tail+k) 顺序回表。
//   ctrl 表（每 CM idx）：buf[2]/recvMax/recvCnt/dataIdOffset + ctrlValid。
//   写数据：icn DataFlit（TxnID=CM idx）按 buf[bufIdx] 落 data/mask RAM，
//     bufIdx=(DataID-dataIdOffset)(1,1)（dw=256）；recvCnt 逐拍累计；
//     末拍到齐（recvCnt==recvMax+1）或 WriteDataCancel 时经 1 拍寄存链
//     toCmDat 通知 CM。
//   读出（W 通道）：fromCmDat（awQueue 保序的一位热）→ 寄 txReqBits/
//     ctrlSel/txReqCnt → 逐槽 readDataReq → stage1(set+last) → stage2
//     （组合取 RAM data/strb）→ axi.w。末槽 readDataReq.fire 即释放槽位；
//     allowNewTx = release || !txReqValid（背靠背突发零气泡）。
//
// MimoQueue 等价论证：其 deq 恒 ready，每拍内容（前拍 enq 的槽号）次拍全部
// 流出，count=前拍 enq 数；与 1 拍延迟寄存（rel_entries/rel_cnt）在
// freelist 可见行为（relValid/relNum/回表顺序 (tail+k)）上完全一致。

#include <array>
#include <cstdint>

#include "model/bridge/axi_flit.h"
#include "model/bridge/bridge_cm.h"
#include "model/flit/zj_flit.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"
#include "wolvicmod/prefab/queue.h"

namespace zj::bridge {

using namespace zj::chi;
using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Valid;
using wolvicmod::prefab::Queue;

class AxiDataBuffer : public wolvicmod::Module {
public:
    IN(bool, clk);
    // 分配（来自 allocSel Queue(2) deq）
    IN(Valid<AllocReqBits>, alloc);
    OUT(bool, alloc_rdy);
    // 环侧写数据
    IN(Valid<DataFlit>, icn);
    OUT(bool, icn_rdy);
    // CM 通知（valid-only，CM 恒 ready）
    OUT(Valid<DataFlit>, to_cm);
    // W 选择（awQueue 保序一位热）
    IN(Valid<uint64_t>, from_cm);
    OUT(bool, from_cm_rdy);
    // AXI W
    OUT(Valid<axi::WFlit>, axi_w);
    IN(bool, axi_w_rdy);

    AxiDataBuffer();

    static constexpr uint32_t kSize = 64;  // ctrlSize = bufferSize = outstanding

    // ---- testbench 白盒：cosim harness 看门狗 dump ----
    struct CtrlEntry {
        std::array<uint16_t, 2> buf{};
        uint8_t                 recv_max       = 0;
        uint8_t                 recv_cnt       = 0;
        uint8_t                 data_id_offset = 0;

        bool operator==(const CtrlEntry&) const = default;
    };

    struct Ptr {  // CircularQueuePtr（value 模 64，flag 绕回翻转）
        uint32_t value = 0;
        bool     flag  = false;

        Ptr operator+(uint32_t n) const {
            const uint32_t v = value + n;
            return {v >= kSize ? v - kSize : v, v >= kSize ? !flag : flag};
        }
        bool operator==(const Ptr&) const = default;
    };

    struct St {
        std::array<uint16_t, kSize> fl = [] {
            std::array<uint16_t, kSize> a{};
            for (uint32_t i = 0; i < kSize; ++i) a[i] = uint16_t(i);
            return a;
        }();  // RegInit(VecInit(tabulate(64)(_.U)))
        Ptr     head{0, false}, tail{0, true};  // head f=false / tail f=true
        uint32_t avail = kSize;                 // availableSlots
        // 回收延迟链（MimoQueue 等价）
        std::array<uint16_t, 4> rel_entries{};
        uint32_t                rel_cnt = 0;
        // ctrl 表
        std::array<bool, kSize>       ctrl_valid{};
        std::array<CtrlEntry, kSize>  ctrl{};
        // tx 管线
        bool      tx_req_vld = false;
        uint64_t  tx_bits    = 0;   // fromCmDat idxOH 寄存
        CtrlEntry tx_ctrl{};        // ctrlSelReg
        uint32_t  tx_cnt     = 0;
        // icn 接收寄存
        bool     rx_vld_reg = false;
        DataFlit rx_bits_reg{};
        // RAM
        std::array<std::array<uint64_t, 4>, kSize> data_ram{};
        std::array<uint32_t, kSize>                mask_ram{};

        bool operator==(const St&) const = default;
    };

    REG(St, st);

private:
    // stage1 管线载荷：读地址槽号 + last（data/strb 在 stage2 组合取）
    struct S1Bits {
        uint16_t set  = 0;
        bool     last = false;

        bool operator==(const S1Bits&) const = default;
    };

    using Stage1Q = Queue<S1Bits, 1, false, true>;      // 宏参数含逗号，先取别名
    using Stage2Q = Queue<axi::WFlit, 1, false, true>;
    MOD(Stage1Q, stage1);
    MOD(Stage2Q, stage2);

    WIRE(uint32_t, w_req_num);
    WIRE(bool, w_alloc_fire);
    WIRE(bool, w_rdr_fire);   // readDataReq.fire
    WIRE(bool, w_rdr_last);
    WIRE(bool, w_release);    // freelist release.valid
    WIRE(bool, w_cancel);     // freelist cancel.valid
    WIRE(bool, w_allow_new);
};

}  // namespace zj::bridge
