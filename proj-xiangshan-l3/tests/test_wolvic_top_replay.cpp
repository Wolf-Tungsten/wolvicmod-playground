// coremark 全程 trace 重放到 WolvicZjTop 三边界（P4b 验收）：
//   DUT = WolvicZjTop 全模型（L2 CHI 缝 → adapter/socket/环/HomeShell/DongJiang →
//     S/HI 桥 → memAXI/cfgAXI）。
//   驱动（trace 输入侧）：L2 六通道的 tx_* valid/bits + rx_* ready；
//     memAXI/cfgAXI 的 aw/w/ar ready + b/r valid/bits。
//   比对（trace 输出侧）：L2 侧 tx_*_ready + rx_* valid/bits；
//     memAXI/cfgAXI 的 aw/w/ar valid/bits + b/r ready；cc_tx_req 断言无流量。
// 模型周期精确时全程零失配：trace 中 mem/cfg 的 b/r 返回就是 RTL 仿真内存/外设
// 的真实应答，驱动给模型后 DongJiang/桥内部状态轨迹随之与 RTL 一致。
//
// trace 由 verify/trace/extract_top_trace.cpp（FST 直读）从 emu 全程 dump 提取
// （make replay-top 一键生成）。文件缺失时以 77 退出（ctest SKIP_RETURN_CODE）。
// 路径：环境变量 TOP_TRACE 优先，缺省 trace/cm_full.txt（相对 ctest 工作目录）。
// bits 按 valid 门控比对——difftest RANDOMIZE_REG_INIT 下 valid=0 的 bits 是随机垃圾。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <doctest/doctest.h>

#include "wolvicmod/wolvicmod.h"

#include "model/wolvic_zj_top.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"

using namespace zj;
using namespace zj::chi;
using wolvicmod::prefab::Valid;

namespace {

// ---------------- trace 读取（与 test_trace_replay.cpp 同格式） ----------------
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
                    if (vals.empty()) {  // cyc 列：十进制
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

// side.chan.field 列取值（side = l2|mem|cfg）
uint64_t colGet(const char* side, const char* chan, const char* field, size_t r) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s.%s.%s", side, chan, field);
    return g_tr.get(buf, r);
}

// ---------------- L2 CHI 侧驱动（列名同 test_trace_replay.cpp） ----------------
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
    uint64_t d[4];
    g_tr.getWide("l2.tx_dat.bits_data", r, d);
    for (int i = 0; i < 4; ++i) f.data[i] = d[i];
    p.set(Valid<xs::CHIDAT>{g_tr.get("l2.tx_dat.valid", r) != 0, f});
}

// ---------------- L2 CHI 侧比对（valid=0 时 bits don't-care） ----------------
void cmpChiRsp(const char* port, const Valid<xs::CHIRSP>& p, size_t r) {
    cmp(port, "valid", g_tr.get("l2.rx_rsp.valid", r), p.valid ? 1 : 0);
    if (!g_tr.get("l2.rx_rsp.valid", r)) return;
    cmp(port, "qos", g_tr.get("l2.rx_rsp.bits_qos", r), p.bits.qos);
    cmp(port, "srcID", g_tr.get("l2.rx_rsp.bits_srcID", r), p.bits.src_id);
    cmp(port, "txnID", g_tr.get("l2.rx_rsp.bits_txnID", r), p.bits.txn_id);
    cmp(port, "opcode", g_tr.get("l2.rx_rsp.bits_opcode", r), p.bits.opcode);
    cmp(port, "respErr", g_tr.get("l2.rx_rsp.bits_respErr", r), p.bits.resp_err);
    cmp(port, "resp", g_tr.get("l2.rx_rsp.bits_resp", r), p.bits.resp);
    cmp(port, "fwdState", g_tr.get("l2.rx_rsp.bits_fwdState", r), p.bits.fwd_state);
    cmp(port, "cBusy", g_tr.get("l2.rx_rsp.bits_cBusy", r), p.bits.c_busy);
    cmp(port, "dbID", g_tr.get("l2.rx_rsp.bits_dbID", r), p.bits.dbid);
    // rx_rsp 无 tgtID 列（RTL 被 firtool 裁剪，mapRspXs 亦不回填），不比
}

