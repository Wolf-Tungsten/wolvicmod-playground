#pragma once

// zhujiang CHI flit 五件套：ReqFlit/HReqFlit/RespFlit/SnoopFlit/DataFlit
// + NodeIdBundle + FlitType（zhujiang/chi/Flit.scala:27-161）的位级 C++ 镜像。
//
// 参数化方式：编译期 config traits（模板参数 Cfg），与 RTL 的 elaboration-time
// Parameters（ZJParameters）同级——改配置 = 换 traits 重编译，等价于改 Chisel
// 参数重新 elaborate；不提供运行期配置（RTL 也没有运行期改位宽）。
// 默认 ZjFlitCfg = kunminghu-v3 DefaultConfig 锁定值
// （ZhuJiangNoCTopology.scala:17 覆盖 nodeNidBits=8 → niw=11；raw=48；dw=256）。
// M/PB/E/R/S/Y 锁定 0（RTL 中为零宽线网，Flit.scala:28-32），对应字段不例化；
// 若未来配置启用，static_assert 会在编译期拦截、需先补字段。
//
// 总宽已对生成 RTL 核实（build/rtl/Router*.sv 的 RingFlit 端口：Payload+38）：
//   RReq 105 / HReq 128 / Resp 66 / Snoop 113 / Data 375
// pack() 布局 = Chisel asUInt：先声明字段在高位，QoS 恒在 [3:0]。

#include <array>
#include <cstddef>
#include <cstdint>

#include "model/flit/bit_pack.h"

namespace zj::chi {

// kunminghu-v3 DefaultConfig（单/双核拓扑不影响 flit 位宽）
struct ZjFlitCfg {
    static constexpr int kNodeNidBits = 8;  // nodeNidBits（ZJParameters 默认 5，拓扑覆盖为 8）
    static constexpr int kNodeAidBits = 3;  // nodeAidBits
    static constexpr int kNiw = kNodeNidBits + kNodeAidBits;  // nodeIdBits = 11
    static constexpr int kRaw = 48;         // requestAddrBits
    static constexpr int kSaw = kRaw - 3;   // snoopAddrBits
    static constexpr int kDw  = 256;        // dataBits
    static constexpr int kBew = kDw / 8;    // beBits
    // 以下 CHI 可选特性位宽锁定 0（RTL 零宽）：M(MPAM)/PB(PBHA)/E/R/S(SecID1)/Y(RSVDC)
    static constexpr int kM = 0, kPb = 0, kE = 0, kR = 0, kS = 0, kY = 0;

    static constexpr int kDbidW  = 16;  // DataFlit.DBID（注意：比 xscache 侧 12b 宽）
    static constexpr int kTxnW   = 12;

    static_assert(kNiw >= 7 && kNiw <= 11, "ZJParameters.scala:228");
    static_assert(kDw % 64 == 0);
};

// FlitType.scala:129-147 通道编码
inline constexpr int kFlitReq = 0;
inline constexpr int kFlitRsp = 1;
inline constexpr int kFlitDat = 2;
inline constexpr int kFlitSnp = 3;
inline constexpr int kFlitErq = 4;

// NodeIdBundle（Flit.scala:195-199）：nid 高位、aid 低位；router = nid << aidBits
template <class Cfg = ZjFlitCfg>
struct NodeIdT {
    uint8_t nid = 0;  // kNodeNidBits
    uint8_t aid = 0;  // kNodeAidBits

    constexpr uint32_t router() const { return uint32_t{nid} << Cfg::kNodeAidBits; }
    constexpr uint32_t id() const { return router() | aid; }

    bool operator==(const NodeIdT&) const = default;
};
using NodeId = NodeIdT<>;

// ReqFlit（Flit.scala:27-61；Dmt=true 即 HReqFlit，多 ReturnNID/ReturnTxnID）
// 零宽字段（RSVDC/SecID1/MECID/PBHA/MPAM）不例化。
template <class Cfg = ZjFlitCfg, bool Dmt = false>
struct ReqFlitT {
    static_assert(Cfg::kY == 0 && Cfg::kS == 0 && Cfg::kE == 0 && Cfg::kR == 0 && Cfg::kPb == 0 &&
                      Cfg::kM == 0,
                  "当前实现未例化零宽可选字段（RSVDC/SecID1/MECID/PBHA/MPAM），启用需先扩字段");

