// coremark 前端 trace 重放到 CC 边界（P2 验收）：
//   DUT = XscChiAdapter + CcSocket（即 L2 CHI 缝 → 环注入点之间的完整 CC 边界路径，
//   由本文件 CcBoundary 按 zj_l3.h:116-130 同款连线组合）。
//   驱动（trace 输入侧）：L2 侧六通道的 tx_* valid/bits + rx_* ready（io_decoupledCHI_*，
//     core_with_l2 实例口）；环侧 eject 四通道 valid/bits + inject 三通道 ready
//     （zhujiang_opt.ccn_0_0x8 io_dev_*）。
//   比对（trace 输出侧）：L2 侧 tx_*_ready + rx_* valid/bits；环侧 inject 三通道
//     valid/bits + eject 四通道 ready。
// 环（P1 已对拍）与 HomeShell/HnfStub（单测）不在本 DUT 内：环侧信号直接由 trace
// 驱动/比对，避免桩 HNF 与真 DongJiang 的响应时序差异传入——本测试验证的是真实
// coremark 流量下适配器+socket 的逐拍等价。
//
// trace 由 verify/trace/extract_cc_trace.py 从 emu FST 提取（make replay 一键生成）。
// 文件缺失时以 77 退出（ctest SKIP_RETURN_CODE）。路径：环境变量 CC_TRACE 优先，
// 缺省 trace/cc_front.txt（相对 ctest 工作目录 = build/）。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <doctest/doctest.h>

#include "wolvicmod/wolvicmod.h"

#include "model/cc/cc_socket.h"
#include "model/cc/xsc_chi_adapter.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"

using namespace zj;
using namespace zj::chi;
using wolvicmod::prefab::Dec;

namespace {

// ---------------- DUT：adapter + socket（连线同 ZjL3） ----------------
class CcBoundary : public wolvicmod::Module {
public:
    IN(bool, clk);

    // L2 侧（xscache flit）
    IN(Dec<xs::CHIREQ>, chi_tx_req);
    OUT(bool, chi_tx_req_rdy);
    IN(Dec<xs::CHIRSP>, chi_tx_rsp);
    OUT(bool, chi_tx_rsp_rdy);
    IN(Dec<xs::CHIDAT>, chi_tx_dat);
    OUT(bool, chi_tx_dat_rdy);
    OUT(Dec<xs::CHIRSP>, chi_rx_rsp);
    IN(bool, chi_rx_rsp_rdy);
    OUT(Dec<xs::CHIDAT>, chi_rx_dat);
    IN(bool, chi_rx_dat_rdy);
    OUT(Dec<xs::CHISNP>, chi_rx_snp);
    IN(bool, chi_rx_snp_rdy);

    // 环侧（zhujiang flit；rx=inject 输出、tx=eject 输入，对齐 SocketIcnSide io.dev）
    OUT(Dec<RReqFlit>, ring_rx_req);
    IN(bool, ring_rx_req_rdy);
    OUT(Dec<RespFlit>, ring_rx_resp);
    IN(bool, ring_rx_resp_rdy);
    OUT(Dec<DataFlit>, ring_rx_data);
    IN(bool, ring_rx_data_rdy);
    IN(Dec<RReqFlit>, ring_tx_req);
    OUT(bool, ring_tx_req_rdy);
    IN(Dec<RespFlit>, ring_tx_resp);
    OUT(bool, ring_tx_resp_rdy);
    IN(Dec<DataFlit>, ring_tx_data);
    OUT(bool, ring_tx_data_rdy);
    IN(Dec<SnoopFlit>, ring_tx_snoop);
    OUT(bool, ring_tx_snoop_rdy);

    MOD(xs::XscChiAdapter, adapter);
    MOD(sock::CcSocket, socket);

