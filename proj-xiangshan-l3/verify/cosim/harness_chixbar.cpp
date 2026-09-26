// ChiXbar 对拍 harness：wolvicmod ChiXbar vs refgenDj RTL ChiXbar
// （dongjiang/ChiXbar.scala 真实源码，nrIcn=1、nrDirBank=2、无 HPR/BBN）。
// 纯组合模块：每拍随机驱动 5 路输入 flit + 全部 ready + cBusy，
// 比对 rx 重定向（dirBank/QoS==0xf→HPR）与 tx 直通（SrcID:=0、CBusy:=cBusy）。

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>

#include "VChiXbar.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include "model/dj/chixbar.h"

using namespace wolvicmod;
using namespace zj::dj;

namespace {

void setRxIn(VChiXbar& ref, const Valid<RReqFlit>& v) {
    ref.io_rxReq_inVec_0_valid = v.valid;
    ref.io_rxReq_inVec_0_bits_ExpCompAck = v.bits.exp_comp_ack;
    ref.io_rxReq_inVec_0_bits_Excl = v.bits.excl;
    ref.io_rxReq_inVec_0_bits_SnpAttr = v.bits.snp_attr;
    ref.io_rxReq_inVec_0_bits_MemAttr = v.bits.mem_attr;
    ref.io_rxReq_inVec_0_bits_Order = v.bits.order;
    ref.io_rxReq_inVec_0_bits_Addr = v.bits.addr;
    ref.io_rxReq_inVec_0_bits_Size = v.bits.size;
    ref.io_rxReq_inVec_0_bits_Opcode = v.bits.opcode;
    ref.io_rxReq_inVec_0_bits_TxnID = v.bits.txn_id;
    ref.io_rxReq_inVec_0_bits_SrcID = v.bits.src_id;
    ref.io_rxReq_inVec_0_bits_TgtID = v.bits.tgt_id;
    ref.io_rxReq_inVec_0_bits_QoS = v.bits.qos;
}

void setTxReq(VChiXbar& ref, const Valid<HReqFlit>& v) {
    ref.io_txReq_in_valid = v.valid;
    ref.io_txReq_in_bits_ExpCompAck = v.bits.exp_comp_ack;
    ref.io_txReq_in_bits_Excl = v.bits.excl;
    ref.io_txReq_in_bits_SnpAttr = v.bits.snp_attr;
    ref.io_txReq_in_bits_MemAttr = v.bits.mem_attr;
    ref.io_txReq_in_bits_Order = v.bits.order;
    ref.io_txReq_in_bits_Addr = v.bits.addr;
    ref.io_txReq_in_bits_Size = v.bits.size;
    ref.io_txReq_in_bits_Opcode = v.bits.opcode;
    ref.io_txReq_in_bits_ReturnTxnID = v.bits.return_txn_id;
    ref.io_txReq_in_bits_ReturnNID = v.bits.return_nid;
    ref.io_txReq_in_bits_TxnID = v.bits.txn_id;
    ref.io_txReq_in_bits_SrcID = v.bits.src_id;
    ref.io_txReq_in_bits_TgtID = v.bits.tgt_id;
    ref.io_txReq_in_bits_QoS = v.bits.qos;
}

void setTxSnp(VChiXbar& ref, const Valid<SnoopFlit>& v) {
    ref.io_txSnp_in_valid = v.valid;
    ref.io_txSnp_in_bits_RetToSrc = v.bits.ret_to_src;
    ref.io_txSnp_in_bits_DoNotGoToSD = v.bits.do_not_go_to_sd;
    ref.io_txSnp_in_bits_Addr = v.bits.addr;
    ref.io_txSnp_in_bits_Opcode = v.bits.opcode;
    ref.io_txSnp_in_bits_FwdTxnID = v.bits.fwd_txn_id;
    ref.io_txSnp_in_bits_FwdNID = v.bits.fwd_nid;
    ref.io_txSnp_in_bits_TxnID = v.bits.txn_id;
    ref.io_txSnp_in_bits_SrcID = v.bits.src_id;
    ref.io_txSnp_in_bits_TgtID = v.bits.tgt_id;
    ref.io_txSnp_in_bits_QoS = v.bits.qos;
}

void setTxRsp(VChiXbar& ref, const Valid<RespFlit>& v) {
    ref.io_txRsp_in_valid = v.valid;
    ref.io_txRsp_in_bits_DBID = v.bits.dbid;
    ref.io_txRsp_in_bits_CBusy = v.bits.c_busy;
    ref.io_txRsp_in_bits_FwdState = v.bits.fwd_state;
    ref.io_txRsp_in_bits_Resp = v.bits.resp;
    ref.io_txRsp_in_bits_RespErr = v.bits.resp_err;
    ref.io_txRsp_in_bits_Opcode = v.bits.opcode;
    ref.io_txRsp_in_bits_TxnID = v.bits.txn_id;
    ref.io_txRsp_in_bits_SrcID = v.bits.src_id;
    ref.io_txRsp_in_bits_TgtID = v.bits.tgt_id;
    ref.io_txRsp_in_bits_QoS = v.bits.qos;
}

void setTxDat(VChiXbar& ref, const Valid<DataFlit>& v) {
    ref.io_txDat_in_valid = v.valid;
    for (int i = 0; i < 4; ++i) {
        ref.io_txDat_in_bits_Data[2 * i] = static_cast<uint32_t>(v.bits.data[i]);
        ref.io_txDat_in_bits_Data[2 * i + 1] = static_cast<uint32_t>(v.bits.data[i] >> 32);
    }
    ref.io_txDat_in_bits_BE = v.bits.be;
    ref.io_txDat_in_bits_DataID = v.bits.data_id;
    ref.io_txDat_in_bits_DBID = v.bits.dbid;
    ref.io_txDat_in_bits_CBusy = v.bits.c_busy;
    ref.io_txDat_in_bits_DataSource = v.bits.data_source;
    ref.io_txDat_in_bits_Resp = v.bits.resp;
    ref.io_txDat_in_bits_RespErr = v.bits.resp_err;
    ref.io_txDat_in_bits_Opcode = v.bits.opcode;
    ref.io_txDat_in_bits_HomeNID = v.bits.home_nid;
    ref.io_txDat_in_bits_TxnID = v.bits.txn_id;
    ref.io_txDat_in_bits_SrcID = v.bits.src_id;
    ref.io_txDat_in_bits_TgtID = v.bits.tgt_id;
    ref.io_txDat_in_bits_QoS = v.bits.qos;
}

// ---- 输出侧比对 ----

#define CHK(st, seed, c, nm, r, d) cosim::check(st, "xb", "ChiXbar", seed, c, nm, r, d, rp)

void checkReqOut(VChiXbar& ref, int j, const char* ch, const Valid<RReqFlit>& d,
                 cosim::Stats& st, uint32_t seed, uint64_t c, cosim::Replay& rp) {
    const bool rv = j == 0 ? (ch[0] == 'q' ? ref.io_rxReq_outVec_0_valid
                                           : ref.io_rxHpr_outVec_0_valid)
                    : (ch[0] == 'q' ? ref.io_rxReq_outVec_1_valid
                                    : ref.io_rxHpr_outVec_1_valid);
    std::string p = std::string(ch) + std::to_string(j);
    CHK(st, seed, c, (p + "_v").c_str(), rv, d.valid);
    if (!rv || !d.valid) return;
    uint64_t rECA, rEx, rSa, rMa, rOr, rAd, rSz, rOp, rTx, rSr, rTg, rQo;
    if (j == 0 && ch[0] == 'q') {
        rECA = ref.io_rxReq_outVec_0_bits_ExpCompAck; rEx = ref.io_rxReq_outVec_0_bits_Excl;
        rSa = ref.io_rxReq_outVec_0_bits_SnpAttr; rMa = ref.io_rxReq_outVec_0_bits_MemAttr;
        rOr = ref.io_rxReq_outVec_0_bits_Order; rAd = ref.io_rxReq_outVec_0_bits_Addr;
        rSz = ref.io_rxReq_outVec_0_bits_Size; rOp = ref.io_rxReq_outVec_0_bits_Opcode;
        rTx = ref.io_rxReq_outVec_0_bits_TxnID; rSr = ref.io_rxReq_outVec_0_bits_SrcID;
        rTg = ref.io_rxReq_outVec_0_bits_TgtID; rQo = ref.io_rxReq_outVec_0_bits_QoS;
    } else if (j == 1 && ch[0] == 'q') {
        rECA = ref.io_rxReq_outVec_1_bits_ExpCompAck; rEx = ref.io_rxReq_outVec_1_bits_Excl;
        rSa = ref.io_rxReq_outVec_1_bits_SnpAttr; rMa = ref.io_rxReq_outVec_1_bits_MemAttr;
        rOr = ref.io_rxReq_outVec_1_bits_Order; rAd = ref.io_rxReq_outVec_1_bits_Addr;
        rSz = ref.io_rxReq_outVec_1_bits_Size; rOp = ref.io_rxReq_outVec_1_bits_Opcode;
        rTx = ref.io_rxReq_outVec_1_bits_TxnID; rSr = ref.io_rxReq_outVec_1_bits_SrcID;
        rTg = ref.io_rxReq_outVec_1_bits_TgtID; rQo = ref.io_rxReq_outVec_1_bits_QoS;
    } else if (j == 0) {
        rECA = ref.io_rxHpr_outVec_0_bits_ExpCompAck; rEx = ref.io_rxHpr_outVec_0_bits_Excl;
        rSa = ref.io_rxHpr_outVec_0_bits_SnpAttr; rMa = ref.io_rxHpr_outVec_0_bits_MemAttr;
        rOr = ref.io_rxHpr_outVec_0_bits_Order; rAd = ref.io_rxHpr_outVec_0_bits_Addr;
        rSz = ref.io_rxHpr_outVec_0_bits_Size; rOp = ref.io_rxHpr_outVec_0_bits_Opcode;
        rTx = ref.io_rxHpr_outVec_0_bits_TxnID; rSr = ref.io_rxHpr_outVec_0_bits_SrcID;
        rTg = ref.io_rxHpr_outVec_0_bits_TgtID; rQo = ref.io_rxHpr_outVec_0_bits_QoS;
    } else {
        rECA = ref.io_rxHpr_outVec_1_bits_ExpCompAck; rEx = ref.io_rxHpr_outVec_1_bits_Excl;
        rSa = ref.io_rxHpr_outVec_1_bits_SnpAttr; rMa = ref.io_rxHpr_outVec_1_bits_MemAttr;
        rOr = ref.io_rxHpr_outVec_1_bits_Order; rAd = ref.io_rxHpr_outVec_1_bits_Addr;
        rSz = ref.io_rxHpr_outVec_1_bits_Size; rOp = ref.io_rxHpr_outVec_1_bits_Opcode;
        rTx = ref.io_rxHpr_outVec_1_bits_TxnID; rSr = ref.io_rxHpr_outVec_1_bits_SrcID;
        rTg = ref.io_rxHpr_outVec_1_bits_TgtID; rQo = ref.io_rxHpr_outVec_1_bits_QoS;
    }
    const auto& b = d.bits;
    CHK(st, seed, c, (p + "_eca").c_str(), rECA, b.exp_comp_ack);
    CHK(st, seed, c, (p + "_ex").c_str(), rEx, b.excl);
    CHK(st, seed, c, (p + "_sa").c_str(), rSa, b.snp_attr);
    CHK(st, seed, c, (p + "_ma").c_str(), rMa, b.mem_attr);
    CHK(st, seed, c, (p + "_or").c_str(), rOr, b.order);
    CHK(st, seed, c, (p + "_ad").c_str(), rAd, b.addr);
    CHK(st, seed, c, (p + "_sz").c_str(), rSz, b.size);
    CHK(st, seed, c, (p + "_op").c_str(), rOp, b.opcode);
    CHK(st, seed, c, (p + "_tx").c_str(), rTx, b.txn_id);
    CHK(st, seed, c, (p + "_sr").c_str(), rSr, b.src_id);
    CHK(st, seed, c, (p + "_tg").c_str(), rTg, b.tgt_id);
    CHK(st, seed, c, (p + "_qo").c_str(), rQo, b.qos);
}

uint64_t cosimChiXbar(uint32_t seed, uint64_t cycles) {
    VChiXbar ref;
    ChiXbar dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;

    cosim::resetRef(ref, [&] {
        setRxIn(ref, {false, {}});
        setTxReq(ref, {false, {}});
        setTxSnp(ref, {false, {}});
        setTxRsp(ref, {false, {}});
        setTxDat(ref, {false, {}});
        ref.io_rxReq_outVec_0_ready = 0;
        ref.io_rxReq_outVec_1_ready = 0;
        ref.io_rxHpr_outVec_0_ready = 0;
        ref.io_rxHpr_outVec_1_ready = 0;
        ref.io_txReq_outVec_0_ready = 0;
        ref.io_txSnp_outVec_0_ready = 0;
        ref.io_txRsp_outVec_0_ready = 0;
        ref.io_txDat_outVec_0_ready = 0;
        ref.io_cBusy = 0;
    });
    dut.clk.set(0);
    dut.rx_req_in.set({false, {}});
    dut.tx_req_in.set({false, {}});
    dut.tx_snp_in.set({false, {}});
    dut.tx_rsp_in.set({false, {}});
    dut.tx_dat_in.set({false, {}});
    dut.rx_req_out_rdy.set({false, false});
    dut.rx_hpr_out_rdy.set({false, false});
    dut.tx_req_out_rdy.set(false);
    dut.tx_snp_out_rdy.set(false);
    dut.tx_rsp_out_rdy.set(false);
    dut.tx_dat_out_rdy.set(false);
    dut.c_busy.set(0);

    for (uint64_t c = 0; c < cycles; ++c) {
        // ---- 激励 ----
        Valid<RReqFlit> rx{cosim::roll(rng, 60), {}};
        if (rx.valid) {
            auto& b = rx.bits;
            b.exp_comp_ack = rng() & 1;
            b.excl = rng() & 1;
            b.snp_attr = rng() & 1;
            b.mem_attr = rng() & 0xf;
            b.order = rng() & 3;
            b.addr = (static_cast<uint64_t>(rng()) << 16) | (rng() & 0xffff);
            b.size = rng() & 7;
            b.opcode = rng() & 0x7f;
            b.txn_id = rng() & 0xfff;
            b.src_id = rng() & 0x7ff;
            b.tgt_id = rng() & 0x7ff;
            b.qos = cosim::roll(rng, 25) ? 0xf : (rng() & 0xf);  // 25% 高优先
        }
        Valid<HReqFlit> txq{cosim::roll(rng, 50), {}};
        if (txq.valid) {
            auto& b = txq.bits;
            b.exp_comp_ack = rng() & 1;
            b.excl = rng() & 1;
            b.snp_attr = rng() & 1;
            b.mem_attr = rng() & 0xf;
            b.order = rng() & 3;
            b.addr = (static_cast<uint64_t>(rng()) << 16) | (rng() & 0xffff);
            b.size = rng() & 7;
            b.opcode = rng() & 0x7f;
            b.return_txn_id = rng() & 0xfff;
            b.return_nid = rng() & 0x7ff;
            b.txn_id = rng() & 0xfff;
            b.src_id = rng() & 0x7ff;
            b.tgt_id = rng() & 0x7ff;
            b.qos = rng() & 0xf;
        }
        Valid<SnoopFlit> txs{cosim::roll(rng, 40), {}};
        if (txs.valid) {
            auto& b = txs.bits;
            b.ret_to_src = rng() & 1;
            b.do_not_go_to_sd = rng() & 1;
            b.addr = (static_cast<uint64_t>(rng()) << 13) | (rng() & 0x1fff);
            b.opcode = rng() & 0x1f;
            b.fwd_txn_id = rng() & 0xfff;
            b.fwd_nid = rng() & 0x7ff;
            b.txn_id = rng() & 0xfff;
            b.src_id = rng() & 0x7ff;
            b.tgt_id = rng() & 0x7ff;
            b.qos = rng() & 0xf;
        }
        Valid<RespFlit> txr{cosim::roll(rng, 50), {}};
        if (txr.valid) {
            auto& b = txr.bits;
            b.dbid = rng() & 0xfff;
            b.c_busy = rng() & 7;
            b.fwd_state = rng() & 7;
            b.resp = rng() & 7;
            b.resp_err = rng() & 3;
            b.opcode = rng() & 0x1f;
            b.txn_id = rng() & 0xfff;
            b.src_id = rng() & 0x7ff;
            b.tgt_id = rng() & 0x7ff;
            b.qos = rng() & 0xf;
        }
        Valid<DataFlit> txd{cosim::roll(rng, 50), {}};
        if (txd.valid) {
            auto& b = txd.bits;
            for (auto& w : b.data) w = (static_cast<uint64_t>(rng()) << 32) | rng();
            b.be = rng();
            b.data_id = rng() & 3;
            b.dbid = rng() & 0xffff;
            b.c_busy = rng() & 7;
            b.data_source = rng() & 0xff;
            b.resp = rng() & 7;
            b.resp_err = rng() & 3;
            b.opcode = rng() & 0xf;
            b.home_nid = rng() & 0x7ff;
            b.txn_id = rng() & 0xfff;
            b.src_id = rng() & 0x7ff;
            b.tgt_id = rng() & 0x7ff;
            b.qos = rng() & 0xf;
        }
        std::array<bool, 2> reqRdy{static_cast<bool>(rng() & 1), static_cast<bool>(rng() & 1)};
        std::array<bool, 2> hprRdy{static_cast<bool>(rng() & 1), static_cast<bool>(rng() & 1)};
        const bool txqRdy = rng() & 1, txsRdy = rng() & 1, txrRdy = rng() & 1,
                   txdRdy = rng() & 1;
        const uint8_t cBusy = rng() & 7;

        setRxIn(ref, rx);
        setTxReq(ref, txq);
        setTxSnp(ref, txs);
        setTxRsp(ref, txr);
        setTxDat(ref, txd);
        ref.io_rxReq_outVec_0_ready = reqRdy[0];
        ref.io_rxReq_outVec_1_ready = reqRdy[1];
        ref.io_rxHpr_outVec_0_ready = hprRdy[0];
        ref.io_rxHpr_outVec_1_ready = hprRdy[1];
        ref.io_txReq_outVec_0_ready = txqRdy;
        ref.io_txSnp_outVec_0_ready = txsRdy;
        ref.io_txRsp_outVec_0_ready = txrRdy;
        ref.io_txDat_outVec_0_ready = txdRdy;
        ref.io_cBusy = cBusy;

        dut.rx_req_in.set(rx);
        dut.tx_req_in.set(txq);
        dut.tx_snp_in.set(txs);
        dut.tx_rsp_in.set(txr);
        dut.tx_dat_in.set(txd);
        dut.rx_req_out_rdy.set(reqRdy);
        dut.rx_hpr_out_rdy.set(hprRdy);
        dut.tx_req_out_rdy.set(txqRdy);
        dut.tx_snp_out_rdy.set(txsRdy);
        dut.tx_rsp_out_rdy.set(txrRdy);
        dut.tx_dat_out_rdy.set(txdRdy);
        dut.c_busy.set(cBusy);

        rp.push("cyc=" + std::to_string(c));
        cosim::phaseLow(ref, dut);
        if (std::getenv("CX_TRACE") && c <= (uint64_t)std::atol(std::getenv("CX_TRACE"))) {
            std::cout << "  [x] cyc=" << c << " rx(v=" << (int)rx.valid << ",a6="
                      << (int)((rx.bits.addr >> 6) & 1) << ",qos=0x" << std::hex
                      << (int)rx.bits.qos << std::dec << ") reqRdy=" << reqRdy[0] << reqRdy[1]
                      << " hprRdy=" << hprRdy[0] << hprRdy[1]
                      << " | ref: inRdy=" << (int)ref.io_rxReq_inVec_0_ready
                      << " qv=" << (int)ref.io_rxReq_outVec_0_valid
                      << (int)ref.io_rxReq_outVec_1_valid
                      << " hv=" << (int)ref.io_rxHpr_outVec_0_valid
                      << (int)ref.io_rxHpr_outVec_1_valid
                      << " | dut: inRdy=" << (int)dut.rx_req_in_rdy.get()
                      << " qv=" << (int)dut.rx_req_out.get()[0].valid
                      << (int)dut.rx_req_out.get()[1].valid
                      << " hv=" << (int)dut.rx_hpr_out.get()[0].valid
                      << (int)dut.rx_hpr_out.get()[1].valid << "\n";
        }

        // ---- 比对 ----
        CHK(st, seed, c, "rxin_rdy", ref.io_rxReq_inVec_0_ready,
            dut.rx_req_in_rdy.get());
        for (int j = 0; j < 2; ++j) {
            checkReqOut(ref, j, "q", dut.rx_req_out.get()[j], st, seed, c, rp);
            checkReqOut(ref, j, "h", dut.rx_hpr_out.get()[j], st, seed, c, rp);
        }
        // txReq
        CHK(st, seed, c, "txq_rdy", ref.io_txReq_in_ready,
            dut.tx_req_in_rdy.get());
        CHK(st, seed, c, "txq_v", ref.io_txReq_outVec_0_valid,
            dut.tx_req_out.get().valid);
        if (ref.io_txReq_outVec_0_valid && dut.tx_req_out.get().valid) {
            const auto& b = dut.tx_req_out.get().bits;
            CHK(st, seed, c, "txq_eca", ref.io_txReq_outVec_0_bits_ExpCompAck, b.exp_comp_ack);
            CHK(st, seed, c, "txq_ex", ref.io_txReq_outVec_0_bits_Excl, b.excl);
            CHK(st, seed, c, "txq_sa", ref.io_txReq_outVec_0_bits_SnpAttr, b.snp_attr);
            CHK(st, seed, c, "txq_ma", ref.io_txReq_outVec_0_bits_MemAttr, b.mem_attr);
            CHK(st, seed, c, "txq_or", ref.io_txReq_outVec_0_bits_Order, b.order);
            CHK(st, seed, c, "txq_ad", ref.io_txReq_outVec_0_bits_Addr, b.addr);
            CHK(st, seed, c, "txq_sz", ref.io_txReq_outVec_0_bits_Size, b.size);
            CHK(st, seed, c, "txq_op", ref.io_txReq_outVec_0_bits_Opcode, b.opcode);
            CHK(st, seed, c, "txq_rtx", ref.io_txReq_outVec_0_bits_ReturnTxnID, b.return_txn_id);
            CHK(st, seed, c, "txq_rni", ref.io_txReq_outVec_0_bits_ReturnNID, b.return_nid);
            CHK(st, seed, c, "txq_tx", ref.io_txReq_outVec_0_bits_TxnID, b.txn_id);
            CHK(st, seed, c, "txq_sr", ref.io_txReq_outVec_0_bits_SrcID, b.src_id);
            CHK(st, seed, c, "txq_tg", ref.io_txReq_outVec_0_bits_TgtID, b.tgt_id);
            CHK(st, seed, c, "txq_qo", ref.io_txReq_outVec_0_bits_QoS, b.qos);
        }
        // txSnp
        CHK(st, seed, c, "txs_rdy", ref.io_txSnp_in_ready,
            dut.tx_snp_in_rdy.get());
        CHK(st, seed, c, "txs_v", ref.io_txSnp_outVec_0_valid,
            dut.tx_snp_out.get().valid);
        if (ref.io_txSnp_outVec_0_valid && dut.tx_snp_out.get().valid) {
            const auto& b = dut.tx_snp_out.get().bits;
            CHK(st, seed, c, "txs_rts", ref.io_txSnp_outVec_0_bits_RetToSrc, b.ret_to_src);
            CHK(st, seed, c, "txs_dng", ref.io_txSnp_outVec_0_bits_DoNotGoToSD, b.do_not_go_to_sd);
            CHK(st, seed, c, "txs_ad", ref.io_txSnp_outVec_0_bits_Addr, b.addr);
            CHK(st, seed, c, "txs_op", ref.io_txSnp_outVec_0_bits_Opcode, b.opcode);
            CHK(st, seed, c, "txs_ftx", ref.io_txSnp_outVec_0_bits_FwdTxnID, b.fwd_txn_id);
            CHK(st, seed, c, "txs_fni", ref.io_txSnp_outVec_0_bits_FwdNID, b.fwd_nid);
            CHK(st, seed, c, "txs_tx", ref.io_txSnp_outVec_0_bits_TxnID, b.txn_id);
            CHK(st, seed, c, "txs_sr", ref.io_txSnp_outVec_0_bits_SrcID, b.src_id);
            CHK(st, seed, c, "txs_tg", ref.io_txSnp_outVec_0_bits_TgtID, b.tgt_id);
            CHK(st, seed, c, "txs_qo", ref.io_txSnp_outVec_0_bits_QoS, b.qos);
        }
        // txRsp
        CHK(st, seed, c, "txr_rdy", ref.io_txRsp_in_ready,
            dut.tx_rsp_in_rdy.get());
        CHK(st, seed, c, "txr_v", ref.io_txRsp_outVec_0_valid,
            dut.tx_rsp_out.get().valid);
        if (ref.io_txRsp_outVec_0_valid && dut.tx_rsp_out.get().valid) {
            const auto& b = dut.tx_rsp_out.get().bits;
            CHK(st, seed, c, "txr_db", ref.io_txRsp_outVec_0_bits_DBID, b.dbid);
            CHK(st, seed, c, "txr_cb", ref.io_txRsp_outVec_0_bits_CBusy, b.c_busy);
            CHK(st, seed, c, "txr_fs", ref.io_txRsp_outVec_0_bits_FwdState, b.fwd_state);
            CHK(st, seed, c, "txr_rs", ref.io_txRsp_outVec_0_bits_Resp, b.resp);
            CHK(st, seed, c, "txr_re", ref.io_txRsp_outVec_0_bits_RespErr, b.resp_err);
            CHK(st, seed, c, "txr_op", ref.io_txRsp_outVec_0_bits_Opcode, b.opcode);
            CHK(st, seed, c, "txr_tx", ref.io_txRsp_outVec_0_bits_TxnID, b.txn_id);
            CHK(st, seed, c, "txr_sr", ref.io_txRsp_outVec_0_bits_SrcID, b.src_id);
            CHK(st, seed, c, "txr_tg", ref.io_txRsp_outVec_0_bits_TgtID, b.tgt_id);
            CHK(st, seed, c, "txr_qo", ref.io_txRsp_outVec_0_bits_QoS, b.qos);
        }
        // txDat
        CHK(st, seed, c, "txd_rdy", ref.io_txDat_in_ready,
            dut.tx_dat_in_rdy.get());
        CHK(st, seed, c, "txd_v", ref.io_txDat_outVec_0_valid,
            dut.tx_dat_out.get().valid);
        if (ref.io_txDat_outVec_0_valid && dut.tx_dat_out.get().valid) {
            const auto& b = dut.tx_dat_out.get().bits;
            for (int i = 0; i < 4; ++i) {
                CHK(st, seed, c, ("txd_d" + std::to_string(2 * i)).c_str(),
                    ref.io_txDat_outVec_0_bits_Data[2 * i],
                    static_cast<uint32_t>(b.data[i]));
                CHK(st, seed, c, ("txd_d" + std::to_string(2 * i + 1)).c_str(),
                    ref.io_txDat_outVec_0_bits_Data[2 * i + 1],
                    static_cast<uint32_t>(b.data[i] >> 32));
            }
            CHK(st, seed, c, "txd_be", ref.io_txDat_outVec_0_bits_BE, b.be);
            CHK(st, seed, c, "txd_di", ref.io_txDat_outVec_0_bits_DataID, b.data_id);
            CHK(st, seed, c, "txd_db", ref.io_txDat_outVec_0_bits_DBID, b.dbid);
            CHK(st, seed, c, "txd_cb", ref.io_txDat_outVec_0_bits_CBusy, b.c_busy);
            CHK(st, seed, c, "txd_ds", ref.io_txDat_outVec_0_bits_DataSource, b.data_source);
            CHK(st, seed, c, "txd_rs", ref.io_txDat_outVec_0_bits_Resp, b.resp);
            CHK(st, seed, c, "txd_re", ref.io_txDat_outVec_0_bits_RespErr, b.resp_err);
            CHK(st, seed, c, "txd_op", ref.io_txDat_outVec_0_bits_Opcode, b.opcode);
            CHK(st, seed, c, "txd_hn", ref.io_txDat_outVec_0_bits_HomeNID, b.home_nid);
            CHK(st, seed, c, "txd_tx", ref.io_txDat_outVec_0_bits_TxnID, b.txn_id);
            CHK(st, seed, c, "txd_sr", ref.io_txDat_outVec_0_bits_SrcID, b.src_id);
            CHK(st, seed, c, "txd_tg", ref.io_txDat_outVec_0_bits_TgtID, b.tgt_id);
            CHK(st, seed, c, "txd_qo", ref.io_txDat_outVec_0_bits_QoS, b.qos);
        }

        cosim::phaseHigh(ref, dut);
    }
    std::cout << (st.mismatches == 0 ? "PASS" : "FAIL") << " chixbar ChiXbar seed=" << seed
              << " cycles=" << cycles << " checks=" << st.checks
              << " mismatches=" << st.mismatches << "\n";
    return st.mismatches;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {1u, 2u, 3u}) bad += cosimChiXbar(seed, 100000);
    return bad == 0 ? 0 : 1;
}
