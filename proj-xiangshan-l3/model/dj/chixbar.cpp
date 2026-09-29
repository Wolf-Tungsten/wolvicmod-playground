#include "model/dj/chixbar.h"

namespace zj::dj {

ChiXbar::ChiXbar() {
    // ---- rxReq 重定向：dirBank = useAddr[0] = addr[6]；QoS==0xf → HPR ----
    // 生成 SV 语义：req/hpr 的 bits 在非选中分支为 DontCare（对拍只在 valid 时
    // 比对 bits）；in.ready = in.valid & 选中 bank 的 (qosF ? hpr_rdy : req_rdy)
    // （VipArbiter(1) 的 in.ready = in.valid & out.ready，与 valid 相关）。
    rx_req_out.assign().reads(rx_req_in) = [](auto src) {
        auto [in] = src;
        A2VRReq o{};
        const uint32_t bank = (in.bits.addr >> 6) & 1u;
        for (uint32_t j = 0; j < 2; ++j) {
            o[j].valid = in.valid && bank == j && in.bits.qos != 0xf;
            o[j].bits = in.bits;
        }
        return o;
    };
    rx_hpr_out.assign().reads(rx_req_in) = [](auto src) {
        auto [in] = src;
        A2VRReq o{};
        const uint32_t bank = (in.bits.addr >> 6) & 1u;
        for (uint32_t j = 0; j < 2; ++j) {
            o[j].valid = in.valid && bank == j && in.bits.qos == 0xf;
            o[j].bits = in.bits;
        }
        return o;
    };
    rx_req_in_rdy.assign().reads(rx_req_in, rx_req_out_rdy, rx_hpr_out_rdy) = [](auto src) {
        auto [in, req_rdy, hpr_rdy] = src;
        const uint32_t bank = (in.bits.addr >> 6) & 1u;
        return in.valid && (in.bits.qos == 0xf ? hpr_rdy[bank] : req_rdy[bank]);
    };

    // ---- tx 四通道：直通 + SrcID:=0（rsp/dat 另 CBusy:=c_busy） ----
    tx_req_out.assign().reads(tx_req_in) = [](auto src) {
        auto [in] = src;
        VHReq o{in.valid, in.bits};
        o.bits.src_id = 0;
        return o;
    };
    tx_req_in_rdy = tx_req_out_rdy;  // in.ready := out.ready（恒等连接，零成本）
    tx_snp_out.assign().reads(tx_snp_in) = [](auto src) {
        auto [in] = src;
        VSnp o{in.valid, in.bits};
        o.bits.src_id = 0;
        return o;
    };
    tx_snp_in_rdy = tx_snp_out_rdy;
    tx_rsp_out.assign().reads(tx_rsp_in, c_busy) = [](auto src) {
        auto [in, c_busy] = src;
        VRsp o{in.valid, in.bits};
        o.bits.src_id = 0;
        o.bits.c_busy = c_busy & 7;
        return o;
    };
    tx_rsp_in_rdy = tx_rsp_out_rdy;
    tx_dat_out.assign().reads(tx_dat_in, c_busy) = [](auto src) {
        auto [in, c_busy] = src;
        VDat o{in.valid, in.bits};
        o.bits.src_id = 0;
        o.bits.c_busy = c_busy & 7;
        return o;
    };
    tx_dat_in_rdy = tx_dat_out_rdy;
}

}  // namespace zj::dj
