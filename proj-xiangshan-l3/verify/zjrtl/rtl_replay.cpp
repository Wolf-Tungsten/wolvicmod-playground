// zj_rtl_replay：独立 ZhuJiang RTL（ZhujiangReplayTop）的 coremark 全程 trace 回放。
//
// 与 tests/test_wolvic_top_replay.cpp 同一份 trace（build/trace/cm_full.txt）、
// 同一套驱动/比对口径（valid 门控、>64b 拆 64b 字），DUT 换成 verilated RTL——
// 得到排除 SoC/BlackBox 边界噪声的"RTL L3 vs wolvicmod L3"孤立对比。
//
// 用法：zj_rtl_replay [trace.txt]
//   环境变量：PREROLL=<N>  复位撤除后的空闲预滚拍数（默认 2000，覆盖环站复位传播）
//             MAX_ROWS=<N> 只回放前 N 行（调试）
// 输出：checks/mismatches + 回放循环墙钟与 ns/拍（循环口径 = 驱动+eval+比对，
// 与 wolvic 侧 test_wolvic_top_replay 的循环体一一对应）。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "VZhujiangReplayTop.h"
#include "verilated.h"

namespace {

// ---------------- trace 读取（与 test_wolvic_top_replay.cpp 同格式） ----------------
struct Trace {
    std::vector<std::string>                cols;
    std::unordered_map<std::string, size_t> idx;
    std::vector<std::vector<uint64_t>>      rows;
    std::vector<uint64_t>                   cycs;

    bool load(const char* path) {
        FILE* f = std::fopen(path, "r");
        if (!f) return false;
        std::string line;
        line.reserve(8192);
        int ch;
        auto readLine = [&](std::string& out) -> bool {
            out.clear();
            while ((ch = std::fgetc(f)) != EOF) {
                if (ch == '\n') return true;
                out.push_back(char(ch));
            }
            return !out.empty();
        };
        bool header = false;
        while (readLine(line)) {
            if (line.empty() || line[0] == '#') continue;
            std::vector<uint64_t> vals;
            std::vector<std::string> names;
            const char* p = line.c_str();
            while (*p) {
                while (*p == ' ' || *p == '\t') ++p;
                if (!*p) break;
                const char* tok = p;
                while (*p && *p != ' ' && *p != '\t') ++p;
                if (!header) {
                    names.emplace_back(tok, p - tok);
                } else {
                    uint64_t v = 0;
                    if (vals.empty()) {
                        for (const char* q = tok; q < p; ++q) v = v * 10 + unsigned(*q - '0');
                        vals.push_back(v);
                        continue;
                    }
                    for (const char* q = tok; q < p; ++q) {
                        char c = *q;
                        unsigned d = (c >= '0' && c <= '9')   ? unsigned(c - '0')
                                     : (c >= 'a' && c <= 'f') ? unsigned(c - 'a' + 10)
                                     : (c >= 'A' && c <= 'F') ? unsigned(c - 'A' + 10)
                                                               : 0;
                        v = (v << 4) | d;
                    }
                    vals.push_back(v);
                }
            }
            if (!header) {
                if (names.empty() || names[0] != "cyc") {
                    std::fprintf(stderr, "bad trace header in %s\n", path);
                    std::fclose(f);
                    return false;
                }
                cols.assign(names.begin() + 1, names.end());
                for (size_t i = 0; i < cols.size(); ++i) idx[cols[i]] = i;
                header = true;
            } else if (!vals.empty()) {
                cycs.push_back(vals[0]);
                rows.emplace_back(vals.begin() + 1, vals.end());
            }
        }
        std::fclose(f);
        return header && !rows.empty();
    }

    uint64_t get(const char* name, size_t r) const {
        auto it = idx.find(name);
        return it == idx.end() ? 0 : rows[r][it->second];
    }
    void getWide(const char* base, size_t r, uint64_t out[4]) const {
        char buf[96];
        for (int i = 0; i < 4; ++i) {
            std::snprintf(buf, sizeof buf, "%s_%d", base, i);
            out[i] = get(buf, r);
        }
    }
};

Trace    g_tr;
uint64_t g_checks = 0, g_mismatch = 0, g_cyc = 0;

void cmp(const char* port, const char* field, uint64_t ref, uint64_t dut) {
    ++g_checks;
    if (ref == dut) return;
    ++g_mismatch;
    if (g_mismatch <= 20)
        std::printf("MISMATCH cyc=%lu %s.%s ref=0x%llx dut=0x%llx\n", (unsigned long)g_cyc, port,
                    field, (unsigned long long)ref, (unsigned long long)dut);
}

uint64_t colGet(const char* side, const char* chan, const char* field, size_t r) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s.%s.%s", side, chan, field);
    return g_tr.get(buf, r);
}