    bool     exp_comp_ack = false;  // ExpCompAck
    bool     excl         = false;  // Excl（def SnoopMe）
    bool     snp_attr     = false;  // SnpAttr（def DoDWT）
    uint8_t  mem_attr     = 0;      // MemAttr[3:0] = {allocate, cacheable, device, ewa}
    uint8_t  order        = 0;      // Order[1:0]
    uint64_t addr         = 0;      // Addr[kRaw-1:0]
    uint8_t  size         = 0;      // Size[2:0]；fullSize = size==6
    uint8_t  opcode       = 0;      // Opcode[6:0]
    uint16_t return_txn_id = 0;     // ReturnTxnID[11:0]，仅 Dmt（打包时才计入位宽）
    uint16_t return_nid    = 0;     // ReturnNID[niw-1:0]，仅 Dmt
    uint16_t txn_id       = 0;      // TxnID[11:0]
    uint16_t src_id       = 0;      // SrcID[niw-1:0]
    uint16_t tgt_id       = 0;      // TgtID[niw-1:0]
    uint8_t  qos          = 0;      // QoS[3:0]

    static constexpr bool kDmt   = Dmt;
    // qos4 + tgt/src(niw×2) + txn12 + opcode7 + size3 + addr(raw) + order2 + memattr4
    // + snpattr1 + excl1 + eca1（+Dmt: return_nid(niw) + return_txn_id(12)）
    static constexpr int kWidth = 35 + 2 * Cfg::kNiw + Cfg::kRaw + (Dmt ? Cfg::kNiw + 12 : 0);
    using Packed = std::array<uint64_t, (kWidth + 63) / 64>;

    bool full_size() const { return size == 6; }  // Flit.scala:60

    // LSB 起布局（asUInt 逆序）：QoS, TgtID, SrcID, TxnID, [ReturnNID, ReturnTxnID],
    // Opcode, Size, Addr, Order, MemAttr, SnpAttr, Excl, ExpCompAck
    Packed pack() const {
        Packed w{};
        int b = 0;
        setBits(w, b + 3, b, qos); b += 4;
        setBits(w, b + Cfg::kNiw - 1, b, tgt_id); b += Cfg::kNiw;
        setBits(w, b + Cfg::kNiw - 1, b, src_id); b += Cfg::kNiw;
        setBits(w, b + Cfg::kTxnW - 1, b, txn_id); b += Cfg::kTxnW;
        if constexpr (Dmt) {
            setBits(w, b + Cfg::kNiw - 1, b, return_nid); b += Cfg::kNiw;
            setBits(w, b + Cfg::kTxnW - 1, b, return_txn_id); b += Cfg::kTxnW;
        }
        setBits(w, b + 6, b, opcode); b += 7;
        setBits(w, b + 2, b, size); b += 3;
        setBits(w, b + Cfg::kRaw - 1, b, addr); b += Cfg::kRaw;
        setBits(w, b + 1, b, order); b += 2;
        setBits(w, b + 3, b, mem_attr); b += 4;
        setBits(w, b, b, snp_attr); b += 1;
        setBits(w, b, b, excl); b += 1;
        setBits(w, b, b, exp_comp_ack); b += 1;
        return w;
    }
    static ReqFlitT unpack(const Packed& w) {
        ReqFlitT f;
        int b = 0;
        f.qos = getBits(w, b + 3, b); b += 4;
        f.tgt_id = getBits(w, b + Cfg::kNiw - 1, b); b += Cfg::kNiw;
        f.src_id = getBits(w, b + Cfg::kNiw - 1, b); b += Cfg::kNiw;
        f.txn_id = getBits(w, b + Cfg::kTxnW - 1, b); b += Cfg::kTxnW;
        if constexpr (Dmt) {
            f.return_nid = getBits(w, b + Cfg::kNiw - 1, b); b += Cfg::kNiw;
            f.return_txn_id = getBits(w, b + Cfg::kTxnW - 1, b); b += Cfg::kTxnW;
        }
        f.opcode = getBits(w, b + 6, b); b += 7;
        f.size = getBits(w, b + 2, b); b += 3;
        f.addr = getBits(w, b + Cfg::kRaw - 1, b); b += Cfg::kRaw;
        f.order = getBits(w, b + 1, b); b += 2;
        f.mem_attr = getBits(w, b + 3, b); b += 4;
        f.snp_attr = getBits(w, b, b); b += 1;
        f.excl = getBits(w, b, b); b += 1;
        f.exp_comp_ack = getBits(w, b, b); b += 1;
        return f;
    }

