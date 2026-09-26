#pragma once

// XscChiAdapter：xscache CHI flit ↔ zhujiang flit 的字段级重映射（纯组合，
// RTL 中就是纯连线），对齐 ZhuJiangBridge.scala:152-252
// （XSCache/src/test/scala/ZhuJiangBridge.scala，package zhujiang）的
// mapReq/mapRsp/mapDat/mapSnp。映射函数同时以自由函数形式导出（trace 解码、
// 测试与后续 DPI 薄壳共用）。
//
// 语义要点（逐行核自 RTL 源）：
//   - 目标 flit 先全零再逐字段赋值：未列字段恒 0（CHIRSP rx 方向 tgt_id 恒 0
//     ——mapRsp(RespFlit→CHIRSP) 不回填，ZhuJiangBridge.scala:187-199）
//   - CHIREQ.rsvdc/mpam_part_id 被 mapReq 读但落入 zhujiang 零宽字段
//     （RSVDC/PBHA/MPAM），丢弃；ReqFlit.ReturnNID/ReturnTxnID 仅 Dmt 变体
//     有，CC inject 是 RReqFlit（.foreach 空转）
//   - DBID 宽度适配：xs 12b ↔ zj DAT 16b（零扩展/截断，xs_flit.h dbid 助手）；
//     RSP 两侧同为 12b
//   - DataSource：xs 4b ↔ zj 8b（零扩展/截断）；mapDat 里 FwdState:= 与
//     DataSource:= 是同一物理字段（Flit.scala:127 def FwdState = DataSource），
//     chisel 后赋值获胜 → 净效果 DataSource:=dataSource
//   - CHISNP.ns 恒 false（xs flit 无此字段，天然满足）

#include <cstdint>

#include "model/flit/xs_flit.h"
#include "model/flit/zj_flit.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/dec.h"

namespace zj::xs {

using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Dec;

// ---------------- 映射函数（ZhuJiangBridge.scala:152-252 逐字段转写）----------------

// mapReq（:152-171）：CHIREQ → ReqFlit
inline chi::RReqFlit mapReq(const CHIREQ& s) {
    chi::RReqFlit d{};
    d.qos          = s.qos;
    d.tgt_id       = s.tgt_id;
    d.src_id       = s.src_id;
    d.txn_id       = s.txn_id;
    d.opcode       = s.opcode & 0x7F;
    d.size         = s.size & 0x7;
    d.addr         = s.addr;
    d.order        = s.order & 0x3;
    d.mem_attr     = s.mem_attr();  // {allocate, cacheable, device, ewa}
    d.snp_attr     = s.snp_attr;
    d.excl         = s.snoop_me;  // excl 即 snoop_me（Message.scala:467）
    d.exp_comp_ack = s.exp_comp_ack;
    return d;
}

// mapRsp（:173-185）：CHIRSP → RespFlit
inline chi::RespFlit mapRspZj(const CHIRSP& s) {
    chi::RespFlit d{};
    d.qos       = s.qos;
    d.tgt_id    = s.tgt_id;
    d.src_id    = s.src_id;
    d.txn_id    = s.txn_id;
    d.opcode    = s.opcode & 0x1F;
    d.resp_err  = s.resp_err & 0x3;
    d.resp      = s.resp & 0x7;
    d.fwd_state = s.fwd_state & 0x7;
    d.c_busy    = s.c_busy & 0x7;
    d.dbid      = s.dbid & 0x0FFF;
    return d;
}

// mapRsp（:187-199）：RespFlit → CHIRSP（tgt_id 不回填，恒 0；pCrdType 恒 0 无字段）
inline CHIRSP mapRspXs(const chi::RespFlit& s) {
    CHIRSP d{};
    d.qos       = s.qos;
    d.src_id    = s.src_id;
    d.txn_id    = s.txn_id;
    d.opcode    = s.opcode & 0x1F;
    d.resp_err  = s.resp_err & 0x3;
    d.resp      = s.resp & 0x7;
    d.fwd_state = s.fwd_state & 0x7;
    d.c_busy    = s.c_busy & 0x7;
    d.dbid      = s.dbid & 0x0FFF;
    return d;
}

// mapDat（:201-218）：CHIDAT → DataFlit
inline chi::DataFlit mapDatZj(const CHIDAT& s) {
    chi::DataFlit d{};
    d.qos         = s.qos;
    d.tgt_id      = s.tgt_id;
    d.src_id      = s.src_id;
    d.txn_id      = s.txn_id;
    d.home_nid    = s.home_nid;
    d.opcode      = s.opcode & 0xF;
    d.resp_err    = s.resp_err & 0x3;
    d.resp        = s.resp & 0x7;
    d.data_source = s.data_source & 0xF;  // 4b→8b 零扩展；FwdState 赋值被其覆盖
    d.c_busy      = s.c_busy & 0x7;
    d.dbid        = dbidXsToZj(s.dbid);
    d.data_id     = s.data_id & 0x3;
    d.data        = s.data;
    d.be          = s.be;
    return d;
}

// mapDat（:220-238）：DataFlit → CHIDAT（ccID/poison 恒 0 无字段）
inline CHIDAT mapDatXs(const chi::DataFlit& s) {
    CHIDAT d{};
    d.qos         = s.qos;
    d.tgt_id      = s.tgt_id;
    d.src_id      = s.src_id;
    d.txn_id      = s.txn_id;
    d.home_nid    = s.home_nid;
    d.opcode      = s.opcode & 0xF;
    d.resp_err    = s.resp_err & 0x3;
    d.resp        = s.resp & 0x7;
    d.data_source = s.data_source & 0xF;  // 8b→4b 截断
    d.c_busy      = s.c_busy & 0x7;
    d.dbid        = dbidZjToXs(s.dbid);
    d.data_id     = s.data_id & 0x3;
    d.data        = s.data;
    d.be          = s.be;
    return d;
}

// mapSnp（:240-252）：SnoopFlit → CHISNP（ns 恒 false 无字段）
inline CHISNP mapSnp(const chi::SnoopFlit& s) {
    CHISNP d{};
    d.qos            = s.qos;
    d.src_id         = s.src_id;
    d.txn_id         = s.txn_id;
    d.fwd_nid        = s.fwd_nid;
    d.fwd_txn_id     = s.fwd_txn_id & 0x0FFF;
    d.opcode         = s.opcode & 0x1F;
    d.addr           = s.addr;
    d.do_not_go_to_sd = s.do_not_go_to_sd;
    d.ret_to_src     = s.ret_to_src;
    return d;
}

// ---------------- XscChiAdapter 模块 ----------------

// 端口命名对齐 connectCHIToZhuJiang（:77-150）：chi_* 接 L2 DecoupledPortIO，
// zj_* 接 CcSocket l2_*（SocketDevSide io.icn；rx=inject、tx=eject）。
class XscChiAdapter : public wolvicmod::Module {
public:
    // ---- L2 侧（xscache flit）----
    IN(Dec<CHIREQ>, chi_tx_req);
    OUT(bool, chi_tx_req_rdy);
    IN(Dec<CHIRSP>, chi_tx_rsp);
    OUT(bool, chi_tx_rsp_rdy);
    IN(Dec<CHIDAT>, chi_tx_dat);
    OUT(bool, chi_tx_dat_rdy);
    OUT(Dec<CHIRSP>, chi_rx_rsp);
    IN(bool, chi_rx_rsp_rdy);
    OUT(Dec<CHIDAT>, chi_rx_dat);
    IN(bool, chi_rx_dat_rdy);
    OUT(Dec<CHISNP>, chi_rx_snp);
    IN(bool, chi_rx_snp_rdy);

