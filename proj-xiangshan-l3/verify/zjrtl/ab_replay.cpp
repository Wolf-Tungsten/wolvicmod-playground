// zj_ab_replay：ZhuJiang L3 共栖 A/B 回放器（apple-to-apple 孤立对比）。
//
// 同一二进制链接两个 DUT 实现，共享同一份 trace、同一进程同一机器：
//   [rtl]    verilated ZhujiangReplayTop（XiangShan 生成的 ZhuJiang RTL）
//   [wolvic] WolvicZjTop 裸模型（wolvicmod，不经任何 DPI 包装）
// 运行时 --dut=rtl|wolvic|both 选择激活侧：
//   - solo 模式用于性能剖析（另一侧不实例化，互不干扰缓存/带宽）；
//   - both 模式同拍驱动两侧、各自独立对 trace 比对（等价性交叉验证；
//     两侧同进程共享内存子系统，计时有互扰，剖析请用 solo）。
// 两侧均不开启任何波形。
//
// 口径与历史独立 harness 完全一致：--dut=rtl 等价于原 rtl_replay.cpp
// （已被本文件取代），--dut=wolvic 等价于 tests/test_wolvic_top_replay.cpp
// （后者仍保留为 ctest 回归）。预滚（复位 10 拍 + 空闲 N 拍）仅作用于
// RTL 侧：模型无复位端口，elaborate 后即对齐 trace row 0（既有回放口径）；
// RTL 需要预滚覆盖环站复位传播。
//
// 用法：zj_ab_replay [trace.txt] [--dut=rtl|wolvic|both] [--rows=N] [--preroll=N]
// 输出：每侧 checks/mismatches + eval 分相计时；loop wall 的 ns/row 口径
// （驱动+eval+比对）与 §9 历史数字（12.8µs / 263.8µs）直接可比。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "VZhujiangReplayTop.h"
#include "verilated.h"

#include "wolvicmod/wolvicmod.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"

#include "model/wolvic_zj_top.h"
#include "verify/zjrtl/trace_io.h"

using namespace zj;
using namespace zj::chi;
using wolvicmod::prefab::Valid;
using zjrtl::Trace;

namespace {

Trace    g_tr;
uint64_t g_cyc = 0;

// side.chan.field 列取值（side = l2|mem|cfg）
uint64_t colGet(const char* side, const char* chan, const char* field, size_t r) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s.%s.%s", side, chan, field);
    return g_tr.get(buf, r);
}

struct Score {
    const char* tag;
    uint64_t    checks = 0, mismatch = 0;
    void cmp(const char* port, const char* field, uint64_t ref, uint64_t dut) {
        ++checks;
        if (ref == dut) return;
        ++mismatch;
        if (mismatch <= 20)
            std::printf("MISMATCH[%s] cyc=%lu %s.%s ref=0x%llx dut=0x%llx\n", tag,
                        (unsigned long)g_cyc, port, field, (unsigned long long)ref,
                        (unsigned long long)dut);
    }
};

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---------------- RTL 侧：verilated ZhujiangReplayTop ----------------

// 64b 字 → Verilator VlWide（32b 小端字序）
template <typename W>
void setWide(W& port, const uint64_t v[4]) {
    for (int i = 0; i < 4; ++i) {
        port[2 * i]     = uint32_t(v[i]);
        port[2 * i + 1] = uint32_t(v[i] >> 32);
    }
}
template <typename W>
void cmpWide(Score& sb, const char* port, const char* field, const uint64_t ref[4], const W& dut) {
    for (int i = 0; i < 4; ++i) {
        uint64_t d = uint64_t(dut[2 * i]) | (uint64_t(dut[2 * i + 1]) << 32);
        sb.cmp(port, field, ref[i], d);
    }
}

struct RtlSide {
    VZhujiangReplayTop* top = nullptr;
    Score               sb{"rtl"};
    double              tEval0 = 0, tEval1 = 0;

    void create() { top = new VZhujiangReplayTop; }
    void eval0() {
        top->clock = 0;
        top->eval();
    }
    void eval1() {
        top->clock = 1;
        top->eval();
    }
    void idleInputs() {
        // valid 全 0、ready 全 1：吸收复位撤除后的链路初始化流量
        top->io_decoupledCHI_tx_req_valid = 0;
        top->io_decoupledCHI_tx_rsp_valid = 0;
        top->io_decoupledCHI_tx_dat_valid = 0;
        top->io_decoupledCHI_rx_rsp_ready = 1;
        top->io_decoupledCHI_rx_dat_ready = 1;
        top->io_decoupledCHI_rx_snp_ready = 1;
        top->m_axi_mem_0_awready = 1;
        top->m_axi_mem_0_wready  = 1;
        top->m_axi_mem_0_arready = 1;
        top->m_axi_mem_0_bvalid  = 0;
        top->m_axi_mem_0_rvalid  = 0;
        top->m_axi_main_awready  = 1;
        top->m_axi_main_wready   = 1;
        top->m_axi_main_arready  = 1;
        top->m_axi_main_bvalid   = 0;
        top->m_axi_main_rvalid   = 0;
    }
    // 复位预滚：对齐 trace row 0（提取器跳过所有复位激活拍，含环站复位）
    void preroll(long n) {
        idleInputs();
        top->reset = 1;
        for (int i = 0; i < 10; ++i) { eval0(); eval1(); }
        top->reset = 0;
        for (long i = 0; i < n; ++i) { eval0(); eval1(); }
        std::printf("[rtl] preroll done: on_reset=%u\n", unsigned(top->on_reset));
    }