    CcBoundary() {
        socket.l2_rx_req = adapter.zj_rx_req;
        socket.l2_rx_resp = adapter.zj_rx_rsp;
        socket.l2_rx_data = adapter.zj_rx_dat;
        adapter.zj_rx_req_rdy = socket.l2_rx_req_rdy;
        adapter.zj_rx_rsp_rdy = socket.l2_rx_resp_rdy;
        adapter.zj_rx_dat_rdy = socket.l2_rx_data_rdy;
        adapter.zj_tx_rsp = socket.l2_tx_resp;
        adapter.zj_tx_dat = socket.l2_tx_data;
        adapter.zj_tx_snp = socket.l2_tx_snoop;
        socket.l2_tx_resp_rdy = adapter.zj_tx_rsp_rdy;
        socket.l2_tx_data_rdy = adapter.zj_tx_dat_rdy;
        socket.l2_tx_snoop_rdy = adapter.zj_tx_snp_rdy;
        // eject REQ 死端（ZhuJiangBridge tx.req.ready := false.B）
        socket.l2_tx_req_rdy = false;

        adapter.chi_tx_req = chi_tx_req;
        adapter.chi_tx_rsp = chi_tx_rsp;
        adapter.chi_tx_dat = chi_tx_dat;
        chi_tx_req_rdy = adapter.chi_tx_req_rdy;
        chi_tx_rsp_rdy = adapter.chi_tx_rsp_rdy;
        chi_tx_dat_rdy = adapter.chi_tx_dat_rdy;
        chi_rx_rsp = adapter.chi_rx_rsp;
        chi_rx_dat = adapter.chi_rx_dat;
        chi_rx_snp = adapter.chi_rx_snp;
        adapter.chi_rx_rsp_rdy = chi_rx_rsp_rdy;
        adapter.chi_rx_dat_rdy = chi_rx_dat_rdy;
        adapter.chi_rx_snp_rdy = chi_rx_snp_rdy;

        ring_rx_req = socket.ring_rx_req;
        ring_rx_resp = socket.ring_rx_resp;
        ring_rx_data = socket.ring_rx_data;
        socket.ring_rx_req_rdy = ring_rx_req_rdy;
        socket.ring_rx_resp_rdy = ring_rx_resp_rdy;
        socket.ring_rx_data_rdy = ring_rx_data_rdy;
        socket.ring_tx_req = ring_tx_req;
        socket.ring_tx_resp = ring_tx_resp;
        socket.ring_tx_data = ring_tx_data;
        socket.ring_tx_snoop = ring_tx_snoop;
        ring_tx_req_rdy = socket.ring_tx_req_rdy;
        ring_tx_resp_rdy = socket.ring_tx_resp_rdy;
        ring_tx_data_rdy = socket.ring_tx_data_rdy;
        ring_tx_snoop_rdy = socket.ring_tx_snoop_rdy;

        socket.clk = clk;
    }
};

// ---------------- trace 读取 ----------------
struct Trace {
    std::vector<std::string>                cols;
    std::unordered_map<std::string, size_t> idx;
    std::vector<std::vector<uint64_t>>      rows;
    std::vector<uint64_t>                   cycs;