void cmpChiDat(const char* port, const Valid<xs::CHIDAT>& p, size_t r) {
    cmp(port, "valid", g_tr.get("l2.rx_dat.valid", r), p.valid ? 1 : 0);
    if (!g_tr.get("l2.rx_dat.valid", r)) return;
    cmp(port, "qos", g_tr.get("l2.rx_dat.bits_qos", r), p.bits.qos);
    cmp(port, "tgtID", g_tr.get("l2.rx_dat.bits_tgtID", r), p.bits.tgt_id);
    cmp(port, "srcID", g_tr.get("l2.rx_dat.bits_srcID", r), p.bits.src_id);
    cmp(port, "txnID", g_tr.get("l2.rx_dat.bits_txnID", r), p.bits.txn_id);
    cmp(port, "homeNID", g_tr.get("l2.rx_dat.bits_homeNID", r), p.bits.home_nid);
    cmp(port, "opcode", g_tr.get("l2.rx_dat.bits_opcode", r), p.bits.opcode);
    cmp(port, "respErr", g_tr.get("l2.rx_dat.bits_respErr", r), p.bits.resp_err);
    cmp(port, "resp", g_tr.get("l2.rx_dat.bits_resp", r), p.bits.resp);
    cmp(port, "dataSource", g_tr.get("l2.rx_dat.bits_dataSource", r), p.bits.data_source);
    cmp(port, "cBusy", g_tr.get("l2.rx_dat.bits_cBusy", r), p.bits.c_busy);
    cmp(port, "dbID", g_tr.get("l2.rx_dat.bits_dbID", r), p.bits.dbid);
    cmp(port, "dataID", g_tr.get("l2.rx_dat.bits_dataID", r), p.bits.data_id);
    cmp(port, "be", g_tr.get("l2.rx_dat.bits_be", r), p.bits.be);
    uint64_t d[4];
    g_tr.getWide("l2.rx_dat.bits_data", r, d);
    for (int i = 0; i < 4; ++i) cmp(port, "data", d[i], p.bits.data[i]);
}

void cmpChiSnp(const char* port, const Valid<xs::CHISNP>& p, size_t r) {
    cmp(port, "valid", g_tr.get("l2.rx_snp.valid", r), p.valid ? 1 : 0);
    if (!g_tr.get("l2.rx_snp.valid", r)) return;
    cmp(port, "qos", g_tr.get("l2.rx_snp.bits_qos", r), p.bits.qos);
    cmp(port, "srcID", g_tr.get("l2.rx_snp.bits_srcID", r), p.bits.src_id);
    cmp(port, "txnID", g_tr.get("l2.rx_snp.bits_txnID", r), p.bits.txn_id);
    cmp(port, "fwdNID", g_tr.get("l2.rx_snp.bits_fwdNID", r), p.bits.fwd_nid);
    cmp(port, "fwdTxnID", g_tr.get("l2.rx_snp.bits_fwdTxnID", r), p.bits.fwd_txn_id);
    cmp(port, "opcode", g_tr.get("l2.rx_snp.bits_opcode", r), p.bits.opcode);
    cmp(port, "addr", g_tr.get("l2.rx_snp.bits_addr", r), p.bits.addr);
    cmp(port, "doNotGoToSD", g_tr.get("l2.rx_snp.bits_doNotGoToSD", r),
        p.bits.do_not_go_to_sd ? 1 : 0);
    cmp(port, "retToSrc", g_tr.get("l2.rx_snp.bits_retToSrc", r), p.bits.ret_to_src ? 1 : 0);
}

// ---------------- AXI 侧驱动/比对（side = "mem"|"cfg"） ----------------
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
    uint64_t d[4];
    g_tr.getWide(base, r, d);
    for (int i = 0; i < 4; ++i) f.data[i] = d[i];
    p.set(Valid<axi::RFlit>{colGet(side, "r", "valid", r) != 0, f});
}

void cmpAxiAx(const char* port, const Valid<axi::AxFlit>& p, const char* side, const char* chan,
              size_t r) {
    cmp(port, "valid", colGet(side, chan, "valid", r), p.valid ? 1 : 0);
    if (!colGet(side, chan, "valid", r)) return;
    cmp(port, "id", colGet(side, chan, "id", r), p.bits.id);
    cmp(port, "addr", colGet(side, chan, "addr", r), p.bits.addr);
    cmp(port, "len", colGet(side, chan, "len", r), p.bits.len);
    cmp(port, "size", colGet(side, chan, "size", r), p.bits.size);
    cmp(port, "burst", colGet(side, chan, "burst", r), p.bits.burst);
    cmp(port, "lock", colGet(side, chan, "lock", r), p.bits.lock ? 1 : 0);
    cmp(port, "cache", colGet(side, chan, "cache", r), p.bits.cache);
    cmp(port, "prot", colGet(side, chan, "prot", r), p.bits.prot);
    cmp(port, "qos", colGet(side, chan, "qos", r), p.bits.qos);
}

void cmpAxiW(const char* port, const Valid<axi::WFlit>& p, const char* side, size_t r) {
    cmp(port, "valid", colGet(side, "w", "valid", r), p.valid ? 1 : 0);
    if (!colGet(side, "w", "valid", r)) return;
    char base[64];
    std::snprintf(base, sizeof base, "%s.w.data", side);
    uint64_t d[4];
    g_tr.getWide(base, r, d);
    for (int i = 0; i < 4; ++i) cmp(port, "data", d[i], p.bits.data[i]);
    cmp(port, "strb", colGet(side, "w", "strb", r), p.bits.strb);
    cmp(port, "last", colGet(side, "w", "last", r), p.bits.last ? 1 : 0);
}

}  // namespace