    void drive(size_t r) {
        uint64_t dw[4];
        top->io_decoupledCHI_tx_req_valid = g_tr.get("l2.tx_req.valid", r);
        top->io_decoupledCHI_tx_req_bits_qos = g_tr.get("l2.tx_req.bits_qos", r);
        top->io_decoupledCHI_tx_req_bits_tgtID = g_tr.get("l2.tx_req.bits_tgtID", r);
        top->io_decoupledCHI_tx_req_bits_srcID = g_tr.get("l2.tx_req.bits_srcID", r);
        top->io_decoupledCHI_tx_req_bits_txnID = g_tr.get("l2.tx_req.bits_txnID", r);
        top->io_decoupledCHI_tx_req_bits_opcode = g_tr.get("l2.tx_req.bits_opcode", r);
        top->io_decoupledCHI_tx_req_bits_size = g_tr.get("l2.tx_req.bits_size", r);
        top->io_decoupledCHI_tx_req_bits_addr = g_tr.get("l2.tx_req.bits_addr", r);
        top->io_decoupledCHI_tx_req_bits_order = g_tr.get("l2.tx_req.bits_order", r);
        top->io_decoupledCHI_tx_req_bits_memAttr_allocate =
            g_tr.get("l2.tx_req.bits_memAttr_allocate", r);
        top->io_decoupledCHI_tx_req_bits_memAttr_cacheable =
            g_tr.get("l2.tx_req.bits_memAttr_cacheable", r);
        top->io_decoupledCHI_tx_req_bits_memAttr_device =
            g_tr.get("l2.tx_req.bits_memAttr_device", r);
        top->io_decoupledCHI_tx_req_bits_memAttr_ewa = g_tr.get("l2.tx_req.bits_memAttr_ewa", r);
        top->io_decoupledCHI_tx_req_bits_snpAttr = g_tr.get("l2.tx_req.bits_snpAttr", r);
        top->io_decoupledCHI_tx_req_bits_snoopMe = g_tr.get("l2.tx_req.bits_snoopMe", r);
        top->io_decoupledCHI_tx_req_bits_expCompAck = g_tr.get("l2.tx_req.bits_expCompAck", r);
        top->io_decoupledCHI_tx_req_bits_mpam_partID = g_tr.get("l2.tx_req.bits_mpam_partID", r);
        top->io_decoupledCHI_tx_req_bits_rsvdc = g_tr.get("l2.tx_req.bits_rsvdc", r);
        // trace 未覆盖、remap 忽略的字段：returnNID/stashNIDValid/returnTxnID/ns/
        // pCrdType/tagOp/traceTag/lpIDWithPadding/mpam_mpamNS/likelyShared/allowRetry —— 驱 0
        top->io_decoupledCHI_tx_req_bits_returnNID = 0;
        top->io_decoupledCHI_tx_req_bits_stashNIDValid = 0;
        top->io_decoupledCHI_tx_req_bits_returnTxnID = 0;
        top->io_decoupledCHI_tx_req_bits_ns = 0;
        top->io_decoupledCHI_tx_req_bits_pCrdType = 0;
        top->io_decoupledCHI_tx_req_bits_tagOp = 0;
        top->io_decoupledCHI_tx_req_bits_traceTag = 0;
        top->io_decoupledCHI_tx_req_bits_lpIDWithPadding = 0;
        top->io_decoupledCHI_tx_req_bits_mpam_mpamNS = 0;

        top->io_decoupledCHI_tx_rsp_valid = g_tr.get("l2.tx_rsp.valid", r);
        top->io_decoupledCHI_tx_rsp_bits_qos = g_tr.get("l2.tx_rsp.bits_qos", r);
        top->io_decoupledCHI_tx_rsp_bits_tgtID = g_tr.get("l2.tx_rsp.bits_tgtID", r);
        top->io_decoupledCHI_tx_rsp_bits_srcID = g_tr.get("l2.tx_rsp.bits_srcID", r);
        top->io_decoupledCHI_tx_rsp_bits_txnID = g_tr.get("l2.tx_rsp.bits_txnID", r);
        top->io_decoupledCHI_tx_rsp_bits_opcode = g_tr.get("l2.tx_rsp.bits_opcode", r);
        top->io_decoupledCHI_tx_rsp_bits_respErr = g_tr.get("l2.tx_rsp.bits_respErr", r);
        top->io_decoupledCHI_tx_rsp_bits_resp = g_tr.get("l2.tx_rsp.bits_resp", r);
        top->io_decoupledCHI_tx_rsp_bits_fwdState = g_tr.get("l2.tx_rsp.bits_fwdState", r);
        top->io_decoupledCHI_tx_rsp_bits_cBusy = g_tr.get("l2.tx_rsp.bits_cBusy", r);
        top->io_decoupledCHI_tx_rsp_bits_dbID = g_tr.get("l2.tx_rsp.bits_dbID", r);
        top->io_decoupledCHI_tx_rsp_bits_pCrdType = 0;
        top->io_decoupledCHI_tx_rsp_bits_tagOp = 0;
        top->io_decoupledCHI_tx_rsp_bits_traceTag = 0;

        top->io_decoupledCHI_tx_dat_valid = g_tr.get("l2.tx_dat.valid", r);
        top->io_decoupledCHI_tx_dat_bits_qos = g_tr.get("l2.tx_dat.bits_qos", r);
        top->io_decoupledCHI_tx_dat_bits_tgtID = g_tr.get("l2.tx_dat.bits_tgtID", r);
        top->io_decoupledCHI_tx_dat_bits_srcID = g_tr.get("l2.tx_dat.bits_srcID", r);
        top->io_decoupledCHI_tx_dat_bits_txnID = g_tr.get("l2.tx_dat.bits_txnID", r);
        top->io_decoupledCHI_tx_dat_bits_homeNID = g_tr.get("l2.tx_dat.bits_homeNID", r);
        top->io_decoupledCHI_tx_dat_bits_opcode = g_tr.get("l2.tx_dat.bits_opcode", r);
        top->io_decoupledCHI_tx_dat_bits_respErr = g_tr.get("l2.tx_dat.bits_respErr", r);
        top->io_decoupledCHI_tx_dat_bits_resp = g_tr.get("l2.tx_dat.bits_resp", r);
        top->io_decoupledCHI_tx_dat_bits_dataSource = g_tr.get("l2.tx_dat.bits_dataSource", r);
        top->io_decoupledCHI_tx_dat_bits_cBusy = g_tr.get("l2.tx_dat.bits_cBusy", r);
        top->io_decoupledCHI_tx_dat_bits_dbID = g_tr.get("l2.tx_dat.bits_dbID", r);
        top->io_decoupledCHI_tx_dat_bits_dataID = g_tr.get("l2.tx_dat.bits_dataID", r);
        top->io_decoupledCHI_tx_dat_bits_be = g_tr.get("l2.tx_dat.bits_be", r);
        g_tr.getWide("l2.tx_dat.bits_data", r, dw);
        setWide(top->io_decoupledCHI_tx_dat_bits_data, dw);
        top->io_decoupledCHI_tx_dat_bits_ccID = 0;
        top->io_decoupledCHI_tx_dat_bits_tag = 0;
        top->io_decoupledCHI_tx_dat_bits_tu = 0;
        top->io_decoupledCHI_tx_dat_bits_traceTag = 0;
        top->io_decoupledCHI_tx_dat_bits_rsvdc = 0;

        top->io_decoupledCHI_rx_rsp_ready = g_tr.get("l2.rx_rsp.ready", r);
        top->io_decoupledCHI_rx_dat_ready = g_tr.get("l2.rx_dat.ready", r);
        top->io_decoupledCHI_rx_snp_ready = g_tr.get("l2.rx_snp.ready", r);

        for (const char* side : {"mem", "cfg"}) {
            const bool isMem = side[0] == 'm';
            if (isMem) {
                top->m_axi_mem_0_awready = colGet(side, "aw", "ready", r);
                top->m_axi_mem_0_wready  = colGet(side, "w", "ready", r);
                top->m_axi_mem_0_arready = colGet(side, "ar", "ready", r);
                top->m_axi_mem_0_bvalid  = colGet(side, "b", "valid", r);
                top->m_axi_mem_0_bid     = colGet(side, "b", "id", r);
                top->m_axi_mem_0_bresp   = colGet(side, "b", "resp", r);
                top->m_axi_mem_0_rvalid  = colGet(side, "r", "valid", r);
                top->m_axi_mem_0_rid     = colGet(side, "r", "id", r);
                top->m_axi_mem_0_rresp   = colGet(side, "r", "resp", r);
                top->m_axi_mem_0_rlast   = colGet(side, "r", "last", r);
                char base[64];
                std::snprintf(base, sizeof base, "%s.r.data", side);
                g_tr.getWide(base, r, dw);
                setWide(top->m_axi_mem_0_rdata, dw);
            } else {
                top->m_axi_main_awready = colGet(side, "aw", "ready", r);
                top->m_axi_main_wready  = colGet(side, "w", "ready", r);
                top->m_axi_main_arready = colGet(side, "ar", "ready", r);
                top->m_axi_main_bvalid  = colGet(side, "b", "valid", r);
                top->m_axi_main_bid     = colGet(side, "b", "id", r);
                top->m_axi_main_bresp   = colGet(side, "b", "resp", r);
                top->m_axi_main_rvalid  = colGet(side, "r", "valid", r);
                top->m_axi_main_rid     = colGet(side, "r", "id", r);
                top->m_axi_main_rresp   = colGet(side, "r", "resp", r);
                top->m_axi_main_rlast   = colGet(side, "r", "last", r);
                char base[64];
                std::snprintf(base, sizeof base, "%s.r.data", side);
                g_tr.getWide(base, r, dw);
                setWide(top->m_axi_main_rdata, dw);
            }
        }
    }

