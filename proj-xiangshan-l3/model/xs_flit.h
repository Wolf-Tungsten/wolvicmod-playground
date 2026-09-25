#pragma once

// xscache CHI flit 四件套：CHIREQ/CHIRSP/CHIDAT/CHISNP（L2↔ZhuJiang 边界
// DecoupledPortIO 载荷，xscache/chi/Message.scala + LinkLayer.scala:99-102）
// 的位级 C++ 镜像。
//
// ★ 字段集 = 生成 RTL 实际端口（build/rtl/CoupledL2.sv io_decoupledCHI_*），
// 即 ZhuJiangBridge（XSCache/src/test/scala/ZhuJiangBridge.scala:152-252）
// mapReq/mapRsp/mapDat/mapSnp 真正读写的字段；CHI Bundle 里其余字段在该集成中
// 被常量绑定或丢进 zhujiang 零宽字段，elaboration 时被裁剪，不穿越边界：
//   - CHIREQ 裁掉：returnNID/stashNIDValid/returnTxnID/ns/likelyshared/allowRetry/
//     pCrdType/lpIDWithPadding/tagOp/traceTag（mpam 只保留 partID 9b，
//     rsvdc 保留——被 mapReq 读但落入 zhujiang 零宽字段，无语义）
//   - CHIRSP：rx 方向 tgtID 恒 0（mapRsp 不回填，ZhuJiangBridge.scala:187-199），
//     只 tx 方向存在；struct 取双向并集，注释标注
//   - CHIDAT 裁掉：ccID/tagOp/tag/tu/traceTag/rsvdc（dataCheck/poison 配置关闭，
//     ZhuJiangBridge.scala:87-93 require）
//   - CHISNP 裁掉：ns(恒 false)/traceTag/mpam
// 参数化同 zj_flit.h：编译期 config traits，默认 = kunminghu-v3 锁定值
// （CHI Issue E.b——Makefile ISSUE=E.b，ZhuJiang 只支持 E.b；ADDR=48）。
// pack() 布局 = 本地约定（声明序、先声明占高位），仅供 DPI 薄壳/trace 解码用。

#include <array>
#include <cstddef>
#include <cstdint>

#include "model/bit_pack.h"

namespace zj::xs {

// xscache CHI Eb_CONFIG（Message.scala:266-281）+ kunminghu-v3 覆盖
struct XsChiCfg {
    static constexpr int kNidW  = 11;   // NODEID_WIDTH（Eb）
    static constexpr int kTxnW  = 12;   // TXNID_WIDTH（Eb）
    static constexpr int kAddrW = 48;   // CHIAddrWidthKey 默认
    static constexpr int kDataW = 256;  // DATA_WIDTH
    static constexpr int kBeW   = kDataW / 8;
    static constexpr int kDbidW = kTxnW;  // DBID_WIDTH = TXNID_WIDTH = 12 ★ zhujiang DAT 侧是 16b

    static_assert(kAddrW >= 44 && kAddrW <= 52, "Message.scala:376");
    static_assert(kDataW % 64 == 0);
};

// DBID 宽度适配（ZhuJiangBridge.scala:214,232）：12→16 零扩展；16→12 截断
//（Chisel := 宽赋窄丢高位；zhujiang DBID 值域不超 12 位语义，见层次文档 §7）
constexpr uint16_t dbidXsToZj(uint16_t v) { return v & 0x0FFF; }
constexpr uint16_t dbidZjToXs(uint16_t v) { return v & 0x0FFF; }
constexpr bool     dbidFitsXs(uint16_t v) { return v <= 0x0FFF; }

// CHIREQ（仅 tx 方向存在）
template <class Cfg = XsChiCfg>
struct CHIREQT {
    uint8_t  qos = 0;             // [3:0]
    uint16_t tgt_id = 0;          // [kNidW-1:0]
    uint16_t src_id = 0;
    uint16_t txn_id = 0;          // [kTxnW-1:0]
    uint8_t  opcode = 0;          // [6:0]
    uint8_t  size = 0;            // [2:0]
    uint64_t addr = 0;            // [kAddrW-1:0]
    uint8_t  order = 0;           // [1:0]
    bool     mem_attr_allocate = false;  // MemAttr 子字段按 SV 端口逐位保留
    bool     mem_attr_cacheable = false;
    bool     mem_attr_device = false;
    bool     mem_attr_ewa = false;
    bool     snp_attr = false;
    bool     snoop_me = false;      // 即 excl（Message.scala:467）
    bool     exp_comp_ack = false;
    uint16_t mpam_part_id = 0;    // [8:0]；被 mapReq 读但落入 zhujiang 零宽字段
    uint8_t  rsvdc = 0;           // [3:0]；同上