TEST_CASE("coremark full trace replay to WolvicZjTop") {
    const char* path = std::getenv("TOP_TRACE");
    if (!path || !*path) path = "trace/cm_full.txt";
    if (!g_tr.load(path)) {
        std::fprintf(stderr, "SKIP: trace not found: %s（先 make replay-top 生成）\n", path);
        std::exit(77);
    }
    std::printf("trace: %s rows=%zu cols=%zu cyc[%lu..%lu]\n", path, g_tr.rows.size(),
                g_tr.cols.size(), (unsigned long)g_tr.cycs.front(),
                (unsigned long)g_tr.cycs.back());

    WolvicZjTop dut;
    dut.elaborate();
    dut.ci.set(0);

    for (size_t r = 0; r < g_tr.rows.size(); ++r) {
        g_cyc = g_tr.cycs[r];

        // ---- 驱动（trace 输入侧）----
        drvChiReq(dut.chi_tx_req, r);
        drvChiRsp(dut.chi_tx_rsp, r);
        drvChiDat(dut.chi_tx_dat, r);
        dut.chi_rx_rsp_rdy.set(g_tr.get("l2.rx_rsp.ready", r) != 0);
        dut.chi_rx_dat_rdy.set(g_tr.get("l2.rx_dat.ready", r) != 0);
        dut.chi_rx_snp_rdy.set(g_tr.get("l2.rx_snp.ready", r) != 0);
        dut.mem_aw_rdy.set(colGet("mem", "aw", "ready", r) != 0);
        dut.mem_w_rdy.set(colGet("mem", "w", "ready", r) != 0);
        dut.mem_ar_rdy.set(colGet("mem", "ar", "ready", r) != 0);
        drvAxiB(dut.mem_b, "mem", r);
        drvAxiR(dut.mem_r, "mem", r);
        dut.cfg_aw_rdy.set(colGet("cfg", "aw", "ready", r) != 0);
        dut.cfg_w_rdy.set(colGet("cfg", "w", "ready", r) != 0);
        dut.cfg_ar_rdy.set(colGet("cfg", "ar", "ready", r) != 0);
        drvAxiB(dut.cfg_b, "cfg", r);
        drvAxiR(dut.cfg_r, "cfg", r);

        // ---- clk=0 eval 采样比对 ----
        dut.clk.set(0);
        dut.eval();

        cmp("chi_tx_req", "ready", g_tr.get("l2.tx_req.ready", r),
            dut.chi_tx_req_rdy.get() ? 1 : 0);
        cmp("chi_tx_rsp", "ready", g_tr.get("l2.tx_rsp.ready", r),
            dut.chi_tx_rsp_rdy.get() ? 1 : 0);
        cmp("chi_tx_dat", "ready", g_tr.get("l2.tx_dat.ready", r),
            dut.chi_tx_dat_rdy.get() ? 1 : 0);
        cmpChiRsp("chi_rx_rsp", dut.chi_rx_rsp.get(), r);
        cmpChiDat("chi_rx_dat", dut.chi_rx_dat.get(), r);
        cmpChiSnp("chi_rx_snp", dut.chi_rx_snp.get(), r);
        cmpAxiAx("mem.aw", dut.mem_aw.get(), "mem", "aw", r);
        cmpAxiW("mem.w", dut.mem_w.get(), "mem", r);
        cmpAxiAx("mem.ar", dut.mem_ar.get(), "mem", "ar", r);
        cmp("mem.b", "ready", colGet("mem", "b", "ready", r), dut.mem_b_rdy.get() ? 1 : 0);
        cmp("mem.r", "ready", colGet("mem", "r", "ready", r), dut.mem_r_rdy.get() ? 1 : 0);
        cmpAxiAx("cfg.aw", dut.cfg_aw.get(), "cfg", "aw", r);
        cmpAxiW("cfg.w", dut.cfg_w.get(), "cfg", r);
        cmpAxiAx("cfg.ar", dut.cfg_ar.get(), "cfg", "ar", r);
        cmp("cfg.b", "ready", colGet("cfg", "b", "ready", r), dut.cfg_b_rdy.get() ? 1 : 0);
        cmp("cfg.r", "ready", colGet("cfg", "r", "ready", r), dut.cfg_r_rdy.get() ? 1 : 0);
        cmp("cc_tx_req", "valid", 0, dut.cc_tx_req.get().valid ? 1 : 0);

        // ---- clk=1 eval 提交 ----
        dut.clk.set(1);
        dut.eval();

        if (g_mismatch > 100) break;
    }

    std::printf("replay: rows=%zu checks=%lu mismatches=%lu\n", g_tr.rows.size(),
                (unsigned long)g_checks, (unsigned long)g_mismatch);
    CHECK(g_mismatch == 0);
}
