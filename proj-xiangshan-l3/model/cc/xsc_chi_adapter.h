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
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/dec.h"

namespace zj::xs {

using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Dec;

// ---------------- 映射函数（ZhuJiangBridge.scala:152-252 逐字段转写）----------------

chi::RReqFlit mapReq(const CHIREQ& s);       // mapReq（:152-171）：CHIREQ → ReqFlit
chi::RespFlit mapRspZj(const CHIRSP& s);     // mapRsp（:173-185）：CHIRSP → RespFlit
CHIRSP mapRspXs(const chi::RespFlit& s);     // mapRsp（:187-199）：RespFlit → CHIRSP（tgt 恒 0）
chi::DataFlit mapDatZj(const CHIDAT& s);     // mapDat（:201-218）：CHIDAT → DataFlit
CHIDAT mapDatXs(const chi::DataFlit& s);     // mapDat（:220-238）：DataFlit → CHIDAT
CHISNP mapSnp(const chi::SnoopFlit& s);      // mapSnp（:240-252）：SnoopFlit → CHISNP

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

    XscChiAdapter();
};

}  // namespace zj::xs
