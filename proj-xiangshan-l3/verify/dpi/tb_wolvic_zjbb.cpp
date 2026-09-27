// tb_wolvic_zjbb.cpp：WolvicZjBB SV 壳 + DPI glue 的独立冒烟测试台（不经 emu）。
//
// 闭环两条链路：
//   in 侧：tb 驱动 leaf 端口 pattern → SV concat → in_pack（DPI）→ glue
//     unpackInputs。tb 读 glue trace 的 in hex，解回 InPack 后用共享的
//     wzj::unpackInputs 装进独立容器模型，逐字段断言 == 驱动 pattern（两拍，
//     第二拍反相防 sticky）。
//   out 侧：同进程模型单例（wzj::model()）经若干 step 后，tb 以
//     wzj::packOutputs 为参照，抽查 leaf 输出字段（rx_rsp / mem_aw / cfg_ar）。
//
// 构建运行：proj Makefile `make smoke-dpi`（verify/dpi/run.sh）。

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "VWolvicZjBB.h"
#include "verilated.h"

#include "dpi/csrc/wolvic_zj_pack.h"

using namespace zj;
using wolvicmod::prefab::Valid;

namespace {

int g_fail = 0;
#define CHECK_EQ(what, got, exp)                                                     \
    do {                                                                             \
        const uint64_t _g = (got), _e = (exp);                                       \
        if (_g != _e) {                                                              \
            std::fprintf(stderr, "[FAIL] %s: got 0x%llx exp 0x%llx\n", what,         \
                         (unsigned long long)_g, (unsigned long long)_e);            \
            ++g_fail;                                                                \
        }                                                                            \
    } while (0)

const char* kTracePath = "build/dpi-smoke/glue_trace.txt";

void tick(VWolvicZjBB* dut) {
    dut->clock = 0;
    dut->eval();
    dut->clock = 1;
    dut->eval();  // posedge：壳调 wolvic_zj_step
}

// 读 trace 最后一行的 in hex，解成 InPack
wzj::InPack readTraceIn() {
    FILE* f = std::fopen(kTracePath, "r");
    if (!f) {
        std::fprintf(stderr, "[FAIL] cannot open %s\n", kTracePath);
        ++g_fail;
        return {};
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    long start = sz - 2;
    bool foundNl = false;
    while (start > 0) {
        std::fseek(f, start, SEEK_SET);
        if (std::fgetc(f) == '\n') { foundNl = true; break; }
        --start;
    }
    std::fseek(f, foundNl ? start + 1 : 0, SEEK_SET);
    char line[2048];
    std::string last;
    while (std::fgets(line, sizeof line, f)) last = line;
    std::fclose(f);
    // 格式：<cyc> in <hex> out <hex>
    const size_t inPos = last.find(" in ");
    const size_t outPos = last.find(" out ");
    std::string hex = last.substr(inPos + 4, outPos - inPos - 4);
    wzj::InPack w{};
    int bit = 0;
    for (auto it = hex.rbegin(); it != hex.rend() && bit < wzj::kInW; ++it, bit += 4) {
        const char c = *it;
        const uint64_t v = (c >= '0' && c <= '9')   ? uint64_t(c - '0')
                           : (c >= 'a' && c <= 'f') ? uint64_t(c - 'a' + 10)
                                                    : uint64_t(c - 'A' + 10);
        w[bit >> 6] |= v << (bit & 63);
    }
    return w;
}

// 驱动一拍输入 pattern 并校验 glue 侧解包结果
void driveAndCheck(VWolvicZjBB* dut, WolvicZjTop& container, bool inv) {
    const auto b1 = [&](uint64_t v) { return inv ? !v : !!v; };

    // L2 CHI tx
    dut->rn_0_tx_req_valid = b1(1);
    dut->rn_0_tx_req_bits_qos = 0xA & 0xF;
    dut->rn_0_tx_req_bits_tgtID = 0x123 & 0x7FF;
    dut->rn_0_tx_req_bits_srcID = 0x456 & 0x7FF;
    dut->rn_0_tx_req_bits_txnID = 0x789 & 0xFFF;
    dut->rn_0_tx_req_bits_opcode = 0x55 & 0x7F;
    dut->rn_0_tx_req_bits_size = 0x6;
    dut->rn_0_tx_req_bits_addr = inv ? 0xEDBCA9876543ULL : 0x123456789ABCULL;
    dut->rn_0_tx_req_bits_order = 0x2;
    dut->rn_0_tx_req_bits_memAttr_allocate = b1(1);
    dut->rn_0_tx_req_bits_memAttr_cacheable = b1(0);
    dut->rn_0_tx_req_bits_memAttr_device = b1(1);
    dut->rn_0_tx_req_bits_memAttr_ewa = b1(0);
    dut->rn_0_tx_req_bits_snpAttr = b1(1);
    dut->rn_0_tx_req_bits_snoopMe = b1(0);
    dut->rn_0_tx_req_bits_expCompAck = b1(1);
    dut->rn_0_tx_req_bits_mpam_partID = 0x155 & 0x1FF;
    dut->rn_0_tx_req_bits_rsvdc = 0x9;
    // 被裁剪字段：驱动反相垃圾，不得漏进模型
    dut->rn_0_tx_req_bits_returnNID = inv ? 0 : 0x7FF;
    dut->rn_0_tx_req_bits_stashNIDValid = b1(0);
    dut->rn_0_tx_req_bits_returnTxnID = inv ? 0 : 0xFFF;
    dut->rn_0_tx_req_bits_ns = b1(0);
    dut->rn_0_tx_req_bits_likelyshared = b1(0);
    dut->rn_0_tx_req_bits_allowRetry = b1(0);
    dut->rn_0_tx_req_bits_pCrdType = inv ? 0 : 0xF;
    dut->rn_0_tx_req_bits_lpIDWithPadding = inv ? 0 : 0xFF;
    dut->rn_0_tx_req_bits_tagOp = 0x3;
    dut->rn_0_tx_req_bits_traceTag = b1(0);
    dut->rn_0_tx_req_bits_mpam_perfMonGroup = b1(0);
    dut->rn_0_tx_req_bits_mpam_mpamNS = b1(0);

    dut->rn_0_tx_rsp_valid = b1(1);
    dut->rn_0_tx_rsp_bits_qos = 0x5;
    dut->rn_0_tx_rsp_bits_tgtID = 0x234 & 0x7FF;
    dut->rn_0_tx_rsp_bits_srcID = 0x567 & 0x7FF;
    dut->rn_0_tx_rsp_bits_txnID = 0x89A & 0xFFF;
    dut->rn_0_tx_rsp_bits_opcode = 0x1A & 0x1F;
    dut->rn_0_tx_rsp_bits_respErr = 0x2;
    dut->rn_0_tx_rsp_bits_resp = 0x5;
    dut->rn_0_tx_rsp_bits_fwdState = 0x3;
    dut->rn_0_tx_rsp_bits_cBusy = 0x6;
    dut->rn_0_tx_rsp_bits_dbID = 0xABC & 0xFFF;
    dut->rn_0_tx_rsp_bits_pCrdType = inv ? 0 : 0xF;
    dut->rn_0_tx_rsp_bits_tagOp = 0x3;
    dut->rn_0_tx_rsp_bits_traceTag = b1(0);

    dut->rn_0_tx_dat_valid = b1(1);
    dut->rn_0_tx_dat_bits_qos = 0x3;
    dut->rn_0_tx_dat_bits_tgtID = 0x345 & 0x7FF;
    dut->rn_0_tx_dat_bits_srcID = 0x678 & 0x7FF;
    dut->rn_0_tx_dat_bits_txnID = 0x9AB & 0xFFF;
    dut->rn_0_tx_dat_bits_homeNID = 0x111 & 0x7FF;
    dut->rn_0_tx_dat_bits_opcode = 0xB;
    dut->rn_0_tx_dat_bits_respErr = 0x1;
    dut->rn_0_tx_dat_bits_resp = 0x2;
    dut->rn_0_tx_dat_bits_dataSource = 0xD;
    dut->rn_0_tx_dat_bits_cBusy = 0x5;
    dut->rn_0_tx_dat_bits_dbID = 0xBCD & 0xFFF;
    dut->rn_0_tx_dat_bits_dataID = 0x2;
    dut->rn_0_tx_dat_bits_be = inv ? 0 : 0xDEADBEEFUL;
    for (int i = 0; i < 8; ++i)
        dut->rn_0_tx_dat_bits_data[i] = inv ? ~uint32_t(0x11111111 * (i + 1)) : uint32_t(0x11111111 * (i + 1));
    dut->rn_0_tx_dat_bits_ccID = 0x3;
    dut->rn_0_tx_dat_bits_tagOp = 0x3;
    dut->rn_0_tx_dat_bits_tag = 0xFF;
    dut->rn_0_tx_dat_bits_tu = 0x3;
    dut->rn_0_tx_dat_bits_traceTag = b1(0);
    dut->rn_0_tx_dat_bits_rsvdc = 0xF;

    // L2 CHI rx ready
    dut->rn_0_rx_rsp_ready = b1(1);
    dut->rn_0_rx_dat_ready = b1(0);
    dut->rn_0_rx_snp_ready = b1(1);

    // memAXI 返回
    dut->ddrc_awready = b1(1);
    dut->ddrc_wready = b1(0);
    dut->ddrc_bvalid = b1(1);
    dut->ddrc_bid = 0x2A & 0x3F;
    dut->ddrc_bresp = 0x1;
    dut->ddrc_arready = b1(1);
    dut->ddrc_rvalid = b1(1);
    dut->ddrc_rid = 0x15 & 0x3F;
    for (int i = 0; i < 8; ++i)
        dut->ddrc_rdata[i] = inv ? ~uint32_t(0x01010101 * (i + 1)) : uint32_t(0x01010101 * (i + 1));
    dut->ddrc_rresp = 0x2;
    dut->ddrc_rlast = b1(1);

    // cfgAXI 返回
    dut->peri_0_awready = b1(0);
    dut->peri_0_wready = b1(1);
    dut->peri_0_bvalid = b1(1);
    dut->peri_0_bid = 0x5;
    dut->peri_0_bresp = 0x2;
    dut->peri_0_arready = b1(0);
    dut->peri_0_rvalid = b1(1);
    dut->peri_0_rid = 0x6;
    for (int i = 0; i < 8; ++i)
        dut->peri_0_rdata[i] = inv ? ~uint32_t(0x02020202 * (i + 1)) : uint32_t(0x02020202 * (i + 1));
    dut->peri_0_rresp = 0x1;
    dut->peri_0_rlast = b1(0);

    tick(dut);

    // ---- 校验：glue trace 的 in pack 解回容器后逐字段相等 ----
    const wzj::InPack in = readTraceIn();
    wzj::unpackInputs(container, in);

    const auto req = container.chi_tx_req.get();
    CHECK_EQ("tx_req.valid", req.valid, b1(1));
    CHECK_EQ("tx_req.qos", req.bits.qos, 0xA);
    CHECK_EQ("tx_req.tgt_id", req.bits.tgt_id, 0x123);
    CHECK_EQ("tx_req.src_id", req.bits.src_id, 0x456);
    CHECK_EQ("tx_req.txn_id", req.bits.txn_id, 0x789);
    CHECK_EQ("tx_req.opcode", req.bits.opcode, 0x55);
    CHECK_EQ("tx_req.size", req.bits.size, 0x6);
    CHECK_EQ("tx_req.addr", req.bits.addr, inv ? 0xEDBCA9876543ULL : 0x123456789ABCULL);
    CHECK_EQ("tx_req.order", req.bits.order, 0x2);
    CHECK_EQ("tx_req.memAttr.allocate", req.bits.mem_attr_allocate, b1(1));
    CHECK_EQ("tx_req.memAttr.cacheable", req.bits.mem_attr_cacheable, b1(0));
    CHECK_EQ("tx_req.memAttr.device", req.bits.mem_attr_device, b1(1));
    CHECK_EQ("tx_req.memAttr.ewa", req.bits.mem_attr_ewa, b1(0));
    CHECK_EQ("tx_req.snpAttr", req.bits.snp_attr, b1(1));
    CHECK_EQ("tx_req.snoopMe", req.bits.snoop_me, b1(0));
    CHECK_EQ("tx_req.expCompAck", req.bits.exp_comp_ack, b1(1));
    CHECK_EQ("tx_req.mpam_partID", req.bits.mpam_part_id, 0x155);
    CHECK_EQ("tx_req.rsvdc", req.bits.rsvdc, 0x9);

    const auto rsp = container.chi_tx_rsp.get();
    CHECK_EQ("tx_rsp.valid", rsp.valid, b1(1));
    CHECK_EQ("tx_rsp.qos", rsp.bits.qos, 0x5);
    CHECK_EQ("tx_rsp.tgt_id", rsp.bits.tgt_id, 0x234);
    CHECK_EQ("tx_rsp.src_id", rsp.bits.src_id, 0x567);
    CHECK_EQ("tx_rsp.txn_id", rsp.bits.txn_id, 0x89A);
    CHECK_EQ("tx_rsp.opcode", rsp.bits.opcode, 0x1A);
    CHECK_EQ("tx_rsp.respErr", rsp.bits.resp_err, 0x2);
    CHECK_EQ("tx_rsp.resp", rsp.bits.resp, 0x5);
    CHECK_EQ("tx_rsp.fwdState", rsp.bits.fwd_state, 0x3);
    CHECK_EQ("tx_rsp.cBusy", rsp.bits.c_busy, 0x6);
    CHECK_EQ("tx_rsp.dbID", rsp.bits.dbid, 0xABC);

    const auto dat = container.chi_tx_dat.get();
    CHECK_EQ("tx_dat.valid", dat.valid, b1(1));
    CHECK_EQ("tx_dat.qos", dat.bits.qos, 0x3);
    CHECK_EQ("tx_dat.tgt_id", dat.bits.tgt_id, 0x345);
    CHECK_EQ("tx_dat.src_id", dat.bits.src_id, 0x678);
    CHECK_EQ("tx_dat.txn_id", dat.bits.txn_id, 0x9AB);
    CHECK_EQ("tx_dat.home_nid", dat.bits.home_nid, 0x111);
    CHECK_EQ("tx_dat.opcode", dat.bits.opcode, 0xB);
    CHECK_EQ("tx_dat.respErr", dat.bits.resp_err, 0x1);
    CHECK_EQ("tx_dat.resp", dat.bits.resp, 0x2);
    CHECK_EQ("tx_dat.dataSource", dat.bits.data_source, 0xD);
    CHECK_EQ("tx_dat.cBusy", dat.bits.c_busy, 0x5);
    CHECK_EQ("tx_dat.dbID", dat.bits.dbid, 0xBCD);
    CHECK_EQ("tx_dat.dataID", dat.bits.data_id, 0x2);
    CHECK_EQ("tx_dat.be", dat.bits.be, inv ? 0 : 0xDEADBEEFUL);
    for (int i = 0; i < 8; ++i) {
        const uint32_t exp32 = inv ? ~uint32_t(0x11111111 * (i + 1)) : uint32_t(0x11111111 * (i + 1));
        CHECK_EQ("tx_dat.data w32", uint32_t(dat.bits.data[i / 2] >> (32 * (i % 2))), exp32);
    }

    CHECK_EQ("rx_rsp_rdy", container.chi_rx_rsp_rdy.get(), b1(1));
    CHECK_EQ("rx_dat_rdy", container.chi_rx_dat_rdy.get(), b1(0));
    CHECK_EQ("rx_snp_rdy", container.chi_rx_snp_rdy.get(), b1(1));

    CHECK_EQ("mem_aw_rdy", container.mem_aw_rdy.get(), b1(1));
    CHECK_EQ("mem_w_rdy", container.mem_w_rdy.get(), b1(0));
    CHECK_EQ("mem_ar_rdy", container.mem_ar_rdy.get(), b1(1));
    const auto mb = container.mem_b.get();
    CHECK_EQ("mem_b.valid", mb.valid, b1(1));
    CHECK_EQ("mem_b.id", mb.bits.id, 0x2A);
    CHECK_EQ("mem_b.resp", mb.bits.resp, 0x1);
    const auto mr = container.mem_r.get();
    CHECK_EQ("mem_r.valid", mr.valid, b1(1));
    CHECK_EQ("mem_r.id", mr.bits.id, 0x15);
    CHECK_EQ("mem_r.resp", mr.bits.resp, 0x2);
    CHECK_EQ("mem_r.last", mr.bits.last, b1(1));
    for (int i = 0; i < 8; ++i) {
        const uint32_t exp32 = inv ? ~uint32_t(0x01010101 * (i + 1)) : uint32_t(0x01010101 * (i + 1));
        CHECK_EQ("mem_r.data w32", uint32_t(mr.bits.data[i / 2] >> (32 * (i % 2))), exp32);
    }

    CHECK_EQ("cfg_aw_rdy", container.cfg_aw_rdy.get(), b1(0));
    CHECK_EQ("cfg_w_rdy", container.cfg_w_rdy.get(), b1(1));
    CHECK_EQ("cfg_ar_rdy", container.cfg_ar_rdy.get(), b1(0));
    const auto cb = container.cfg_b.get();
    CHECK_EQ("cfg_b.valid", cb.valid, b1(1));
    CHECK_EQ("cfg_b.id", cb.bits.id, 0x5);
    CHECK_EQ("cfg_b.resp", cb.bits.resp, 0x2);
    const auto cr = container.cfg_r.get();
    CHECK_EQ("cfg_r.valid", cr.valid, b1(1));
    CHECK_EQ("cfg_r.id", cr.bits.id, 0x6);
    CHECK_EQ("cfg_r.resp", cr.bits.resp, 0x1);
    CHECK_EQ("cfg_r.last", cr.bits.last, b1(0));
    for (int i = 0; i < 8; ++i) {
        const uint32_t exp32 = inv ? ~uint32_t(0x02020202 * (i + 1)) : uint32_t(0x02020202 * (i + 1));
        CHECK_EQ("cfg_r.data w32", uint32_t(cr.bits.data[i / 2] >> (32 * (i % 2))), exp32);
    }
}

// out 侧抽查：leaf 读回 vs packOutputs 参照
void checkOutputs(VWolvicZjBB* dut) {
    const wzj::OutPack ref = wzj::packOutputs(wzj::model());
    // 布局（LSB 先行）：cfg(448) | mem(454) | snp_bits(102) snp_valid(1)
    //   dat_bits(367) dat_valid(1) | rsp_bits(66) rsp_valid(1) | 3 rdy
    constexpr int kMemOff = 448;
    constexpr int kRspBitsOff = 448 + 454 + 102 + 1 + 367 + 1;
    const auto rspBits = xs::CHIRSP::unpack(getWide<(xs::CHIRSP::kWidth + 63) / 64>(ref, kRspBitsOff));
    CHECK_EQ("o.rx_rsp.qos", dut->rn_0_rx_rsp_bits_qos, rspBits.qos);
    CHECK_EQ("o.rx_rsp.txnID", dut->rn_0_rx_rsp_bits_txnID, rspBits.txn_id);
    CHECK_EQ("o.rx_rsp.resp", dut->rn_0_rx_rsp_bits_resp, rspBits.resp);
    CHECK_EQ("o.rx_rsp.dbID", dut->rn_0_rx_rsp_bits_dbID, rspBits.dbid);
    CHECK_EQ("o.rx_rsp.valid", dut->rn_0_rx_rsp_valid, getBits(ref, kRspBitsOff + 66, kRspBitsOff + 66));
    // mem 组（LSB 先行，组内末尾 = aw 组高位）：awvalid@[453], awid@[452:447]（组内）
    CHECK_EQ("o.mem_awvalid", dut->ddrc_awvalid, getBits(ref, kMemOff + 453, kMemOff + 453));
    CHECK_EQ("o.mem_awid", dut->ddrc_awid, getBits(ref, kMemOff + 452, kMemOff + 447));
    CHECK_EQ("o.mem_awaddr", dut->ddrc_awaddr, getBits(ref, kMemOff + 446, kMemOff + 398));
    CHECK_EQ("o.mem_awlen", dut->ddrc_awlen, getBits(ref, kMemOff + 397, kMemOff + 390));
    // cfg 组（LSB 先行，448 位）：arvalid 位于 ar 组内偏移 1+4+3+4+1+2+3+8+49+3=78 之前：
    // rready(1) arqos(4) arprot(3) arcache(4) arlock(1) arburst(2) arsize(3)
    //   arlen(8) araddr(49) arid(3) arvalid(1) → arvalid@[78], arid@[77:75], araddr@[74:26]
    CHECK_EQ("o.cfg_arvalid", dut->peri_0_arvalid, getBits(ref, 78, 78));
    CHECK_EQ("o.cfg_arid", dut->peri_0_arid, getBits(ref, 77, 75));
    CHECK_EQ("o.cfg_araddr", dut->peri_0_araddr, getBits(ref, 74, 26));
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    setenv("WOLVIC_ZJ_TRACE", kTracePath, 1);

    auto* dut = new VWolvicZjBB;
    WolvicZjTop container;  // unpack 容器
    container.elaborate();  // set() 要求先 elaborate

    // 复位：reset 期间 step 不得被调用（模型保持构造态）
    dut->reset = 1;
    for (int i = 0; i < 5; ++i) tick(dut);
    dut->reset = 0;

    // out 侧初态（initial peek 路径）抽查
    checkOutputs(dut);

    driveAndCheck(dut, container, false);
    driveAndCheck(dut, container, true);

    // 跑过两拍后再抽查一次 out 侧（模型已推进）
    checkOutputs(dut);

    dut->final();
    delete dut;

    if (g_fail == 0) std::printf("[smoke-dpi] PASS\n");
    else std::printf("[smoke-dpi] FAIL (%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