    void check(size_t r) {
        uint64_t dw[4];
        sb.cmp("chi_tx_req", "ready", g_tr.get("l2.tx_req.ready", r),
               top->io_decoupledCHI_tx_req_ready);
        sb.cmp("chi_tx_rsp", "ready", g_tr.get("l2.tx_rsp.ready", r),
               top->io_decoupledCHI_tx_rsp_ready);
        sb.cmp("chi_tx_dat", "ready", g_tr.get("l2.tx_dat.ready", r),
               top->io_decoupledCHI_tx_dat_ready);

        // rx_rsp（valid=0 时 bits 不比）
        sb.cmp("chi_rx_rsp", "valid", g_tr.get("l2.rx_rsp.valid", r),
               top->io_decoupledCHI_rx_rsp_valid);
        if (g_tr.get("l2.rx_rsp.valid", r)) {
            sb.cmp("chi_rx_rsp", "qos", g_tr.get("l2.rx_rsp.bits_qos", r),
                   top->io_decoupledCHI_rx_rsp_bits_qos);
            sb.cmp("chi_rx_rsp", "srcID", g_tr.get("l2.rx_rsp.bits_srcID", r),
                   top->io_decoupledCHI_rx_rsp_bits_srcID);
            sb.cmp("chi_rx_rsp", "txnID", g_tr.get("l2.rx_rsp.bits_txnID", r),
                   top->io_decoupledCHI_rx_rsp_bits_txnID);
            sb.cmp("chi_rx_rsp", "opcode", g_tr.get("l2.rx_rsp.bits_opcode", r),
                   top->io_decoupledCHI_rx_rsp_bits_opcode);
            sb.cmp("chi_rx_rsp", "respErr", g_tr.get("l2.rx_rsp.bits_respErr", r),
                   top->io_decoupledCHI_rx_rsp_bits_respErr);
            sb.cmp("chi_rx_rsp", "resp", g_tr.get("l2.rx_rsp.bits_resp", r),
                   top->io_decoupledCHI_rx_rsp_bits_resp);
            sb.cmp("chi_rx_rsp", "fwdState", g_tr.get("l2.rx_rsp.bits_fwdState", r),
                   top->io_decoupledCHI_rx_rsp_bits_fwdState);
            sb.cmp("chi_rx_rsp", "cBusy", g_tr.get("l2.rx_rsp.bits_cBusy", r),
                   top->io_decoupledCHI_rx_rsp_bits_cBusy);
            sb.cmp("chi_rx_rsp", "dbID", g_tr.get("l2.rx_rsp.bits_dbID", r),
                   top->io_decoupledCHI_rx_rsp_bits_dbID);
        }

        // rx_dat
        sb.cmp("chi_rx_dat", "valid", g_tr.get("l2.rx_dat.valid", r),
               top->io_decoupledCHI_rx_dat_valid);
        if (g_tr.get("l2.rx_dat.valid", r)) {
            sb.cmp("chi_rx_dat", "qos", g_tr.get("l2.rx_dat.bits_qos", r),
                   top->io_decoupledCHI_rx_dat_bits_qos);
            sb.cmp("chi_rx_dat", "tgtID", g_tr.get("l2.rx_dat.bits_tgtID", r),
                   top->io_decoupledCHI_rx_dat_bits_tgtID);
            sb.cmp("chi_rx_dat", "srcID", g_tr.get("l2.rx_dat.bits_srcID", r),
                   top->io_decoupledCHI_rx_dat_bits_srcID);
            sb.cmp("chi_rx_dat", "txnID", g_tr.get("l2.rx_dat.bits_txnID", r),
                   top->io_decoupledCHI_rx_dat_bits_txnID);
            sb.cmp("chi_rx_dat", "homeNID", g_tr.get("l2.rx_dat.bits_homeNID", r),
                   top->io_decoupledCHI_rx_dat_bits_homeNID);
            sb.cmp("chi_rx_dat", "opcode", g_tr.get("l2.rx_dat.bits_opcode", r),
                   top->io_decoupledCHI_rx_dat_bits_opcode);
            sb.cmp("chi_rx_dat", "respErr", g_tr.get("l2.rx_dat.bits_respErr", r),
                   top->io_decoupledCHI_rx_dat_bits_respErr);
            sb.cmp("chi_rx_dat", "resp", g_tr.get("l2.rx_dat.bits_resp", r),
                   top->io_decoupledCHI_rx_dat_bits_resp);
            sb.cmp("chi_rx_dat", "dataSource", g_tr.get("l2.rx_dat.bits_dataSource", r),
                   top->io_decoupledCHI_rx_dat_bits_dataSource);
            sb.cmp("chi_rx_dat", "cBusy", g_tr.get("l2.rx_dat.bits_cBusy", r),
                   top->io_decoupledCHI_rx_dat_bits_cBusy);
            sb.cmp("chi_rx_dat", "dbID", g_tr.get("l2.rx_dat.bits_dbID", r),
                   top->io_decoupledCHI_rx_dat_bits_dbID);
            sb.cmp("chi_rx_dat", "dataID", g_tr.get("l2.rx_dat.bits_dataID", r),
                   top->io_decoupledCHI_rx_dat_bits_dataID);
            sb.cmp("chi_rx_dat", "be", g_tr.get("l2.rx_dat.bits_be", r),
                   top->io_decoupledCHI_rx_dat_bits_be);
            g_tr.getWide("l2.rx_dat.bits_data", r, dw);
            cmpWide(sb, "chi_rx_dat", "data", dw, top->io_decoupledCHI_rx_dat_bits_data);
        }

        // rx_snp
        sb.cmp("chi_rx_snp", "valid", g_tr.get("l2.rx_snp.valid", r),
               top->io_decoupledCHI_rx_snp_valid);
        if (g_tr.get("l2.rx_snp.valid", r)) {
            sb.cmp("chi_rx_snp", "qos", g_tr.get("l2.rx_snp.bits_qos", r),
                   top->io_decoupledCHI_rx_snp_bits_qos);
            sb.cmp("chi_rx_snp", "srcID", g_tr.get("l2.rx_snp.bits_srcID", r),
                   top->io_decoupledCHI_rx_snp_bits_srcID);
            sb.cmp("chi_rx_snp", "txnID", g_tr.get("l2.rx_snp.bits_txnID", r),
                   top->io_decoupledCHI_rx_snp_bits_txnID);
            sb.cmp("chi_rx_snp", "fwdNID", g_tr.get("l2.rx_snp.bits_fwdNID", r),
                   top->io_decoupledCHI_rx_snp_bits_fwdNID);
            sb.cmp("chi_rx_snp", "fwdTxnID", g_tr.get("l2.rx_snp.bits_fwdTxnID", r),
                   top->io_decoupledCHI_rx_snp_bits_fwdTxnID);
            sb.cmp("chi_rx_snp", "opcode", g_tr.get("l2.rx_snp.bits_opcode", r),
                   top->io_decoupledCHI_rx_snp_bits_opcode);
            sb.cmp("chi_rx_snp", "addr", g_tr.get("l2.rx_snp.bits_addr", r),
                   top->io_decoupledCHI_rx_snp_bits_addr);
            sb.cmp("chi_rx_snp", "doNotGoToSD", g_tr.get("l2.rx_snp.bits_doNotGoToSD", r),
                   top->io_decoupledCHI_rx_snp_bits_doNotGoToSD);
            sb.cmp("chi_rx_snp", "retToSrc", g_tr.get("l2.rx_snp.bits_retToSrc", r),
                   top->io_decoupledCHI_rx_snp_bits_retToSrc);
        }

        // AXI aw/w/ar 输出 + b/r ready 输出（mem/cfg 同构，宏展开两遍）
#define CMP_AXI(side, P)                                                                          \
    do {                                                                                          \
        sb.cmp(#side ".aw", "valid", colGet(#side, "aw", "valid", r), top->P##awvalid);           \
        if (colGet(#side, "aw", "valid", r)) {                                                    \
            sb.cmp(#side ".aw", "id", colGet(#side, "aw", "id", r), top->P##awid);                \
            sb.cmp(#side ".aw", "addr", colGet(#side, "aw", "addr", r), top->P##awaddr);          \
            sb.cmp(#side ".aw", "len", colGet(#side, "aw", "len", r), top->P##awlen);             \
            sb.cmp(#side ".aw", "size", colGet(#side, "aw", "size", r), top->P##awsize);          \
            sb.cmp(#side ".aw", "burst", colGet(#side, "aw", "burst", r), top->P##awburst);       \
            sb.cmp(#side ".aw", "lock", colGet(#side, "aw", "lock", r), top->P##awlock);          \
            sb.cmp(#side ".aw", "cache", colGet(#side, "aw", "cache", r), top->P##awcache);       \
            sb.cmp(#side ".aw", "prot", colGet(#side, "aw", "prot", r), top->P##awprot);          \
            sb.cmp(#side ".aw", "qos", colGet(#side, "aw", "qos", r), top->P##awqos);             \
        }                                                                                         \
        sb.cmp(#side ".w", "valid", colGet(#side, "w", "valid", r), top->P##wvalid);              \
        if (colGet(#side, "w", "valid", r)) {                                                     \
            char base[64];                                                                        \
            std::snprintf(base, sizeof base, "%s.w.data", #side);                                 \
            g_tr.getWide(base, r, dw);                                                            \
            cmpWide(sb, #side ".w", "data", dw, top->P##wdata);                                   \
            sb.cmp(#side ".w", "strb", colGet(#side, "w", "strb", r), top->P##wstrb);             \
            sb.cmp(#side ".w", "last", colGet(#side, "w", "last", r), top->P##wlast);             \
        }                                                                                         \
        sb.cmp(#side ".ar", "valid", colGet(#side, "ar", "valid", r), top->P##arvalid);           \
        if (colGet(#side, "ar", "valid", r)) {                                                    \
            sb.cmp(#side ".ar", "id", colGet(#side, "ar", "id", r), top->P##arid);                \
            sb.cmp(#side ".ar", "addr", colGet(#side, "ar", "addr", r), top->P##araddr);          \
            sb.cmp(#side ".ar", "len", colGet(#side, "ar", "len", r), top->P##arlen);             \
            sb.cmp(#side ".ar", "size", colGet(#side, "ar", "size", r), top->P##arsize);          \
            sb.cmp(#side ".ar", "burst", colGet(#side, "ar", "burst", r), top->P##arburst);       \
            sb.cmp(#side ".ar", "lock", colGet(#side, "ar", "lock", r), top->P##arlock);          \
            sb.cmp(#side ".ar", "cache", colGet(#side, "ar", "cache", r), top->P##arcache);       \
            sb.cmp(#side ".ar", "prot", colGet(#side, "ar", "prot", r), top->P##arprot);          \
            sb.cmp(#side ".ar", "qos", colGet(#side, "ar", "qos", r), top->P##arqos);             \
        }                                                                                         \
        sb.cmp(#side ".b", "ready", colGet(#side, "b", "ready", r), top->P##bready);              \
        sb.cmp(#side ".r", "ready", colGet(#side, "r", "ready", r), top->P##rready);              \
    } while (0)

        CMP_AXI(mem, m_axi_mem_0_);
        CMP_AXI(cfg, m_axi_main_);
#undef CMP_AXI
    }
};

// ---------------- wolvic 侧：WolvicZjTop 裸模型（无 DPI） ----------------

void drvChiReq(wolvicmod::In<Valid<xs::CHIREQ>>& p, size_t r) {
    xs::CHIREQ f;
    f.qos                = g_tr.get("l2.tx_req.bits_qos", r);
    f.tgt_id             = g_tr.get("l2.tx_req.bits_tgtID", r);
    f.src_id             = g_tr.get("l2.tx_req.bits_srcID", r);
    f.txn_id             = g_tr.get("l2.tx_req.bits_txnID", r);
    f.opcode             = g_tr.get("l2.tx_req.bits_opcode", r);
    f.size               = g_tr.get("l2.tx_req.bits_size", r);
    f.addr               = g_tr.get("l2.tx_req.bits_addr", r);
    f.order              = g_tr.get("l2.tx_req.bits_order", r);
    f.mem_attr_allocate  = g_tr.get("l2.tx_req.bits_memAttr_allocate", r);
    f.mem_attr_cacheable = g_tr.get("l2.tx_req.bits_memAttr_cacheable", r);
    f.mem_attr_device    = g_tr.get("l2.tx_req.bits_memAttr_device", r);
    f.mem_attr_ewa       = g_tr.get("l2.tx_req.bits_memAttr_ewa", r);
    f.snp_attr           = g_tr.get("l2.tx_req.bits_snpAttr", r);
    f.snoop_me           = g_tr.get("l2.tx_req.bits_snoopMe", r);
    f.exp_comp_ack       = g_tr.get("l2.tx_req.bits_expCompAck", r);
    f.mpam_part_id       = g_tr.get("l2.tx_req.bits_mpam_partID", r);
    f.rsvdc              = g_tr.get("l2.tx_req.bits_rsvdc", r);
    p.set(Valid<xs::CHIREQ>{g_tr.get("l2.tx_req.valid", r) != 0, f});
}

void drvChiRsp(wolvicmod::In<Valid<xs::CHIRSP>>& p, size_t r) {
    xs::CHIRSP f;
    f.qos       = g_tr.get("l2.tx_rsp.bits_qos", r);
    f.tgt_id    = g_tr.get("l2.tx_rsp.bits_tgtID", r);
    f.src_id    = g_tr.get("l2.tx_rsp.bits_srcID", r);
    f.txn_id    = g_tr.get("l2.tx_rsp.bits_txnID", r);
    f.opcode    = g_tr.get("l2.tx_rsp.bits_opcode", r);
    f.resp_err  = g_tr.get("l2.tx_rsp.bits_respErr", r);
    f.resp      = g_tr.get("l2.tx_rsp.bits_resp", r);
    f.fwd_state = g_tr.get("l2.tx_rsp.bits_fwdState", r);
    f.c_busy    = g_tr.get("l2.tx_rsp.bits_cBusy", r);
    f.dbid      = g_tr.get("l2.tx_rsp.bits_dbID", r);
    p.set(Valid<xs::CHIRSP>{g_tr.get("l2.tx_rsp.valid", r) != 0, f});
}

void drvChiDat(wolvicmod::In<Valid<xs::CHIDAT>>& p, size_t r) {
    xs::CHIDAT f;
    f.qos         = g_tr.get("l2.tx_dat.bits_qos", r);
    f.tgt_id      = g_tr.get("l2.tx_dat.bits_tgtID", r);
    f.src_id      = g_tr.get("l2.tx_dat.bits_srcID", r);
    f.txn_id      = g_tr.get("l2.tx_dat.bits_txnID", r);
    f.home_nid    = g_tr.get("l2.tx_dat.bits_homeNID", r);
    f.opcode      = g_tr.get("l2.tx_dat.bits_opcode", r);
    f.resp_err    = g_tr.get("l2.tx_dat.bits_respErr", r);
    f.resp        = g_tr.get("l2.tx_dat.bits_resp", r);
    f.data_source = g_tr.get("l2.tx_dat.bits_dataSource", r);
    f.c_busy      = g_tr.get("l2.tx_dat.bits_cBusy", r);
    f.dbid        = g_tr.get("l2.tx_dat.bits_dbID", r);
    f.data_id     = g_tr.get("l2.tx_dat.bits_dataID", r);
    f.be          = g_tr.get("l2.tx_dat.bits_be", r);
    uint64_t dw[4];
    g_tr.getWide("l2.tx_dat.bits_data", r, dw);
    for (int i = 0; i < 4; ++i) f.data[i] = dw[i];
    p.set(Valid<xs::CHIDAT>{g_tr.get("l2.tx_dat.valid", r) != 0, f});
}

void drvAxiB(wolvicmod::In<Valid<axi::BFlit>>& p, const char* side, size_t r) {
    axi::BFlit f;
    f.id   = colGet(side, "b", "id", r);
    f.resp = colGet(side, "b", "resp", r);
    p.set(Valid<axi::BFlit>{colGet(side, "b", "valid", r) != 0, f});
}

void drvAxiR(wolvicmod::In<Valid<axi::RFlit>>& p, const char* side, size_t r) {
    axi::RFlit f;
    f.id   = colGet(side, "r", "id", r);
    f.resp = colGet(side, "r", "resp", r);
    f.last = colGet(side, "r", "last", r) != 0;
    char base[64];
    std::snprintf(base, sizeof base, "%s.r.data", side);
    uint64_t dw[4];
    g_tr.getWide(base, r, dw);
    for (int i = 0; i < 4; ++i) f.data[i] = dw[i];
    p.set(Valid<axi::RFlit>{colGet(side, "r", "valid", r) != 0, f});
}

// ---- 比对（valid=0 时 bits don't-care）----
void cmpChiRsp(Score& sb, const char* port, const Valid<xs::CHIRSP>& p, size_t r) {
    sb.cmp(port, "valid", g_tr.get("l2.rx_rsp.valid", r), p.valid ? 1 : 0);
    if (!g_tr.get("l2.rx_rsp.valid", r)) return;
    sb.cmp(port, "qos", g_tr.get("l2.rx_rsp.bits_qos", r), p.bits.qos);
    sb.cmp(port, "srcID", g_tr.get("l2.rx_rsp.bits_srcID", r), p.bits.src_id);
    sb.cmp(port, "txnID", g_tr.get("l2.rx_rsp.bits_txnID", r), p.bits.txn_id);
    sb.cmp(port, "opcode", g_tr.get("l2.rx_rsp.bits_opcode", r), p.bits.opcode);
    sb.cmp(port, "respErr", g_tr.get("l2.rx_rsp.bits_respErr", r), p.bits.resp_err);
    sb.cmp(port, "resp", g_tr.get("l2.rx_rsp.bits_resp", r), p.bits.resp);
    sb.cmp(port, "fwdState", g_tr.get("l2.rx_rsp.bits_fwdState", r), p.bits.fwd_state);
    sb.cmp(port, "cBusy", g_tr.get("l2.rx_rsp.bits_cBusy", r), p.bits.c_busy);
    sb.cmp(port, "dbID", g_tr.get("l2.rx_rsp.bits_dbID", r), p.bits.dbid);
    // rx_rsp 无 tgtID 列（RTL 被 firtool 裁剪，mapRspXs 亦不回填），不比
}

void cmpChiDat(Score& sb, const char* port, const Valid<xs::CHIDAT>& p, size_t r) {
    sb.cmp(port, "valid", g_tr.get("l2.rx_dat.valid", r), p.valid ? 1 : 0);
    if (!g_tr.get("l2.rx_dat.valid", r)) return;
    sb.cmp(port, "qos", g_tr.get("l2.rx_dat.bits_qos", r), p.bits.qos);
    sb.cmp(port, "tgtID", g_tr.get("l2.rx_dat.bits_tgtID", r), p.bits.tgt_id);
    sb.cmp(port, "srcID", g_tr.get("l2.rx_dat.bits_srcID", r), p.bits.src_id);
    sb.cmp(port, "txnID", g_tr.get("l2.rx_dat.bits_txnID", r), p.bits.txn_id);
    sb.cmp(port, "homeNID", g_tr.get("l2.rx_dat.bits_homeNID", r), p.bits.home_nid);
    sb.cmp(port, "opcode", g_tr.get("l2.rx_dat.bits_opcode", r), p.bits.opcode);
    sb.cmp(port, "respErr", g_tr.get("l2.rx_dat.bits_respErr", r), p.bits.resp_err);
    sb.cmp(port, "resp", g_tr.get("l2.rx_dat.bits_resp", r), p.bits.resp);
    sb.cmp(port, "dataSource", g_tr.get("l2.rx_dat.bits_dataSource", r), p.bits.data_source);
    sb.cmp(port, "cBusy", g_tr.get("l2.rx_dat.bits_cBusy", r), p.bits.c_busy);
    sb.cmp(port, "dbID", g_tr.get("l2.rx_dat.bits_dbID", r), p.bits.dbid);
    sb.cmp(port, "dataID", g_tr.get("l2.rx_dat.bits_dataID", r), p.bits.data_id);
    sb.cmp(port, "be", g_tr.get("l2.rx_dat.bits_be", r), p.bits.be);
    uint64_t dw[4];
    g_tr.getWide("l2.rx_dat.bits_data", r, dw);
    for (int i = 0; i < 4; ++i) sb.cmp(port, "data", dw[i], p.bits.data[i]);
}

void cmpChiSnp(Score& sb, const char* port, const Valid<xs::CHISNP>& p, size_t r) {
    sb.cmp(port, "valid", g_tr.get("l2.rx_snp.valid", r), p.valid ? 1 : 0);
    if (!g_tr.get("l2.rx_snp.valid", r)) return;
    sb.cmp(port, "qos", g_tr.get("l2.rx_snp.bits_qos", r), p.bits.qos);
    sb.cmp(port, "srcID", g_tr.get("l2.rx_snp.bits_srcID", r), p.bits.src_id);
    sb.cmp(port, "txnID", g_tr.get("l2.rx_snp.bits_txnID", r), p.bits.txn_id);
    sb.cmp(port, "fwdNID", g_tr.get("l2.rx_snp.bits_fwdNID", r), p.bits.fwd_nid);
    sb.cmp(port, "fwdTxnID", g_tr.get("l2.rx_snp.bits_fwdTxnID", r), p.bits.fwd_txn_id);
    sb.cmp(port, "opcode", g_tr.get("l2.rx_snp.bits_opcode", r), p.bits.opcode);
    sb.cmp(port, "addr", g_tr.get("l2.rx_snp.bits_addr", r), p.bits.addr);
    sb.cmp(port, "doNotGoToSD", g_tr.get("l2.rx_snp.bits_doNotGoToSD", r),
           p.bits.do_not_go_to_sd ? 1 : 0);
    sb.cmp(port, "retToSrc", g_tr.get("l2.rx_snp.bits_retToSrc", r), p.bits.ret_to_src ? 1 : 0);
}

void cmpAxiAx(Score& sb, const char* port, const Valid<axi::AxFlit>& p, const char* side,
              const char* chan, size_t r) {
    sb.cmp(port, "valid", colGet(side, chan, "valid", r), p.valid ? 1 : 0);
    if (!colGet(side, chan, "valid", r)) return;
    sb.cmp(port, "id", colGet(side, chan, "id", r), p.bits.id);
    sb.cmp(port, "addr", colGet(side, chan, "addr", r), p.bits.addr);
    sb.cmp(port, "len", colGet(side, chan, "len", r), p.bits.len);
    sb.cmp(port, "size", colGet(side, chan, "size", r), p.bits.size);
    sb.cmp(port, "burst", colGet(side, chan, "burst", r), p.bits.burst);
    sb.cmp(port, "lock", colGet(side, chan, "lock", r), p.bits.lock ? 1 : 0);
    sb.cmp(port, "cache", colGet(side, chan, "cache", r), p.bits.cache);
    sb.cmp(port, "prot", colGet(side, chan, "prot", r), p.bits.prot);
    sb.cmp(port, "qos", colGet(side, chan, "qos", r), p.bits.qos);
}

void cmpAxiW(Score& sb, const char* port, const Valid<axi::WFlit>& p, const char* side, size_t r) {
    sb.cmp(port, "valid", colGet(side, "w", "valid", r), p.valid ? 1 : 0);
    if (!colGet(side, "w", "valid", r)) return;
    char base[64];
    std::snprintf(base, sizeof base, "%s.w.data", side);
    uint64_t dw[4];
    g_tr.getWide(base, r, dw);
    for (int i = 0; i < 4; ++i) sb.cmp(port, "data", dw[i], p.bits.data[i]);
    sb.cmp(port, "strb", colGet(side, "w", "strb", r), p.bits.strb);
    sb.cmp(port, "last", colGet(side, "w", "last", r), p.bits.last ? 1 : 0);
}

struct WvSide {
    std::unique_ptr<WolvicZjTop> top;
    Score                        sb{"wolvic"};
    double                       tEval0 = 0, tEval1 = 0;

    void create() {
        top = std::make_unique<WolvicZjTop>();
        top->elaborate();
        top->ci.set(0);  // 单核恒 0
    }
    void eval0() {
        top->clk.set(0);
        top->eval();
    }
    void eval1() {
        top->clk.set(1);
        top->eval();
    }

    void drive(size_t r) {
        drvChiReq(top->chi_tx_req, r);
        drvChiRsp(top->chi_tx_rsp, r);
        drvChiDat(top->chi_tx_dat, r);
        top->chi_rx_rsp_rdy.set(g_tr.get("l2.rx_rsp.ready", r) != 0);
        top->chi_rx_dat_rdy.set(g_tr.get("l2.rx_dat.ready", r) != 0);
        top->chi_rx_snp_rdy.set(g_tr.get("l2.rx_snp.ready", r) != 0);
        top->mem_aw_rdy.set(colGet("mem", "aw", "ready", r) != 0);
        top->mem_w_rdy.set(colGet("mem", "w", "ready", r) != 0);
        top->mem_ar_rdy.set(colGet("mem", "ar", "ready", r) != 0);
        drvAxiB(top->mem_b, "mem", r);
        drvAxiR(top->mem_r, "mem", r);
        top->cfg_aw_rdy.set(colGet("cfg", "aw", "ready", r) != 0);
        top->cfg_w_rdy.set(colGet("cfg", "w", "ready", r) != 0);
        top->cfg_ar_rdy.set(colGet("cfg", "ar", "ready", r) != 0);
        drvAxiB(top->cfg_b, "cfg", r);
        drvAxiR(top->cfg_r, "cfg", r);
    }

    void check(size_t r) {
        sb.cmp("chi_tx_req", "ready", g_tr.get("l2.tx_req.ready", r),
               top->chi_tx_req_rdy.get() ? 1 : 0);
        sb.cmp("chi_tx_rsp", "ready", g_tr.get("l2.tx_rsp.ready", r),
               top->chi_tx_rsp_rdy.get() ? 1 : 0);
        sb.cmp("chi_tx_dat", "ready", g_tr.get("l2.tx_dat.ready", r),
               top->chi_tx_dat_rdy.get() ? 1 : 0);
        cmpChiRsp(sb, "chi_rx_rsp", top->chi_rx_rsp.get(), r);
        cmpChiDat(sb, "chi_rx_dat", top->chi_rx_dat.get(), r);
        cmpChiSnp(sb, "chi_rx_snp", top->chi_rx_snp.get(), r);
        cmpAxiAx(sb, "mem.aw", top->mem_aw.get(), "mem", "aw", r);
        cmpAxiW(sb, "mem.w", top->mem_w.get(), "mem", r);
        cmpAxiAx(sb, "mem.ar", top->mem_ar.get(), "mem", "ar", r);
        sb.cmp("mem.b", "ready", colGet("mem", "b", "ready", r), top->mem_b_rdy.get() ? 1 : 0);
        sb.cmp("mem.r", "ready", colGet("mem", "r", "ready", r), top->mem_r_rdy.get() ? 1 : 0);
        cmpAxiAx(sb, "cfg.aw", top->cfg_aw.get(), "cfg", "aw", r);
        cmpAxiW(sb, "cfg.w", top->cfg_w.get(), "cfg", r);
        cmpAxiAx(sb, "cfg.ar", top->cfg_ar.get(), "cfg", "ar", r);
        sb.cmp("cfg.b", "ready", colGet("cfg", "b", "ready", r), top->cfg_b_rdy.get() ? 1 : 0);
        sb.cmp("cfg.r", "ready", colGet("cfg", "r", "ready", r), top->cfg_r_rdy.get() ? 1 : 0);
        sb.cmp("cc_tx_req", "valid", 0, top->cc_tx_req.get().valid ? 1 : 0);
    }
};

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    const char* path    = nullptr;
    std::string mode    = "both";
    long        preroll = 2000;
    size_t      maxRows = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--dut=", 0) == 0)
            mode = a.substr(6);
        else if (a.rfind("--preroll=", 0) == 0)
            preroll = std::atol(a.c_str() + 10);
        else if (a.rfind("--rows=", 0) == 0)
            maxRows = size_t(std::atol(a.c_str() + 7));
        else if (!a.empty() && a[0] != '-')
            path = argv[i];
        else {
            std::fprintf(stderr,
                         "unknown arg: %s\nusage: zj_ab_replay [trace.txt] "
                         "[--dut=rtl|wolvic|both] [--rows=N] [--preroll=N]\n",
                         argv[i]);
            return 2;
        }
    }
    if (!path) path = "build/trace/cm_full.txt";
    const bool useRtl = mode == "rtl" || mode == "both";
    const bool useWv  = mode == "wolvic" || mode == "both";
    if (!useRtl && !useWv) {
        std::fprintf(stderr, "bad --dut=%s（rtl|wolvic|both）\n", mode.c_str());
        return 2;
    }
    if (!g_tr.load(path)) {
        std::fprintf(stderr, "trace not found: %s\n", path);
        return 2;
    }
    std::printf("trace: %s rows=%zu cols=%zu cyc[%lu..%lu]\n", path, g_tr.rows.size(),
                g_tr.cols.size(), (unsigned long)g_tr.cycs.front(),
                (unsigned long)g_tr.cycs.back());
    std::printf("mode=%s（两侧均不开波形；预滚仅 RTL 侧）\n", mode.c_str());

    RtlSide rtl;
    WvSide  wv;
    if (useRtl) {
        rtl.create();
        rtl.preroll(preroll);
    }
    if (useWv) wv.create();

    const size_t rows = (maxRows && maxRows < g_tr.rows.size()) ? maxRows : g_tr.rows.size();

    const double t0 = nowSec();
    for (size_t r = 0; r < rows; ++r) {
        g_cyc = g_tr.cycs[r];

        // ---- 驱动（trace 输入侧，两侧同一份激励）----
        if (useRtl) rtl.drive(r);
        if (useWv) wv.drive(r);

        // ---- clk=0 eval 采样比对 ----
        if (useRtl) {
            const double c = nowSec();
            rtl.eval0();
            rtl.tEval0 += nowSec() - c;
            rtl.check(r);
        }
        if (useWv) {
            const double c = nowSec();
            wv.eval0();
            wv.tEval0 += nowSec() - c;
            wv.check(r);
        }

        // ---- clk=1 eval 提交 ----
        if (useRtl) {
            const double c = nowSec();
            rtl.eval1();
            rtl.tEval1 += nowSec() - c;
        }
        if (useWv) {
            const double c = nowSec();
            wv.eval1();
            wv.tEval1 += nowSec() - c;
        }

        if (rtl.sb.mismatch > 100 || wv.sb.mismatch > 100) break;
    }
    const double wall = nowSec() - t0;

    double evalSum = 0;
    std::printf("loop: %.3fs over %zu rows = %.1f ns/row（驱动+eval+比对）\n", wall, rows,
                wall * 1e9 / rows);
    if (useRtl) {
        const double e = rtl.tEval0 + rtl.tEval1;
        evalSum += e;
        std::printf("[rtl]    checks=%lu mismatches=%lu  eval=%.3fs（clk0 %.3f + clk1 %.3f）"
                    " = %.1f ns/row\n",
                    (unsigned long)rtl.sb.checks, (unsigned long)rtl.sb.mismatch, e, rtl.tEval0,
                    rtl.tEval1, e * 1e9 / rows);
    }
    if (useWv) {
        const double e = wv.tEval0 + wv.tEval1;
        evalSum += e;
        std::printf("[wolvic] checks=%lu mismatches=%lu  eval=%.3fs（clk0 %.3f + clk1 %.3f）"
                    " = %.1f ns/row\n",
                    (unsigned long)wv.sb.checks, (unsigned long)wv.sb.mismatch, e, wv.tEval0,
                    wv.tEval1, e * 1e9 / rows);
    }
    std::printf("harness: %.3fs（驱动+比对+计时开销）\n", wall - evalSum);
    return (rtl.sb.mismatch || wv.sb.mismatch) ? 1 : 0;
}