    // ---- socket 侧（zhujiang flit，接 CcSocket l2_*）----
    OUT(Dec<chi::RReqFlit>, zj_rx_req);
    IN(bool, zj_rx_req_rdy);
    OUT(Dec<chi::RespFlit>, zj_rx_rsp);
    IN(bool, zj_rx_rsp_rdy);
    OUT(Dec<chi::DataFlit>, zj_rx_dat);
    IN(bool, zj_rx_dat_rdy);
    IN(Dec<chi::RespFlit>, zj_tx_rsp);
    OUT(bool, zj_tx_rsp_rdy);
    IN(Dec<chi::DataFlit>, zj_tx_dat);
    OUT(bool, zj_tx_dat_rdy);
    IN(Dec<chi::SnoopFlit>, zj_tx_snp);
    OUT(bool, zj_tx_snp_rdy);

    XscChiAdapter() {
        // L2 → ZJ（mapReq/mapRsp/mapDat）+ ready 直通
        zj_rx_req.assign().reads(chi_tx_req) = [](auto src) {
            auto [chi_tx_req] = src;
            Dec<chi::RReqFlit> d;
            d.valid = chi_tx_req.valid;
            d.bits  = mapReq(chi_tx_req.bits);
            return d;
        };
        chi_tx_req_rdy = zj_rx_req_rdy;
        zj_rx_rsp.assign().reads(chi_tx_rsp) = [](auto src) {
            auto [chi_tx_rsp] = src;
            Dec<chi::RespFlit> d;
            d.valid = chi_tx_rsp.valid;
            d.bits  = mapRspZj(chi_tx_rsp.bits);
            return d;
        };
        chi_tx_rsp_rdy = zj_rx_rsp_rdy;
        zj_rx_dat.assign().reads(chi_tx_dat) = [](auto src) {
            auto [chi_tx_dat] = src;
            Dec<chi::DataFlit> d;
            d.valid = chi_tx_dat.valid;
            d.bits  = mapDatZj(chi_tx_dat.bits);
            return d;
        };
        chi_tx_dat_rdy = zj_rx_dat_rdy;
        // ZJ → L2（mapRsp/mapDat/mapSnp）+ ready 直通
        chi_rx_rsp.assign().reads(zj_tx_rsp) = [](auto src) {
            auto [zj_tx_rsp] = src;
            Dec<CHIRSP> d;
            d.valid = zj_tx_rsp.valid;
            d.bits  = mapRspXs(zj_tx_rsp.bits);
            return d;
        };
        zj_tx_rsp_rdy = chi_rx_rsp_rdy;
        chi_rx_dat.assign().reads(zj_tx_dat) = [](auto src) {
            auto [zj_tx_dat] = src;
            Dec<CHIDAT> d;
            d.valid = zj_tx_dat.valid;
            d.bits  = mapDatXs(zj_tx_dat.bits);
            return d;
        };
        zj_tx_dat_rdy = chi_rx_dat_rdy;
        chi_rx_snp.assign().reads(zj_tx_snp) = [](auto src) {
            auto [zj_tx_snp] = src;
            Dec<CHISNP> d;
            d.valid = zj_tx_snp.valid;
            d.bits  = mapSnp(zj_tx_snp.bits);
            return d;
        };
        zj_tx_snp_rdy = chi_rx_snp_rdy;
    }
};

}  // namespace zj::xs
