// ChiXbar（dongjiang/ChiXbar.scala，kunminghu-v3 真实配置：
// nrIcn=1、nrDirBank=2、hasHPR=false、hasBBN=false）。
// rxReq 一路按 getDirBank(addr[6]) 重定向到两个 dirBank；QoS==0xf 的重定向到
// rxHpr 输出（Frontend 的 HPR 优先通道由此而来），其余到 rxReq 输出。
// tx 四通道一出：直通，SrcID:=0，txRsp/txDat CBusy:=io.cBusy。
#pragma once

#include <array>
#include <cstdint>

#include <wolvicmod/wolvicmod.h>

#include "model/flit/zj_flit.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::dj {

using chi::DataFlit;
using chi::HReqFlit;
using chi::RespFlit;
using chi::RReqFlit;
using chi::SnoopFlit;
using wolvicmod::prefab::Valid;

class ChiXbar : public wolvicmod::Module {
public:
    using VRReq = Valid<RReqFlit>;
    using VHReq = Valid<HReqFlit>;
    using VSnp = Valid<SnoopFlit>;
    using VRsp = Valid<RespFlit>;
    using VDat = Valid<DataFlit>;
    using A2VRReq = std::array<VRReq, 2>;  // 宏参数含逗号，先取别名
    using A2Bool = std::array<bool, 2>;

    IN(bool, clk);  // 纯组合，clk 仅为接口统一

    IN(VRReq, rx_req_in);
    OUT(bool, rx_req_in_rdy);
    OUT(A2VRReq, rx_req_out);
    IN(A2Bool, rx_req_out_rdy);
    OUT(A2VRReq, rx_hpr_out);
    IN(A2Bool, rx_hpr_out_rdy);

    IN(VHReq, tx_req_in);
    OUT(bool, tx_req_in_rdy);
    OUT(VHReq, tx_req_out);
    IN(bool, tx_req_out_rdy);
    IN(VSnp, tx_snp_in);
    OUT(bool, tx_snp_in_rdy);
    OUT(VSnp, tx_snp_out);
    IN(bool, tx_snp_out_rdy);
    IN(VRsp, tx_rsp_in);
    OUT(bool, tx_rsp_in_rdy);
    OUT(VRsp, tx_rsp_out);
    IN(bool, tx_rsp_out_rdy);
    IN(VDat, tx_dat_in);
    OUT(bool, tx_dat_in_rdy);
    OUT(VDat, tx_dat_out);
    IN(bool, tx_dat_out_rdy);

    IN(uint8_t, c_busy);  // 3bit

    ChiXbar();
};

}  // namespace zj::dj