    bool load(const char* path) {
        FILE* f = std::fopen(path, "r");
        if (!f) return false;
        std::string line;
        line.reserve(4096);
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

// ---------------- 驱动（trace → DUT 输入） ----------------
void drvChiReq(wolvicmod::In<Dec<xs::CHIREQ>>& p, size_t r) {
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
    p.set(Dec<xs::CHIREQ>{g_tr.get("l2.tx_req.valid", r) != 0, f});
}

void drvChiRsp(wolvicmod::In<Dec<xs::CHIRSP>>& p, size_t r) {
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
    p.set(Dec<xs::CHIRSP>{g_tr.get("l2.tx_rsp.valid", r) != 0, f});
}

void drvChiDat(wolvicmod::In<Dec<xs::CHIDAT>>& p, size_t r) {
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
    p.set(Dec<xs::CHIDAT>{g_tr.get("l2.tx_dat.valid", r) != 0, f});
}

void drvZjReq(wolvicmod::In<Dec<RReqFlit>>& p, size_t r) {
    RReqFlit f;
    f.qos          = g_tr.get("ring.rx_req.bits_QoS", r);
    f.tgt_id       = g_tr.get("ring.rx_req.bits_TgtID", r);
    f.src_id       = g_tr.get("ring.rx_req.bits_SrcID", r);
    f.txn_id       = g_tr.get("ring.rx_req.bits_TxnID", r);
    f.opcode       = g_tr.get("ring.rx_req.bits_Opcode", r);
    f.size         = g_tr.get("ring.rx_req.bits_Size", r);
    f.addr         = g_tr.get("ring.rx_req.bits_Addr", r);
    f.order        = g_tr.get("ring.rx_req.bits_Order", r);
    f.mem_attr     = g_tr.get("ring.rx_req.bits_MemAttr", r);
    f.snp_attr     = g_tr.get("ring.rx_req.bits_SnpAttr", r);
    f.excl         = g_tr.get("ring.rx_req.bits_Excl", r);
    f.exp_comp_ack = g_tr.get("ring.rx_req.bits_ExpCompAck", r);
    p.set(Dec<RReqFlit>{g_tr.get("ring.rx_req.valid", r) != 0, f});
}

void drvZjResp(wolvicmod::In<Dec<RespFlit>>& p, size_t r) {
    RespFlit f;
    f.qos       = g_tr.get("ring.rx_resp.bits_QoS", r);
    f.tgt_id    = g_tr.get("ring.rx_resp.bits_TgtID", r);
    f.src_id    = g_tr.get("ring.rx_resp.bits_SrcID", r);
    f.txn_id    = g_tr.get("ring.rx_resp.bits_TxnID", r);
    f.opcode    = g_tr.get("ring.rx_resp.bits_Opcode", r);
    f.resp_err  = g_tr.get("ring.rx_resp.bits_RespErr", r);
    f.resp      = g_tr.get("ring.rx_resp.bits_Resp", r);
    f.fwd_state = g_tr.get("ring.rx_resp.bits_FwdState", r);
    f.c_busy    = g_tr.get("ring.rx_resp.bits_CBusy", r);
    f.dbid      = g_tr.get("ring.rx_resp.bits_DBID", r);
    p.set(Dec<RespFlit>{g_tr.get("ring.rx_resp.valid", r) != 0, f});
}

void drvZjData(wolvicmod::In<Dec<DataFlit>>& p, size_t r) {
    DataFlit f;
    f.qos         = g_tr.get("ring.rx_data.bits_QoS", r);
    f.tgt_id      = g_tr.get("ring.rx_data.bits_TgtID", r);
    f.src_id      = g_tr.get("ring.rx_data.bits_SrcID", r);
    f.txn_id      = g_tr.get("ring.rx_data.bits_TxnID", r);
    f.home_nid    = g_tr.get("ring.rx_data.bits_HomeNID", r);
    f.opcode      = g_tr.get("ring.rx_data.bits_Opcode", r);
    f.resp_err    = g_tr.get("ring.rx_data.bits_RespErr", r);
    f.resp        = g_tr.get("ring.rx_data.bits_Resp", r);
    f.data_source = g_tr.get("ring.rx_data.bits_DataSource", r);
    f.c_busy      = g_tr.get("ring.rx_data.bits_CBusy", r);
    f.dbid        = g_tr.get("ring.rx_data.bits_DBID", r);
    f.data_id     = g_tr.get("ring.rx_data.bits_DataID", r);
    f.be          = g_tr.get("ring.rx_data.bits_BE", r);
    uint64_t d[4];
    g_tr.getWide("ring.rx_data.bits_Data", r, d);
    for (int i = 0; i < 4; ++i) f.data[i] = d[i];
    p.set(Dec<DataFlit>{g_tr.get("ring.rx_data.valid", r) != 0, f});
}

void drvZjSnp(wolvicmod::In<Dec<SnoopFlit>>& p, size_t r) {
    SnoopFlit f;
    f.qos             = g_tr.get("ring.rx_snoop.bits_QoS", r);
    f.tgt_id          = g_tr.get("ring.rx_snoop.bits_TgtID", r);
    f.src_id          = g_tr.get("ring.rx_snoop.bits_SrcID", r);
    f.txn_id          = g_tr.get("ring.rx_snoop.bits_TxnID", r);
    f.fwd_nid         = g_tr.get("ring.rx_snoop.bits_FwdNID", r);
    f.fwd_txn_id      = g_tr.get("ring.rx_snoop.bits_FwdTxnID", r);
    f.opcode          = g_tr.get("ring.rx_snoop.bits_Opcode", r);
    f.addr            = g_tr.get("ring.rx_snoop.bits_Addr", r);
    f.do_not_go_to_sd = g_tr.get("ring.rx_snoop.bits_DoNotGoToSD", r);
    f.ret_to_src      = g_tr.get("ring.rx_snoop.bits_RetToSrc", r);
    p.set(Dec<SnoopFlit>{g_tr.get("ring.rx_snoop.valid", r) != 0, f});
}

// ---------------- 比对（DUT 输出 vs trace） ----------------
void cmpRdy(const char* port, bool dut, const char* col, size_t r) {
    cmp(port, "ready", g_tr.get(col, r), dut ? 1 : 0);
}

void cmpChiRsp(const char* port, const Dec<xs::CHIRSP>& p, size_t r) {
    cmp(port, "valid", g_tr.get("l2.rx_rsp.valid", r), p.valid ? 1 : 0);
    if (!g_tr.get("l2.rx_rsp.valid", r)) return;  // valid=0 时 bits 为 don't-care（RTL 寄存器随机初始化）
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


void cmpChiDat(const char* port, const Dec<xs::CHIDAT>& p, size_t r) {
    cmp(port, "valid", g_tr.get("l2.rx_dat.valid", r), p.valid ? 1 : 0);
    if (!g_tr.get("l2.rx_dat.valid", r)) return;  // valid=0 时 bits 为 don't-care（RTL 寄存器随机初始化）
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

void cmpChiSnp(const char* port, const Dec<xs::CHISNP>& p, size_t r) {
    cmp(port, "valid", g_tr.get("l2.rx_snp.valid", r), p.valid ? 1 : 0);
    if (!g_tr.get("l2.rx_snp.valid", r)) return;  // valid=0 时 bits 为 don't-care（RTL 寄存器随机初始化）
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

void cmpZjReq(const char* port, const Dec<RReqFlit>& p, size_t r) {
    cmp(port, "valid", g_tr.get("ring.tx_req.valid", r), p.valid ? 1 : 0);
    if (!g_tr.get("ring.tx_req.valid", r)) return;  // valid=0 时 bits 为 don't-care（RTL 寄存器随机初始化）
    cmp(port, "QoS", g_tr.get("ring.tx_req.bits_QoS", r), p.bits.qos);
    cmp(port, "TgtID", g_tr.get("ring.tx_req.bits_TgtID", r), p.bits.tgt_id);
    cmp(port, "SrcID", g_tr.get("ring.tx_req.bits_SrcID", r), p.bits.src_id);
    cmp(port, "TxnID", g_tr.get("ring.tx_req.bits_TxnID", r), p.bits.txn_id);
    cmp(port, "Opcode", g_tr.get("ring.tx_req.bits_Opcode", r), p.bits.opcode);
    cmp(port, "Size", g_tr.get("ring.tx_req.bits_Size", r), p.bits.size);
    cmp(port, "Addr", g_tr.get("ring.tx_req.bits_Addr", r), p.bits.addr);
    cmp(port, "Order", g_tr.get("ring.tx_req.bits_Order", r), p.bits.order);
    cmp(port, "MemAttr", g_tr.get("ring.tx_req.bits_MemAttr", r), p.bits.mem_attr);
    cmp(port, "SnpAttr", g_tr.get("ring.tx_req.bits_SnpAttr", r), p.bits.snp_attr ? 1 : 0);
    cmp(port, "Excl", g_tr.get("ring.tx_req.bits_Excl", r), p.bits.excl ? 1 : 0);
    cmp(port, "ExpCompAck", g_tr.get("ring.tx_req.bits_ExpCompAck", r),
        p.bits.exp_comp_ack ? 1 : 0);
}

void cmpZjResp(const char* port, const Dec<RespFlit>& p, size_t r) {
    cmp(port, "valid", g_tr.get("ring.tx_resp.valid", r), p.valid ? 1 : 0);
    if (!g_tr.get("ring.tx_resp.valid", r)) return;  // valid=0 时 bits 为 don't-care（RTL 寄存器随机初始化）
    cmp(port, "QoS", g_tr.get("ring.tx_resp.bits_QoS", r), p.bits.qos);
    cmp(port, "TgtID", g_tr.get("ring.tx_resp.bits_TgtID", r), p.bits.tgt_id);
    cmp(port, "SrcID", g_tr.get("ring.tx_resp.bits_SrcID", r), p.bits.src_id);
    cmp(port, "TxnID", g_tr.get("ring.tx_resp.bits_TxnID", r), p.bits.txn_id);
    cmp(port, "Opcode", g_tr.get("ring.tx_resp.bits_Opcode", r), p.bits.opcode);
    cmp(port, "RespErr", g_tr.get("ring.tx_resp.bits_RespErr", r), p.bits.resp_err);
    cmp(port, "Resp", g_tr.get("ring.tx_resp.bits_Resp", r), p.bits.resp);
    cmp(port, "FwdState", g_tr.get("ring.tx_resp.bits_FwdState", r), p.bits.fwd_state);
    cmp(port, "CBusy", g_tr.get("ring.tx_resp.bits_CBusy", r), p.bits.c_busy);
    cmp(port, "DBID", g_tr.get("ring.tx_resp.bits_DBID", r), p.bits.dbid);
}

void cmpZjData(const char* port, const Dec<DataFlit>& p, size_t r) {
    cmp(port, "valid", g_tr.get("ring.tx_data.valid", r), p.valid ? 1 : 0);
    if (!g_tr.get("ring.tx_data.valid", r)) return;  // valid=0 时 bits 为 don't-care（RTL 寄存器随机初始化）
    cmp(port, "QoS", g_tr.get("ring.tx_data.bits_QoS", r), p.bits.qos);
    cmp(port, "TgtID", g_tr.get("ring.tx_data.bits_TgtID", r), p.bits.tgt_id);
    cmp(port, "SrcID", g_tr.get("ring.tx_data.bits_SrcID", r), p.bits.src_id);
    cmp(port, "TxnID", g_tr.get("ring.tx_data.bits_TxnID", r), p.bits.txn_id);
    cmp(port, "HomeNID", g_tr.get("ring.tx_data.bits_HomeNID", r), p.bits.home_nid);
    cmp(port, "Opcode", g_tr.get("ring.tx_data.bits_Opcode", r), p.bits.opcode);
    cmp(port, "RespErr", g_tr.get("ring.tx_data.bits_RespErr", r), p.bits.resp_err);
    cmp(port, "Resp", g_tr.get("ring.tx_data.bits_Resp", r), p.bits.resp);
    cmp(port, "DataSource", g_tr.get("ring.tx_data.bits_DataSource", r), p.bits.data_source);
    cmp(port, "CBusy", g_tr.get("ring.tx_data.bits_CBusy", r), p.bits.c_busy);
    cmp(port, "DBID", g_tr.get("ring.tx_data.bits_DBID", r), p.bits.dbid);
    cmp(port, "DataID", g_tr.get("ring.tx_data.bits_DataID", r), p.bits.data_id);
    cmp(port, "BE", g_tr.get("ring.tx_data.bits_BE", r), p.bits.be);
    uint64_t d[4];
    g_tr.getWide("ring.tx_data.bits_Data", r, d);
    for (int i = 0; i < 4; ++i) cmp(port, "Data", d[i], p.bits.data[i]);
}

}  // namespace

TEST_CASE("coremark front trace replay to CC boundary") {
    const char* path = std::getenv("CC_TRACE");
    if (!path || !*path) path = "trace/cc_front.txt";
    if (!g_tr.load(path)) {
        std::fprintf(stderr, "SKIP: trace not found: %s（先 make replay 生成）\n", path);
        std::exit(77);
    }
    std::printf("trace: %s rows=%zu cols=%zu cyc[%lu..%lu]\n", path, g_tr.rows.size(),
                g_tr.cols.size(), (unsigned long)g_tr.cycs.front(),
                (unsigned long)g_tr.cycs.back());

    CcBoundary dut;
    dut.elaborate();

    for (size_t r = 0; r < g_tr.rows.size(); ++r) {
        g_cyc = g_tr.cycs[r];

        // ---- 驱动（trace 输入侧）----
        drvChiReq(dut.chi_tx_req, r);
        drvChiRsp(dut.chi_tx_rsp, r);
        drvChiDat(dut.chi_tx_dat, r);
        dut.chi_rx_rsp_rdy.set(g_tr.get("l2.rx_rsp.ready", r) != 0);
        dut.chi_rx_dat_rdy.set(g_tr.get("l2.rx_dat.ready", r) != 0);
        dut.chi_rx_snp_rdy.set(g_tr.get("l2.rx_snp.ready", r) != 0);
        drvZjReq(dut.ring_tx_req, r);
        drvZjResp(dut.ring_tx_resp, r);
        drvZjData(dut.ring_tx_data, r);
        drvZjSnp(dut.ring_tx_snoop, r);
        dut.ring_rx_req_rdy.set(g_tr.get("ring.tx_req.ready", r) != 0);
        dut.ring_rx_resp_rdy.set(g_tr.get("ring.tx_resp.ready", r) != 0);
        dut.ring_rx_data_rdy.set(g_tr.get("ring.tx_data.ready", r) != 0);

        // ---- clk=0 eval 采样比对 ----
        dut.clk.set(0);
        dut.eval();

        cmpRdy("chi_tx_req", dut.chi_tx_req_rdy.get(), "l2.tx_req.ready", r);
        cmpRdy("chi_tx_rsp", dut.chi_tx_rsp_rdy.get(), "l2.tx_rsp.ready", r);
        cmpRdy("chi_tx_dat", dut.chi_tx_dat_rdy.get(), "l2.tx_dat.ready", r);
        cmpChiRsp("chi_rx_rsp", dut.chi_rx_rsp.get(), r);
        cmpChiDat("chi_rx_dat", dut.chi_rx_dat.get(), r);
        cmpChiSnp("chi_rx_snp", dut.chi_rx_snp.get(), r);
        cmpZjReq("ring_rx_req", dut.ring_rx_req.get(), r);
        cmpZjResp("ring_rx_resp", dut.ring_rx_resp.get(), r);
        cmpZjData("ring_rx_data", dut.ring_rx_data.get(), r);
        cmpRdy("ring_tx_req", dut.ring_tx_req_rdy.get(), "ring.rx_req.ready", r);
        cmpRdy("ring_tx_resp", dut.ring_tx_resp_rdy.get(), "ring.rx_resp.ready", r);
        cmpRdy("ring_tx_data", dut.ring_tx_data_rdy.get(), "ring.rx_data.ready", r);
        cmpRdy("ring_tx_snoop", dut.ring_tx_snoop_rdy.get(), "ring.rx_snoop.ready", r);

        // ---- clk=1 eval 提交 ----
        dut.clk.set(1);
        dut.eval();

        if (g_mismatch > 100) break;
    }

    std::printf("replay: rows=%zu checks=%lu mismatches=%lu\n", g_tr.rows.size(),
                (unsigned long)g_checks, (unsigned long)g_mismatch);
    CHECK(g_mismatch == 0);
}