    // zhujiang 侧 MemAttr 位序：{allocate, cacheable, device, ewa}（ZhuJiangBridge.scala:162）
    uint8_t mem_attr() const {
        return uint8_t(mem_attr_allocate) << 3 | uint8_t(mem_attr_cacheable) << 2 |
               uint8_t(mem_attr_device) << 1 | uint8_t(mem_attr_ewa);
    }

    // qos4 + tgt/src(nid×2) + txn + opcode7 + size3 + addr + order2 + memattr4
    // + snpattr1 + snoopme1 + eca1 + mpam_part9 + rsvdc4
    static constexpr int kWidth = 36 + 2 * Cfg::kNidW + Cfg::kTxnW + Cfg::kAddrW;  // 118
    using Packed = std::array<uint64_t, (kWidth + 63) / 64>;

    Packed pack() const {
        Packed w{};
        int b = 0;
        setBits(w, b + 3, b, rsvdc); b += 4;
        setBits(w, b + 8, b, mpam_part_id); b += 9;
        setBits(w, b, b, exp_comp_ack); b += 1;
        setBits(w, b, b, snoop_me); b += 1;
        setBits(w, b, b, snp_attr); b += 1;
        setBits(w, b + 3, b, mem_attr()); b += 4;
        setBits(w, b + 1, b, order); b += 2;
        setBits(w, b + Cfg::kAddrW - 1, b, addr); b += Cfg::kAddrW;
        setBits(w, b + 2, b, size); b += 3;
        setBits(w, b + 6, b, opcode); b += 7;
        setBits(w, b + Cfg::kTxnW - 1, b, txn_id); b += Cfg::kTxnW;
        setBits(w, b + Cfg::kNidW - 1, b, src_id); b += Cfg::kNidW;
        setBits(w, b + Cfg::kNidW - 1, b, tgt_id); b += Cfg::kNidW;
        setBits(w, b + 3, b, qos); b += 4;
        return w;
    }
    static CHIREQT unpack(const Packed& w) {
        CHIREQT f;
        int b = 0;
        f.rsvdc = getBits(w, b + 3, b); b += 4;
        f.mpam_part_id = getBits(w, b + 8, b); b += 9;
        f.exp_comp_ack = getBits(w, b, b); b += 1;
        f.snoop_me = getBits(w, b, b); b += 1;
        f.snp_attr = getBits(w, b, b); b += 1;
        const uint8_t ma = getBits(w, b + 3, b); b += 4;
        f.mem_attr_allocate = ma >> 3 & 1;
        f.mem_attr_cacheable = ma >> 2 & 1;
        f.mem_attr_device = ma >> 1 & 1;
        f.mem_attr_ewa = ma & 1;
        f.order = getBits(w, b + 1, b); b += 2;
        f.addr = getBits(w, b + Cfg::kAddrW - 1, b); b += Cfg::kAddrW;
        f.size = getBits(w, b + 2, b); b += 3;
        f.opcode = getBits(w, b + 6, b); b += 7;
        f.txn_id = getBits(w, b + Cfg::kTxnW - 1, b); b += Cfg::kTxnW;
        f.src_id = getBits(w, b + Cfg::kNidW - 1, b); b += Cfg::kNidW;
        f.tgt_id = getBits(w, b + Cfg::kNidW - 1, b); b += Cfg::kNidW;
        f.qos = getBits(w, b + 3, b); b += 4;
        return f;
    }

    bool operator==(const CHIREQT&) const = default;
};
using CHIREQ = CHIREQT<>;

// CHIRSP。tgt_id 仅 tx 方向（rx 方向 RTL 恒 0，被裁剪）。
template <class Cfg = XsChiCfg>
struct CHIRSPT {
    uint8_t  qos = 0;
    uint16_t tgt_id = 0;   // 仅 tx 方向有意义
    uint16_t src_id = 0;
    uint16_t txn_id = 0;
    uint8_t  opcode = 0;    // [4:0]
    uint8_t  resp_err = 0;  // [1:0]
    uint8_t  resp = 0;      // [2:0]
    uint8_t  fwd_state = 0; // [2:0]
    uint8_t  c_busy = 0;    // [2:0]
    uint16_t dbid = 0;      // [11:0] ★ zhujiang RespFlit.DBID 同为 12b，DAT 才不等宽

