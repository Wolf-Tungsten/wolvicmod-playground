#include "model/cc/xsc_chi_adapter.h"

#include <wolvicmod/wolvicmod.h>

namespace zj::xs {

// ---------------- 映射函数（ZhuJiangBridge.scala:152-252 逐字段转写）----------------

// mapReq（:152-171）：CHIREQ → ReqFlit
chi::RReqFlit mapReq(const CHIREQ& s) {
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
chi::RespFlit mapRspZj(const CHIRSP& s) {
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
CHIRSP mapRspXs(const chi::RespFlit& s) {
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
chi::DataFlit mapDatZj(const CHIDAT& s) {
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
CHIDAT mapDatXs(const chi::DataFlit& s) {
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
CHISNP mapSnp(const chi::SnoopFlit& s) {
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

XscChiAdapter::XscChiAdapter() {
    // L2 → ZJ（mapReq/mapRsp/mapDat）+ ready 直通
    zj_rx_req.assign().reads(chi_tx_req) = [](auto src) {
        auto [chi_tx_req] = src;
        Valid<chi::RReqFlit> d;
        d.valid = chi_tx_req.valid;
        d.bits  = mapReq(chi_tx_req.bits);
        return d;
    };
    chi_tx_req_rdy = zj_rx_req_rdy;
    zj_rx_rsp.assign().reads(chi_tx_rsp) = [](auto src) {
        auto [chi_tx_rsp] = src;
        Valid<chi::RespFlit> d;
        d.valid = chi_tx_rsp.valid;
        d.bits  = mapRspZj(chi_tx_rsp.bits);
        return d;
    };
    chi_tx_rsp_rdy = zj_rx_rsp_rdy;
    zj_rx_dat.assign().reads(chi_tx_dat) = [](auto src) {
        auto [chi_tx_dat] = src;
        Valid<chi::DataFlit> d;
        d.valid = chi_tx_dat.valid;
        d.bits  = mapDatZj(chi_tx_dat.bits);
        return d;
    };
    chi_tx_dat_rdy = zj_rx_dat_rdy;
    // ZJ → L2（mapRsp/mapDat/mapSnp）+ ready 直通
    chi_rx_rsp.assign().reads(zj_tx_rsp) = [](auto src) {
        auto [zj_tx_rsp] = src;
        Valid<CHIRSP> d;
        d.valid = zj_tx_rsp.valid;
        d.bits  = mapRspXs(zj_tx_rsp.bits);
        return d;
    };
    zj_tx_rsp_rdy = chi_rx_rsp_rdy;
    chi_rx_dat.assign().reads(zj_tx_dat) = [](auto src) {
        auto [zj_tx_dat] = src;
        Valid<CHIDAT> d;
        d.valid = zj_tx_dat.valid;
        d.bits  = mapDatXs(zj_tx_dat.bits);
        return d;
    };
    zj_tx_dat_rdy = chi_rx_dat_rdy;
    chi_rx_snp.assign().reads(zj_tx_snp) = [](auto src) {
        auto [zj_tx_snp] = src;
        Valid<CHISNP> d;
        d.valid = zj_tx_snp.valid;
        d.bits  = mapSnp(zj_tx_snp.bits);
        return d;
    };
    zj_tx_snp_rdy = chi_rx_snp_rdy;
}

}  // namespace zj::xs