    bool operator==(const ReqFlitT&) const = default;
};
using RReqFlit = ReqFlitT<ZjFlitCfg, false>;
using HReqFlit = ReqFlitT<ZjFlitCfg, true>;

// RespFlit（Flit.scala:65-82）。PGroupID/StashGroupID/TagGroupID 复用 DBID，
// DataPull 复用 FwdState——用同名字段即可，不单设。
template <class Cfg = ZjFlitCfg>
struct RespFlitT {
    uint16_t dbid      = 0;  // DBID[11:0]（注意与 DataFlit.DBID 16b 不同）
    uint8_t  c_busy    = 0;  // CBusy[2:0]
    uint8_t  fwd_state = 0;  // FwdState[2:0]
    uint8_t  resp      = 0;  // Resp[2:0]
    uint8_t  resp_err  = 0;  // RespErr[1:0]
    uint8_t  opcode    = 0;  // Opcode[4:0]
    uint16_t txn_id    = 0;
    uint16_t src_id    = 0;
    uint16_t tgt_id    = 0;
    uint8_t  qos       = 0;

    // qos4 + tgt/src(niw×2) + txn12 + opcode5 + resp_err2 + resp3 + fwd3 + cbusy3 + dbid12
    static constexpr int kWidth = 44 + 2 * Cfg::kNiw;
    using Packed = std::array<uint64_t, (kWidth + 63) / 64>;

    Packed pack() const {
        Packed w{};
        int b = 0;
        setBits(w, b + 3, b, qos); b += 4;
        setBits(w, b + Cfg::kNiw - 1, b, tgt_id); b += Cfg::kNiw;
        setBits(w, b + Cfg::kNiw - 1, b, src_id); b += Cfg::kNiw;
        setBits(w, b + Cfg::kTxnW - 1, b, txn_id); b += Cfg::kTxnW;
        setBits(w, b + 4, b, opcode); b += 5;
        setBits(w, b + 1, b, resp_err); b += 2;
        setBits(w, b + 2, b, resp); b += 3;
        setBits(w, b + 2, b, fwd_state); b += 3;
        setBits(w, b + 2, b, c_busy); b += 3;
        setBits(w, b + 11, b, dbid); b += 12;
        return w;
    }
    static RespFlitT unpack(const Packed& w) {
        RespFlitT f;
        int b = 0;
        f.qos = getBits(w, b + 3, b); b += 4;
        f.tgt_id = getBits(w, b + Cfg::kNiw - 1, b); b += Cfg::kNiw;
        f.src_id = getBits(w, b + Cfg::kNiw - 1, b); b += Cfg::kNiw;
        f.txn_id = getBits(w, b + Cfg::kTxnW - 1, b); b += Cfg::kTxnW;
        f.opcode = getBits(w, b + 4, b); b += 5;
        f.resp_err = getBits(w, b + 1, b); b += 2;
        f.resp = getBits(w, b + 2, b); b += 3;
        f.fwd_state = getBits(w, b + 2, b); b += 3;
        f.c_busy = getBits(w, b + 2, b); b += 3;
        f.dbid = getBits(w, b + 11, b); b += 12;
        return f;
    }

    bool operator==(const RespFlitT&) const = default;
};
using RespFlit = RespFlitT<>;

// SnoopFlit（Flit.scala:84-102）。VMIDExt 复用 FwdTxnID、PBHA 复用 FwdNID。
template <class Cfg = ZjFlitCfg>
struct SnoopFlitT {
    static_assert(Cfg::kE == 0 && Cfg::kM == 0, "SnoopFlit 零宽字段 MECID/MPAM 未例化");