    // qos4 + tgt/src(nid×2) + txn + opcode5 + resp_err2 + resp3 + fwd3 + cbusy3 + dbid
    static constexpr int kWidth = 20 + 2 * Cfg::kNidW + Cfg::kTxnW + Cfg::kDbidW;  // 66
    using Packed = std::array<uint64_t, (kWidth + 63) / 64>;

    Packed pack() const {
        Packed w{};
        int b = 0;
        setBits(w, b + Cfg::kDbidW - 1, b, dbid); b += Cfg::kDbidW;
        setBits(w, b + 2, b, c_busy); b += 3;
        setBits(w, b + 2, b, fwd_state); b += 3;
        setBits(w, b + 2, b, resp); b += 3;
        setBits(w, b + 1, b, resp_err); b += 2;
        setBits(w, b + 4, b, opcode); b += 5;
        setBits(w, b + Cfg::kTxnW - 1, b, txn_id); b += Cfg::kTxnW;
        setBits(w, b + Cfg::kNidW - 1, b, src_id); b += Cfg::kNidW;
        setBits(w, b + Cfg::kNidW - 1, b, tgt_id); b += Cfg::kNidW;
        setBits(w, b + 3, b, qos); b += 4;
        return w;
    }
    static CHIRSPT unpack(const Packed& w) {
        CHIRSPT f;
        int b = 0;
        f.dbid = getBits(w, b + Cfg::kDbidW - 1, b); b += Cfg::kDbidW;
        f.c_busy = getBits(w, b + 2, b); b += 3;
        f.fwd_state = getBits(w, b + 2, b); b += 3;
        f.resp = getBits(w, b + 2, b); b += 3;
        f.resp_err = getBits(w, b + 1, b); b += 2;
        f.opcode = getBits(w, b + 4, b); b += 5;
        f.txn_id = getBits(w, b + Cfg::kTxnW - 1, b); b += Cfg::kTxnW;
        f.src_id = getBits(w, b + Cfg::kNidW - 1, b); b += Cfg::kNidW;
        f.tgt_id = getBits(w, b + Cfg::kNidW - 1, b); b += Cfg::kNidW;
        f.qos = getBits(w, b + 3, b); b += 4;
        return f;
    }

    bool operator==(const CHIRSPT&) const = default;
};
using CHIRSP = CHIRSPT<>;

// CHIDAT
template <class Cfg = XsChiCfg>
struct CHIDATT {
    uint8_t  qos = 0;
    uint16_t tgt_id = 0;
    uint16_t src_id = 0;
    uint16_t txn_id = 0;
    uint16_t home_nid = 0;
    uint8_t  opcode = 0;      // [3:0]
    uint8_t  resp_err = 0;    // [1:0]
    uint8_t  resp = 0;        // [2:0]
    uint8_t  data_source = 0; // [3:0]（Eb DATASOURCE_WIDTH=4；zhujiang 侧 8b，零扩展）
    uint8_t  c_busy = 0;      // [2:0]
    uint16_t dbid = 0;        // [11:0] ★ zhujiang DataFlit.DBID 16b
    uint8_t  data_id = 0;     // [1:0]
    uint64_t be = 0;          // [kBeW-1:0]
    std::array<uint64_t, Cfg::kDataW / 64> data{};

    // qos4 + tgt/src/home(nid×3) + txn + opcode4 + resp_err2 + resp3 + ds4 + cbusy3
    // + dbid + data_id2 + be + data
    static constexpr int kWidth = 22 + 3 * Cfg::kNidW + Cfg::kTxnW + Cfg::kDbidW + Cfg::kBeW +
                                  Cfg::kDataW;  // 367
    using Packed = std::array<uint64_t, (kWidth + 63) / 64>;