// 64b 字 → Verilator VlWide（32b 小端字序）
template <typename W>
void setWide(W& port, const uint64_t v[4]) {
    for (int i = 0; i < 4; ++i) {
        port[2 * i]     = uint32_t(v[i]);
        port[2 * i + 1] = uint32_t(v[i] >> 32);
    }
}
template <typename W>
void cmpWide(const char* port, const char* field, const uint64_t ref[4], const W& dut) {
    for (int i = 0; i < 4; ++i) {
        uint64_t d = uint64_t(dut[2 * i]) | (uint64_t(dut[2 * i + 1]) << 32);
        cmp(port, field, ref[i], d);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    const char* path = argc > 1 ? argv[1] : "build/trace/cm_full.txt";
    if (!g_tr.load(path)) {
        std::fprintf(stderr, "trace not found: %s\n", path);
        return 2;
    }
    std::printf("trace: %s rows=%zu cols=%zu cyc[%lu..%lu]\n", path, g_tr.rows.size(),
                g_tr.cols.size(), (unsigned long)g_tr.cycs.front(),
                (unsigned long)g_tr.cycs.back());

    VZhujiangReplayTop* dut = new VZhujiangReplayTop;
    auto eval0 = [&] {
        dut->clock = 0;
        dut->eval();
    };
    auto eval1 = [&] {
        dut->clock = 1;
        dut->eval();
    };
    auto idleInputs = [&] {
        // valid 全 0、ready 全 1：吸收复位撤除后的链路初始化流量
        dut->io_decoupledCHI_tx_req_valid = 0;
        dut->io_decoupledCHI_tx_rsp_valid = 0;
        dut->io_decoupledCHI_tx_dat_valid = 0;
        dut->io_decoupledCHI_rx_rsp_ready = 1;
        dut->io_decoupledCHI_rx_dat_ready = 1;
        dut->io_decoupledCHI_rx_snp_ready = 1;
        dut->m_axi_mem_0_awready = 1;
        dut->m_axi_mem_0_wready  = 1;
        dut->m_axi_mem_0_arready = 1;
        dut->m_axi_mem_0_bvalid  = 0;
        dut->m_axi_mem_0_rvalid  = 0;
        dut->m_axi_main_awready  = 1;
        dut->m_axi_main_wready   = 1;
        dut->m_axi_main_arready  = 1;
        dut->m_axi_main_bvalid   = 0;
        dut->m_axi_main_rvalid   = 0;
    };

    // ---- 复位预滚：对齐 trace row 0（提取器跳过所有复位激活拍，含环站复位） ----
    const long preroll = std::getenv("PREROLL") ? std::atol(std::getenv("PREROLL")) : 2000;
    idleInputs();
    dut->reset = 1;
    for (int i = 0; i < 10; ++i) { eval0(); eval1(); }
    dut->reset = 0;
    for (long i = 0; i < preroll; ++i) { eval0(); eval1(); }
    std::printf("preroll done: on_reset=%u\n", unsigned(dut->on_reset));

    const size_t maxRows =
        std::getenv("MAX_ROWS") ? size_t(std::atol(std::getenv("MAX_ROWS"))) : g_tr.rows.size();

    auto t0 = std::chrono::steady_clock::now();
    double tEval0 = 0, tEval1 = 0;
    size_t rows = 0;
    for (size_t r = 0; r < g_tr.rows.size() && r < maxRows; ++r) {
        rows = r + 1;
        g_cyc = g_tr.cycs[r];

        // ---- 驱动（trace 输入侧）----
        dut->io_decoupledCHI_tx_req_valid = g_tr.get("l2.tx_req.valid", r);
        dut->io_decoupledCHI_tx_req_bits_qos = g_tr.get("l2.tx_req.bits_qos", r);
        dut->io_decoupledCHI_tx_req_bits_tgtID = g_tr.get("l2.tx_req.bits_tgtID", r);
        dut->io_decoupledCHI_tx_req_bits_srcID = g_tr.get("l2.tx_req.bits_srcID", r);
        dut->io_decoupledCHI_tx_req_bits_txnID = g_tr.get("l2.tx_req.bits_txnID", r);
        dut->io_decoupledCHI_tx_req_bits_opcode = g_tr.get("l2.tx_req.bits_opcode", r);
        dut->io_decoupledCHI_tx_req_bits_size = g_tr.get("l2.tx_req.bits_size", r);
        dut->io_decoupledCHI_tx_req_bits_addr = g_tr.get("l2.tx_req.bits_addr", r);
        dut->io_decoupledCHI_tx_req_bits_order = g_tr.get("l2.tx_req.bits_order", r);
        dut->io_decoupledCHI_tx_req_bits_memAttr_allocate =
            g_tr.get("l2.tx_req.bits_memAttr_allocate", r);
        dut->io_decoupledCHI_tx_req_bits_memAttr_cacheable =
            g_tr.get("l2.tx_req.bits_memAttr_cacheable", r);
        dut->io_decoupledCHI_tx_req_bits_memAttr_device =
            g_tr.get("l2.tx_req.bits_memAttr_device", r);
        dut->io_decoupledCHI_tx_req_bits_memAttr_ewa = g_tr.get("l2.tx_req.bits_memAttr_ewa", r);
        dut->io_decoupledCHI_tx_req_bits_snpAttr = g_tr.get("l2.tx_req.bits_snpAttr", r);
        dut->io_decoupledCHI_tx_req_bits_snoopMe = g_tr.get("l2.tx_req.bits_snoopMe", r);
        dut->io_decoupledCHI_tx_req_bits_expCompAck = g_tr.get("l2.tx_req.bits_expCompAck", r);
        dut->io_decoupledCHI_tx_req_bits_mpam_partID = g_tr.get("l2.tx_req.bits_mpam_partID", r);
        dut->io_decoupledCHI_tx_req_bits_rsvdc = g_tr.get("l2.tx_req.bits_rsvdc", r);
        // trace 未覆盖、remap 忽略的字段：returnNID/stashNIDValid/returnTxnID/ns/
        // pCrdType/tagOp/traceTag/lpIDWithPadding/mpam_mpamNS/likelyShared/allowRetry —— 驱 0
        dut->io_decoupledCHI_tx_req_bits_returnNID = 0;
        dut->io_decoupledCHI_tx_req_bits_stashNIDValid = 0;
        dut->io_decoupledCHI_tx_req_bits_returnTxnID = 0;
        dut->io_decoupledCHI_tx_req_bits_ns = 0;
        dut->io_decoupledCHI_tx_req_bits_pCrdType = 0;
        dut->io_decoupledCHI_tx_req_bits_tagOp = 0;
        dut->io_decoupledCHI_tx_req_bits_traceTag = 0;
        dut->io_decoupledCHI_tx_req_bits_lpIDWithPadding = 0;
        dut->io_decoupledCHI_tx_req_bits_mpam_mpamNS = 0;

        dut->io_decoupledCHI_tx_rsp_valid = g_tr.get("l2.tx_rsp.valid", r);
        dut->io_decoupledCHI_tx_rsp_bits_qos = g_tr.get("l2.tx_rsp.bits_qos", r);
        dut->io_decoupledCHI_tx_rsp_bits_tgtID = g_tr.get("l2.tx_rsp.bits_tgtID", r);
        dut->io_decoupledCHI_tx_rsp_bits_srcID = g_tr.get("l2.tx_rsp.bits_srcID", r);
        dut->io_decoupledCHI_tx_rsp_bits_txnID = g_tr.get("l2.tx_rsp.bits_txnID", r);
        dut->io_decoupledCHI_tx_rsp_bits_opcode = g_tr.get("l2.tx_rsp.bits_opcode", r);
        dut->io_decoupledCHI_tx_rsp_bits_respErr = g_tr.get("l2.tx_rsp.bits_respErr", r);
        dut->io_decoupledCHI_tx_rsp_bits_resp = g_tr.get("l2.tx_rsp.bits_resp", r);
        dut->io_decoupledCHI_tx_rsp_bits_fwdState = g_tr.get("l2.tx_rsp.bits_fwdState", r);
        dut->io_decoupledCHI_tx_rsp_bits_cBusy = g_tr.get("l2.tx_rsp.bits_cBusy", r);
        dut->io_decoupledCHI_tx_rsp_bits_dbID = g_tr.get("l2.tx_rsp.bits_dbID", r);
        dut->io_decoupledCHI_tx_rsp_bits_pCrdType = 0;
        dut->io_decoupledCHI_tx_rsp_bits_tagOp = 0;
        dut->io_decoupledCHI_tx_rsp_bits_traceTag = 0;

        dut->io_decoupledCHI_tx_dat_valid = g_tr.get("l2.tx_dat.valid", r);
        dut->io_decoupledCHI_tx_dat_bits_qos = g_tr.get("l2.tx_dat.bits_qos", r);
        dut->io_decoupledCHI_tx_dat_bits_tgtID = g_tr.get("l2.tx_dat.bits_tgtID", r);
        dut->io_decoupledCHI_tx_dat_bits_srcID = g_tr.get("l2.tx_dat.bits_srcID", r);
        dut->io_decoupledCHI_tx_dat_bits_txnID = g_tr.get("l2.tx_dat.bits_txnID", r);
        dut->io_decoupledCHI_tx_dat_bits_homeNID = g_tr.get("l2.tx_dat.bits_homeNID", r);
        dut->io_decoupledCHI_tx_dat_bits_opcode = g_tr.get("l2.tx_dat.bits_opcode", r);
        dut->io_decoupledCHI_tx_dat_bits_respErr = g_tr.get("l2.tx_dat.bits_respErr", r);
        dut->io_decoupledCHI_tx_dat_bits_resp = g_tr.get("l2.tx_dat.bits_resp", r);
        dut->io_decoupledCHI_tx_dat_bits_dataSource = g_tr.get("l2.tx_dat.bits_dataSource", r);
        dut->io_decoupledCHI_tx_dat_bits_cBusy = g_tr.get("l2.tx_dat.bits_cBusy", r);
        dut->io_decoupledCHI_tx_dat_bits_dbID = g_tr.get("l2.tx_dat.bits_dbID", r);
        dut->io_decoupledCHI_tx_dat_bits_dataID = g_tr.get("l2.tx_dat.bits_dataID", r);
        dut->io_decoupledCHI_tx_dat_bits_be = g_tr.get("l2.tx_dat.bits_be", r);
        uint64_t d[4];
        g_tr.getWide("l2.tx_dat.bits_data", r, d);
        setWide(dut->io_decoupledCHI_tx_dat_bits_data, d);
        dut->io_decoupledCHI_tx_dat_bits_ccID = 0;
        dut->io_decoupledCHI_tx_dat_bits_tag = 0;
        dut->io_decoupledCHI_tx_dat_bits_tu = 0;
        dut->io_decoupledCHI_tx_dat_bits_traceTag = 0;
        dut->io_decoupledCHI_tx_dat_bits_rsvdc = 0;

        dut->io_decoupledCHI_rx_rsp_ready = g_tr.get("l2.rx_rsp.ready", r);
        dut->io_decoupledCHI_rx_dat_ready = g_tr.get("l2.rx_dat.ready", r);
        dut->io_decoupledCHI_rx_snp_ready = g_tr.get("l2.rx_snp.ready", r);

        for (const char* side : {"mem", "cfg"}) {
            const bool isMem = side[0] == 'm';
            // ready 输入
            if (isMem) {
                dut->m_axi_mem_0_awready = colGet(side, "aw", "ready", r);
                dut->m_axi_mem_0_wready  = colGet(side, "w", "ready", r);
                dut->m_axi_mem_0_arready = colGet(side, "ar", "ready", r);
                dut->m_axi_mem_0_bvalid  = colGet(side, "b", "valid", r);
                dut->m_axi_mem_0_bid     = colGet(side, "b", "id", r);
                dut->m_axi_mem_0_bresp   = colGet(side, "b", "resp", r);
                dut->m_axi_mem_0_rvalid  = colGet(side, "r", "valid", r);
                dut->m_axi_mem_0_rid     = colGet(side, "r", "id", r);
                dut->m_axi_mem_0_rresp   = colGet(side, "r", "resp", r);
                dut->m_axi_mem_0_rlast   = colGet(side, "r", "last", r);
                char base[64];
                std::snprintf(base, sizeof base, "%s.r.data", side);
                g_tr.getWide(base, r, d);
                setWide(dut->m_axi_mem_0_rdata, d);
            } else {
                dut->m_axi_main_awready = colGet(side, "aw", "ready", r);
                dut->m_axi_main_wready  = colGet(side, "w", "ready", r);
                dut->m_axi_main_arready = colGet(side, "ar", "ready", r);
                dut->m_axi_main_bvalid  = colGet(side, "b", "valid", r);
                dut->m_axi_main_bid     = colGet(side, "b", "id", r);
                dut->m_axi_main_bresp   = colGet(side, "b", "resp", r);
                dut->m_axi_main_rvalid  = colGet(side, "r", "valid", r);
                dut->m_axi_main_rid     = colGet(side, "r", "id", r);
                dut->m_axi_main_rresp   = colGet(side, "r", "resp", r);
                dut->m_axi_main_rlast   = colGet(side, "r", "last", r);
                char base[64];
                std::snprintf(base, sizeof base, "%s.r.data", side);
                g_tr.getWide(base, r, d);
                setWide(dut->m_axi_main_rdata, d);
            }
        }

        // ---- clk=0 eval 采样比对 ----
        auto c0 = std::chrono::steady_clock::now();
        eval0();
        auto c1 = std::chrono::steady_clock::now();
        tEval0 += std::chrono::duration<double>(c1 - c0).count();

        cmp("chi_tx_req", "ready", g_tr.get("l2.tx_req.ready", r), dut->io_decoupledCHI_tx_req_ready);
        cmp("chi_tx_rsp", "ready", g_tr.get("l2.tx_rsp.ready", r), dut->io_decoupledCHI_tx_rsp_ready);
        cmp("chi_tx_dat", "ready", g_tr.get("l2.tx_dat.ready", r), dut->io_decoupledCHI_tx_dat_ready);

        // rx_rsp（valid=0 时 bits 不比）
        cmp("chi_rx_rsp", "valid", g_tr.get("l2.rx_rsp.valid", r), dut->io_decoupledCHI_rx_rsp_valid);
        if (g_tr.get("l2.rx_rsp.valid", r)) {
            cmp("chi_rx_rsp", "qos", g_tr.get("l2.rx_rsp.bits_qos", r),
                dut->io_decoupledCHI_rx_rsp_bits_qos);
            cmp("chi_rx_rsp", "srcID", g_tr.get("l2.rx_rsp.bits_srcID", r),
                dut->io_decoupledCHI_rx_rsp_bits_srcID);
            cmp("chi_rx_rsp", "txnID", g_tr.get("l2.rx_rsp.bits_txnID", r),
                dut->io_decoupledCHI_rx_rsp_bits_txnID);
            cmp("chi_rx_rsp", "opcode", g_tr.get("l2.rx_rsp.bits_opcode", r),
                dut->io_decoupledCHI_rx_rsp_bits_opcode);
            cmp("chi_rx_rsp", "respErr", g_tr.get("l2.rx_rsp.bits_respErr", r),
                dut->io_decoupledCHI_rx_rsp_bits_respErr);
            cmp("chi_rx_rsp", "resp", g_tr.get("l2.rx_rsp.bits_resp", r),
                dut->io_decoupledCHI_rx_rsp_bits_resp);
            cmp("chi_rx_rsp", "fwdState", g_tr.get("l2.rx_rsp.bits_fwdState", r),
                dut->io_decoupledCHI_rx_rsp_bits_fwdState);
            cmp("chi_rx_rsp", "cBusy", g_tr.get("l2.rx_rsp.bits_cBusy", r),
                dut->io_decoupledCHI_rx_rsp_bits_cBusy);
            cmp("chi_rx_rsp", "dbID", g_tr.get("l2.rx_rsp.bits_dbID", r),
                dut->io_decoupledCHI_rx_rsp_bits_dbID);
        }

        // rx_dat
        cmp("chi_rx_dat", "valid", g_tr.get("l2.rx_dat.valid", r), dut->io_decoupledCHI_rx_dat_valid);
        if (g_tr.get("l2.rx_dat.valid", r)) {
            cmp("chi_rx_dat", "qos", g_tr.get("l2.rx_dat.bits_qos", r),
                dut->io_decoupledCHI_rx_dat_bits_qos);
            cmp("chi_rx_dat", "tgtID", g_tr.get("l2.rx_dat.bits_tgtID", r),
                dut->io_decoupledCHI_rx_dat_bits_tgtID);
            cmp("chi_rx_dat", "srcID", g_tr.get("l2.rx_dat.bits_srcID", r),
                dut->io_decoupledCHI_rx_dat_bits_srcID);
            cmp("chi_rx_dat", "txnID", g_tr.get("l2.rx_dat.bits_txnID", r),
                dut->io_decoupledCHI_rx_dat_bits_txnID);
            cmp("chi_rx_dat", "homeNID", g_tr.get("l2.rx_dat.bits_homeNID", r),
                dut->io_decoupledCHI_rx_dat_bits_homeNID);
            cmp("chi_rx_dat", "opcode", g_tr.get("l2.rx_dat.bits_opcode", r),
                dut->io_decoupledCHI_rx_dat_bits_opcode);
            cmp("chi_rx_dat", "respErr", g_tr.get("l2.rx_dat.bits_respErr", r),
                dut->io_decoupledCHI_rx_dat_bits_respErr);
            cmp("chi_rx_dat", "resp", g_tr.get("l2.rx_dat.bits_resp", r),
                dut->io_decoupledCHI_rx_dat_bits_resp);
            cmp("chi_rx_dat", "dataSource", g_tr.get("l2.rx_dat.bits_dataSource", r),
                dut->io_decoupledCHI_rx_dat_bits_dataSource);
            cmp("chi_rx_dat", "cBusy", g_tr.get("l2.rx_dat.bits_cBusy", r),
                dut->io_decoupledCHI_rx_dat_bits_cBusy);
            cmp("chi_rx_dat", "dbID", g_tr.get("l2.rx_dat.bits_dbID", r),
                dut->io_decoupledCHI_rx_dat_bits_dbID);
            cmp("chi_rx_dat", "dataID", g_tr.get("l2.rx_dat.bits_dataID", r),
                dut->io_decoupledCHI_rx_dat_bits_dataID);
            cmp("chi_rx_dat", "be", g_tr.get("l2.rx_dat.bits_be", r),
                dut->io_decoupledCHI_rx_dat_bits_be);
            g_tr.getWide("l2.rx_dat.bits_data", r, d);
            cmpWide("chi_rx_dat", "data", d, dut->io_decoupledCHI_rx_dat_bits_data);
        }

        // rx_snp
        cmp("chi_rx_snp", "valid", g_tr.get("l2.rx_snp.valid", r), dut->io_decoupledCHI_rx_snp_valid);
        if (g_tr.get("l2.rx_snp.valid", r)) {
            cmp("chi_rx_snp", "qos", g_tr.get("l2.rx_snp.bits_qos", r),
                dut->io_decoupledCHI_rx_snp_bits_qos);
            cmp("chi_rx_snp", "srcID", g_tr.get("l2.rx_snp.bits_srcID", r),
                dut->io_decoupledCHI_rx_snp_bits_srcID);
            cmp("chi_rx_snp", "txnID", g_tr.get("l2.rx_snp.bits_txnID", r),
                dut->io_decoupledCHI_rx_snp_bits_txnID);
            cmp("chi_rx_snp", "fwdNID", g_tr.get("l2.rx_snp.bits_fwdNID", r),
                dut->io_decoupledCHI_rx_snp_bits_fwdNID);
            cmp("chi_rx_snp", "fwdTxnID", g_tr.get("l2.rx_snp.bits_fwdTxnID", r),
                dut->io_decoupledCHI_rx_snp_bits_fwdTxnID);
            cmp("chi_rx_snp", "opcode", g_tr.get("l2.rx_snp.bits_opcode", r),
                dut->io_decoupledCHI_rx_snp_bits_opcode);
            cmp("chi_rx_snp", "addr", g_tr.get("l2.rx_snp.bits_addr", r),
                dut->io_decoupledCHI_rx_snp_bits_addr);
            cmp("chi_rx_snp", "doNotGoToSD", g_tr.get("l2.rx_snp.bits_doNotGoToSD", r),
                dut->io_decoupledCHI_rx_snp_bits_doNotGoToSD);
            cmp("chi_rx_snp", "retToSrc", g_tr.get("l2.rx_snp.bits_retToSrc", r),
                dut->io_decoupledCHI_rx_snp_bits_retToSrc);
        }

        // AXI aw/w/ar 输出 + b/r ready 输出（mem/cfg 同构，宏展开两遍）
#define CMP_AXI(side, P)                                                                          \
    do {                                                                                          \
        cmp(#side ".aw", "valid", colGet(#side, "aw", "valid", r), dut->P##awvalid);              \
        if (colGet(#side, "aw", "valid", r)) {                                                    \
            cmp(#side ".aw", "id", colGet(#side, "aw", "id", r), dut->P##awid);                   \
            cmp(#side ".aw", "addr", colGet(#side, "aw", "addr", r), dut->P##awaddr);             \
            cmp(#side ".aw", "len", colGet(#side, "aw", "len", r), dut->P##awlen);                \
            cmp(#side ".aw", "size", colGet(#side, "aw", "size", r), dut->P##awsize);             \
            cmp(#side ".aw", "burst", colGet(#side, "aw", "burst", r), dut->P##awburst);          \
            cmp(#side ".aw", "lock", colGet(#side, "aw", "lock", r), dut->P##awlock);             \
            cmp(#side ".aw", "cache", colGet(#side, "aw", "cache", r), dut->P##awcache);          \
            cmp(#side ".aw", "prot", colGet(#side, "aw", "prot", r), dut->P##awprot);             \
            cmp(#side ".aw", "qos", colGet(#side, "aw", "qos", r), dut->P##awqos);                \
        }                                                                                         \
        cmp(#side ".w", "valid", colGet(#side, "w", "valid", r), dut->P##wvalid);                 \
        if (colGet(#side, "w", "valid", r)) {                                                     \
            char base[64];                                                                        \
            std::snprintf(base, sizeof base, "%s.w.data", #side);                                 \
            g_tr.getWide(base, r, d);                                                             \
            cmpWide(#side ".w", "data", d, dut->P##wdata);                                        \
            cmp(#side ".w", "strb", colGet(#side, "w", "strb", r), dut->P##wstrb);                \
            cmp(#side ".w", "last", colGet(#side, "w", "last", r), dut->P##wlast);                \
        }                                                                                         \
        cmp(#side ".ar", "valid", colGet(#side, "ar", "valid", r), dut->P##arvalid);              \
        if (colGet(#side, "ar", "valid", r)) {                                                    \
            cmp(#side ".ar", "id", colGet(#side, "ar", "id", r), dut->P##arid);                   \
            cmp(#side ".ar", "addr", colGet(#side, "ar", "addr", r), dut->P##araddr);             \
            cmp(#side ".ar", "len", colGet(#side, "ar", "len", r), dut->P##arlen);                \
            cmp(#side ".ar", "size", colGet(#side, "ar", "size", r), dut->P##arsize);             \
            cmp(#side ".ar", "burst", colGet(#side, "ar", "burst", r), dut->P##arburst);          \
            cmp(#side ".ar", "lock", colGet(#side, "ar", "lock", r), dut->P##arlock);             \
            cmp(#side ".ar", "cache", colGet(#side, "ar", "cache", r), dut->P##arcache);          \
            cmp(#side ".ar", "prot", colGet(#side, "ar", "prot", r), dut->P##arprot);             \
            cmp(#side ".ar", "qos", colGet(#side, "ar", "qos", r), dut->P##arqos);                \
        }                                                                                         \
        cmp(#side ".b", "ready", colGet(#side, "b", "ready", r), dut->P##bready);                 \
        cmp(#side ".r", "ready", colGet(#side, "r", "ready", r), dut->P##rready);                 \
    } while (0)

        CMP_AXI(mem, m_axi_mem_0_);
        CMP_AXI(cfg, m_axi_main_);
#undef CMP_AXI

        // ---- clk=1 eval 提交 ----
        auto c2 = std::chrono::steady_clock::now();
        eval1();
        tEval1 += std::chrono::duration<double>(std::chrono::steady_clock::now() - c2).count();

        if (g_mismatch > 100) break;
    }
    auto t1 = std::chrono::steady_clock::now();

    double wall = std::chrono::duration<double>(t1 - t0).count();
    std::printf("replay: rows=%zu checks=%lu mismatches=%lu\n", rows, (unsigned long)g_checks,
                (unsigned long)g_mismatch);
    std::printf("loop: %.3fs over %zu rows = %.1f ns/row\n", wall, rows, wall * 1e9 / rows);
    std::printf("  eval0=%.3fs (%.0f%%)  eval1=%.3fs (%.0f%%)  harness=%.3fs (%.0f%%)\n", tEval0,
                tEval0 / wall * 100, tEval1, tEval1 / wall * 100, wall - tEval0 - tEval1,
                (wall - tEval0 - tEval1) / wall * 100);
    delete dut;
    return g_mismatch ? 1 : 0;
}