    bool     ret_to_src      = false;  // RetToSrc
    bool     do_not_go_to_sd = false;  // DoNotGoToSD
    uint64_t addr            = 0;      // Addr[kSaw-1:0]
    uint8_t  opcode          = 0;      // Opcode[4:0]
    uint16_t fwd_txn_id      = 0;      // FwdTxnID[11:0]
    uint16_t fwd_nid         = 0;      // FwdNID[niw-1:0]
    uint16_t txn_id          = 0;
    uint16_t src_id          = 0;
    uint16_t tgt_id          = 0;
    uint8_t  qos             = 0;

    // qos4 + tgt/src/fwd(niw×3) + txn12 + fwdtxn12 + opcode5 + addr(saw) + dngsd1 + rts1
    static constexpr int kWidth = 35 + 3 * Cfg::kNiw + Cfg::kSaw;
    using Packed = std::array<uint64_t, (kWidth + 63) / 64>;

    Packed pack() const {
        Packed w{};
        int b = 0;
        setBits(w, b + 3, b, qos); b += 4;
        setBits(w, b + Cfg::kNiw - 1, b, tgt_id); b += Cfg::kNiw;
        setBits(w, b + Cfg::kNiw - 1, b, src_id); b += Cfg::kNiw;
        setBits(w, b + Cfg::kTxnW - 1, b, txn_id); b += Cfg::kTxnW;
        setBits(w, b + Cfg::kNiw - 1, b, fwd_nid); b += Cfg::kNiw;
        setBits(w, b + Cfg::kTxnW - 1, b, fwd_txn_id); b += Cfg::kTxnW;
        setBits(w, b + 4, b, opcode); b += 5;
        setBits(w, b + Cfg::kSaw - 1, b, addr); b += Cfg::kSaw;
        setBits(w, b, b, do_not_go_to_sd); b += 1;
        setBits(w, b, b, ret_to_src); b += 1;
        return w;
    }
    static SnoopFlitT unpack(const Packed& w) {
        SnoopFlitT f;
        int b = 0;
        f.qos = getBits(w, b + 3, b); b += 4;
        f.tgt_id = getBits(w, b + Cfg::kNiw - 1, b); b += Cfg::kNiw;
        f.src_id = getBits(w, b + Cfg::kNiw - 1, b); b += Cfg::kNiw;
        f.txn_id = getBits(w, b + Cfg::kTxnW - 1, b); b += Cfg::kTxnW;
        f.fwd_nid = getBits(w, b + Cfg::kNiw - 1, b); b += Cfg::kNiw;
        f.fwd_txn_id = getBits(w, b + Cfg::kTxnW - 1, b); b += Cfg::kTxnW;
        f.opcode = getBits(w, b + 4, b); b += 5;
        f.addr = getBits(w, b + Cfg::kSaw - 1, b); b += Cfg::kSaw;
        f.do_not_go_to_sd = getBits(w, b, b); b += 1;
        f.ret_to_src = getBits(w, b, b); b += 1;
        return f;
    }

    bool operator==(const SnoopFlitT&) const = default;
};
using SnoopFlit = SnoopFlitT<>;

// DataFlit（Flit.scala:104-127）。MECID 复用 DBID、FwdState 复用 DataSource。
// ★ DBID 16b——xscache 侧只有 12b，适配层做零扩展/截断（见 xs_flit.h dbid 助手）。
template <class Cfg = ZjFlitCfg>
struct DataFlitT {
    static_assert(Cfg::kY == 0, "DataFlit 零宽字段 RSVDC 未例化");

    std::array<uint64_t, Cfg::kDw / 64> data{};  // Data[kDw-1:0]，低位在前
    uint64_t be          = 0;   // BE[kBew-1:0]
    uint8_t  data_id     = 0;   // DataID[1:0]
    uint16_t dbid        = 0;   // DBID[15:0] ★
    uint8_t  c_busy      = 0;   // CBusy[2:0]
    uint8_t  data_source = 0;   // DataSource[7:0]
    uint8_t  resp        = 0;   // Resp[2:0]
    uint8_t  resp_err    = 0;   // RespErr[1:0]
    uint8_t  opcode      = 0;   // Opcode[3:0]
    uint16_t home_nid    = 0;   // HomeNID[niw-1:0]
    uint16_t txn_id      = 0;
    uint16_t src_id      = 0;
    uint16_t tgt_id      = 0;
    uint8_t  qos         = 0;