    Packed pack() const {
        Packed w{};
        int b = 0;
        setWide(w, b, data); b += Cfg::kDataW;
        setBits(w, b + Cfg::kBeW - 1, b, be); b += Cfg::kBeW;
        setBits(w, b + 1, b, data_id); b += 2;
        setBits(w, b + Cfg::kDbidW - 1, b, dbid); b += Cfg::kDbidW;
        setBits(w, b + 2, b, c_busy); b += 3;
        setBits(w, b + 3, b, data_source); b += 4;
        setBits(w, b + 2, b, resp); b += 3;
        setBits(w, b + 1, b, resp_err); b += 2;
        setBits(w, b + 3, b, opcode); b += 4;
        setBits(w, b + Cfg::kNidW - 1, b, home_nid); b += Cfg::kNidW;
        setBits(w, b + Cfg::kTxnW - 1, b, txn_id); b += Cfg::kTxnW;
        setBits(w, b + Cfg::kNidW - 1, b, src_id); b += Cfg::kNidW;
        setBits(w, b + Cfg::kNidW - 1, b, tgt_id); b += Cfg::kNidW;
        setBits(w, b + 3, b, qos); b += 4;
        return w;
    }
    static CHIDATT unpack(const Packed& w) {
        CHIDATT f;
        int b = 0;
        f.data = getWide<Cfg::kDataW / 64>(w, b); b += Cfg::kDataW;
        f.be = getBits(w, b + Cfg::kBeW - 1, b); b += Cfg::kBeW;
        f.data_id = getBits(w, b + 1, b); b += 2;
        f.dbid = getBits(w, b + Cfg::kDbidW - 1, b); b += Cfg::kDbidW;
        f.c_busy = getBits(w, b + 2, b); b += 3;
        f.data_source = getBits(w, b + 3, b); b += 4;
        f.resp = getBits(w, b + 2, b); b += 3;
        f.resp_err = getBits(w, b + 1, b); b += 2;
        f.opcode = getBits(w, b + 3, b); b += 4;
        f.home_nid = getBits(w, b + Cfg::kNidW - 1, b); b += Cfg::kNidW;
        f.txn_id = getBits(w, b + Cfg::kTxnW - 1, b); b += Cfg::kTxnW;
        f.src_id = getBits(w, b + Cfg::kNidW - 1, b); b += Cfg::kNidW;
        f.tgt_id = getBits(w, b + Cfg::kNidW - 1, b); b += Cfg::kNidW;
        f.qos = getBits(w, b + 3, b); b += 4;
        return f;
    }

    bool operator==(const CHIDATT&) const = default;
};
using CHIDAT = CHIDATT<>;

// CHISNP（仅 rx 方向存在）
template <class Cfg = XsChiCfg>
struct CHISNPT {
    uint8_t  qos = 0;
    uint16_t src_id = 0;
    uint16_t txn_id = 0;
    uint16_t fwd_nid = 0;
    uint16_t fwd_txn_id = 0;    // [kTxnW-1:0]
    uint8_t  opcode = 0;        // [4:0]
    uint64_t addr = 0;          // [kAddrW-4:0]（SNP_ADDR_WIDTH = ADDR_WIDTH-3）
    bool     do_not_go_to_sd = false;
    bool     ret_to_src = false;

    static constexpr int kSaw = Cfg::kAddrW - 3;
    // qos4 + src/fwd(nid×2) + txn + fwdtxn + opcode5 + addr(saw) + dngsd1 + rts1
    static constexpr int kWidth = 11 + 2 * Cfg::kNidW + 2 * Cfg::kTxnW + kSaw;  // 102
    using Packed = std::array<uint64_t, (kWidth + 63) / 64>;

    Packed pack() const {
        Packed w{};
        int b = 0;
        setBits(w, b, b, ret_to_src); b += 1;
        setBits(w, b, b, do_not_go_to_sd); b += 1;
        setBits(w, b + kSaw - 1, b, addr); b += kSaw;
        setBits(w, b + 4, b, opcode); b += 5;
        setBits(w, b + Cfg::kTxnW - 1, b, fwd_txn_id); b += Cfg::kTxnW;
        setBits(w, b + Cfg::kNidW - 1, b, fwd_nid); b += Cfg::kNidW;
        setBits(w, b + Cfg::kTxnW - 1, b, txn_id); b += Cfg::kTxnW;
        setBits(w, b + Cfg::kNidW - 1, b, src_id); b += Cfg::kNidW;
        setBits(w, b + 3, b, qos); b += 4;
        return w;
    }
    static CHISNPT unpack(const Packed& w) {
        CHISNPT f;
        int b = 0;
        f.ret_to_src = getBits(w, b, b); b += 1;
        f.do_not_go_to_sd = getBits(w, b, b); b += 1;
        f.addr = getBits(w, b + kSaw - 1, b); b += kSaw;
        f.opcode = getBits(w, b + 4, b); b += 5;
        f.fwd_txn_id = getBits(w, b + Cfg::kTxnW - 1, b); b += Cfg::kTxnW;
        f.fwd_nid = getBits(w, b + Cfg::kNidW - 1, b); b += Cfg::kNidW;
        f.txn_id = getBits(w, b + Cfg::kTxnW - 1, b); b += Cfg::kTxnW;
        f.src_id = getBits(w, b + Cfg::kNidW - 1, b); b += Cfg::kNidW;
        f.qos = getBits(w, b + 3, b); b += 4;
        return f;
    }

    bool operator==(const CHISNPT&) const = default;
};
using CHISNP = CHISNPT<>;

}  // namespace zj::xs