    // qos4 + tgt/src/home(niw×3) + txn12 + opcode4 + resp_err2 + resp3 + ds8 + cbusy3
    // + dbid16 + data_id2 + be(bew) + data(dw)
    static constexpr int kWidth = 54 + 3 * Cfg::kNiw + Cfg::kBew + Cfg::kDw;
    using Packed = std::array<uint64_t, (kWidth + 63) / 64>;

    Packed pack() const {
        Packed w{};
        int b = 0;
        setBits(w, b + 3, b, qos); b += 4;
        setBits(w, b + Cfg::kNiw - 1, b, tgt_id); b += Cfg::kNiw;
        setBits(w, b + Cfg::kNiw - 1, b, src_id); b += Cfg::kNiw;
        setBits(w, b + Cfg::kTxnW - 1, b, txn_id); b += Cfg::kTxnW;
        setBits(w, b + Cfg::kNiw - 1, b, home_nid); b += Cfg::kNiw;
        setBits(w, b + 3, b, opcode); b += 4;
        setBits(w, b + 1, b, resp_err); b += 2;
        setBits(w, b + 2, b, resp); b += 3;
        setBits(w, b + 7, b, data_source); b += 8;
        setBits(w, b + 2, b, c_busy); b += 3;
        setBits(w, b + Cfg::kDbidW - 1, b, dbid); b += Cfg::kDbidW;
        setBits(w, b + 1, b, data_id); b += 2;
        setBits(w, b + Cfg::kBew - 1, b, be); b += Cfg::kBew;
        setWide(w, b, data); b += Cfg::kDw;
        return w;
    }
    static DataFlitT unpack(const Packed& w) {
        DataFlitT f;
        int b = 0;
        f.qos = getBits(w, b + 3, b); b += 4;
        f.tgt_id = getBits(w, b + Cfg::kNiw - 1, b); b += Cfg::kNiw;
        f.src_id = getBits(w, b + Cfg::kNiw - 1, b); b += Cfg::kNiw;
        f.txn_id = getBits(w, b + Cfg::kTxnW - 1, b); b += Cfg::kTxnW;
        f.home_nid = getBits(w, b + Cfg::kNiw - 1, b); b += Cfg::kNiw;
        f.opcode = getBits(w, b + 3, b); b += 4;
        f.resp_err = getBits(w, b + 1, b); b += 2;
        f.resp = getBits(w, b + 2, b); b += 3;
        f.data_source = getBits(w, b + 7, b); b += 8;
        f.c_busy = getBits(w, b + 2, b); b += 3;
        f.dbid = getBits(w, b + Cfg::kDbidW - 1, b); b += Cfg::kDbidW;
        f.data_id = getBits(w, b + 1, b); b += 2;
        f.be = getBits(w, b + Cfg::kBew - 1, b); b += Cfg::kBew;
        f.data = getWide<Cfg::kDw / 64>(w, b); b += Cfg::kDw;
        return f;
    }

    bool operator==(const DataFlitT&) const = default;
};
using DataFlit = DataFlitT<>;

// 各通道环位宽（HasZJParams，ZJParameters.scala:293-299）：
// REQ=RReq，RSP=Resp，DAT=Data，HRQ=max(HReq, Snoop)
inline constexpr int kRingReqBits = RReqFlit::kWidth;                        // 105
inline constexpr int kRingRspBits = RespFlit::kWidth;                        // 66
inline constexpr int kRingDatBits = DataFlit::kWidth;                        // 375
inline constexpr int kRingHrqBits = HReqFlit::kWidth > SnoopFlit::kWidth ? HReqFlit::kWidth
                                                                         : SnoopFlit::kWidth;  // 128

}  // namespace zj::chi
