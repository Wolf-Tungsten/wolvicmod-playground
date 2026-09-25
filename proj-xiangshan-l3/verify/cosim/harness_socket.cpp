// CC socket（PDC）对拍：wolvicmod zj::sock::CcSocket vs RTL
// CcSocketRef（SocketDevSide+SocketIcnSide 背对背，XiangShan emu 构建产物
// build/rtl，kunminghu-v3 DefaultConfig + ZhuJiang，与目标配置同源同参）。
//
// 协议（同 harness_ring）：每拍 = 生成激励（同一 RNG 两侧共享）→ 驱动两侧 →
// clk=0 eval 采样比对 → clk=1 eval 提交。复位：ref.reset 拉高 16 拍撤除，
// 空跑 32 拍（dut 初始态即复位后态）。
// 边界：l2_* 六通道 + ring_* 七通道。eject REQ 在 RTL 集成中被桥恒置
// ready=false（io_icn_tx_req 口被 firtool 裁掉）：dut 侧 l2_tx_req_rdy 恒
// false，其死端反压效果经 ring_tx_req_ready 比对覆盖（该通道照常激励）。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "VCcSocket.h"
#include "verilated.h"
#include "wolvicmod/wolvicmod.h"
#include "model/cc/cc_socket.h"

using namespace zj;
using namespace zj::chi;
using namespace zj::sock;
using wolvicmod::prefab::Dec;

namespace {

std::mt19937_64 rng;

uint64_t randBits(int w) { return rng() & ((w >= 64) ? ~uint64_t{0} : ((uint64_t{1} << w) - 1)); }

// ---------------- 失配报告 ----------------
uint64_t g_checks = 0, g_mismatch = 0;
uint32_t g_seed   = 0;
uint64_t g_cyc    = 0;

void cmp(const char* port, const char* field, uint64_t refV, uint64_t dutV) {
    ++g_checks;
    if (refV == dutV) return;
    ++g_mismatch;
    if (g_mismatch > 20) return;
    std::printf("MISMATCH seed=%u cyc=%lu port=%s.%s ref=0x%llx dut=0x%llx\n", g_seed,
                (unsigned long)g_cyc, port, field, (unsigned long long)refV,
                (unsigned long long)dutV);
}

}  // namespace

// ---------------- 逐字段比对/驱动宏（pfx 必须是 token） ----------------
#define CMP_REQ(pfx, dv)                                                \
    do {                                                                \
        cmp(#pfx, "valid", ref.pfx##_valid, dv.valid);                  \
        cmp(#pfx, "QoS", ref.pfx##_bits_QoS, dv.bits.qos);              \
        cmp(#pfx, "TgtID", ref.pfx##_bits_TgtID, dv.bits.tgt_id);       \
        cmp(#pfx, "SrcID", ref.pfx##_bits_SrcID, dv.bits.src_id);       \
        cmp(#pfx, "TxnID", ref.pfx##_bits_TxnID, dv.bits.txn_id);       \
        cmp(#pfx, "Opcode", ref.pfx##_bits_Opcode, dv.bits.opcode);     \
        cmp(#pfx, "Size", ref.pfx##_bits_Size, dv.bits.size);           \
        cmp(#pfx, "Addr", ref.pfx##_bits_Addr, dv.bits.addr);           \
        cmp(#pfx, "Order", ref.pfx##_bits_Order, dv.bits.order);        \
        cmp(#pfx, "MemAttr", ref.pfx##_bits_MemAttr, dv.bits.mem_attr); \
        cmp(#pfx, "SnpAttr", ref.pfx##_bits_SnpAttr, dv.bits.snp_attr); \
        cmp(#pfx, "Excl", ref.pfx##_bits_Excl, dv.bits.excl);           \
        cmp(#pfx, "ExpCompAck", ref.pfx##_bits_ExpCompAck, dv.bits.exp_comp_ack); \
    } while (0)

#define CMP_RESP(pfx, dv)                                               \
    do {                                                                \
        cmp(#pfx, "valid", ref.pfx##_valid, dv.valid);                  \
        cmp(#pfx, "QoS", ref.pfx##_bits_QoS, dv.bits.qos);              \
        cmp(#pfx, "TgtID", ref.pfx##_bits_TgtID, dv.bits.tgt_id);       \
        cmp(#pfx, "SrcID", ref.pfx##_bits_SrcID, dv.bits.src_id);       \
        cmp(#pfx, "TxnID", ref.pfx##_bits_TxnID, dv.bits.txn_id);       \
        cmp(#pfx, "Opcode", ref.pfx##_bits_Opcode, dv.bits.opcode);     \
        cmp(#pfx, "RespErr", ref.pfx##_bits_RespErr, dv.bits.resp_err); \
        cmp(#pfx, "Resp", ref.pfx##_bits_Resp, dv.bits.resp);           \
        cmp(#pfx, "FwdState", ref.pfx##_bits_FwdState, dv.bits.fwd_state); \
        cmp(#pfx, "CBusy", ref.pfx##_bits_CBusy, dv.bits.c_busy);       \
        cmp(#pfx, "DBID", ref.pfx##_bits_DBID, dv.bits.dbid);           \
    } while (0)

// L2 侧 eject RSP：RTL 端口无 TgtID（桥不消费，被 firtool 裁剪）
#define CMP_RESP_L2(pfx, dv)                                            \
    do {                                                                \
        cmp(#pfx, "valid", ref.pfx##_valid, dv.valid);                  \
        cmp(#pfx, "QoS", ref.pfx##_bits_QoS, dv.bits.qos);              \
        cmp(#pfx, "SrcID", ref.pfx##_bits_SrcID, dv.bits.src_id);       \
        cmp(#pfx, "TxnID", ref.pfx##_bits_TxnID, dv.bits.txn_id);       \
        cmp(#pfx, "Opcode", ref.pfx##_bits_Opcode, dv.bits.opcode);     \
        cmp(#pfx, "RespErr", ref.pfx##_bits_RespErr, dv.bits.resp_err); \
        cmp(#pfx, "Resp", ref.pfx##_bits_Resp, dv.bits.resp);           \
        cmp(#pfx, "FwdState", ref.pfx##_bits_FwdState, dv.bits.fwd_state); \
        cmp(#pfx, "CBusy", ref.pfx##_bits_CBusy, dv.bits.c_busy);       \
        cmp(#pfx, "DBID", ref.pfx##_bits_DBID, dv.bits.dbid);           \
    } while (0)

#define CMP_DATA(pfx, dv)                                                   \
    do {                                                                    \
        cmp(#pfx, "valid", ref.pfx##_valid, dv.valid);                      \
        cmp(#pfx, "QoS", ref.pfx##_bits_QoS, dv.bits.qos);                  \
        cmp(#pfx, "TgtID", ref.pfx##_bits_TgtID, dv.bits.tgt_id);           \
        cmp(#pfx, "SrcID", ref.pfx##_bits_SrcID, dv.bits.src_id);           \
        cmp(#pfx, "TxnID", ref.pfx##_bits_TxnID, dv.bits.txn_id);           \
        cmp(#pfx, "HomeNID", ref.pfx##_bits_HomeNID, dv.bits.home_nid);     \
        cmp(#pfx, "Opcode", ref.pfx##_bits_Opcode, dv.bits.opcode);         \
        cmp(#pfx, "RespErr", ref.pfx##_bits_RespErr, dv.bits.resp_err);     \
        cmp(#pfx, "Resp", ref.pfx##_bits_Resp, dv.bits.resp);               \
        cmp(#pfx, "DataSource", ref.pfx##_bits_DataSource, dv.bits.data_source); \
        cmp(#pfx, "CBusy", ref.pfx##_bits_CBusy, dv.bits.c_busy);           \
        cmp(#pfx, "DBID", ref.pfx##_bits_DBID, dv.bits.dbid);               \
        cmp(#pfx, "DataID", ref.pfx##_bits_DataID, dv.bits.data_id);        \
        cmp(#pfx, "BE", ref.pfx##_bits_BE, dv.bits.be);                     \
        for (int w_ = 0; w_ < 4; ++w_) {                                    \
            uint64_t rw_ = (uint64_t(ref.pfx##_bits_Data[2 * w_ + 1]) << 32) | \
                           ref.pfx##_bits_Data[2 * w_];                     \
            cmp(#pfx, "Data", rw_, dv.bits.data[w_]);                       \
        }                                                                   \
    } while (0)

#define CMP_SNP(pfx, dv)                                                    \
    do {                                                                    \
        cmp(#pfx, "valid", ref.pfx##_valid, dv.valid);                      \
        cmp(#pfx, "QoS", ref.pfx##_bits_QoS, dv.bits.qos);                  \
        cmp(#pfx, "TgtID", ref.pfx##_bits_TgtID, dv.bits.tgt_id);           \
        cmp(#pfx, "SrcID", ref.pfx##_bits_SrcID, dv.bits.src_id);           \
        cmp(#pfx, "TxnID", ref.pfx##_bits_TxnID, dv.bits.txn_id);           \
        cmp(#pfx, "FwdNID", ref.pfx##_bits_FwdNID, dv.bits.fwd_nid);        \
        cmp(#pfx, "FwdTxnID", ref.pfx##_bits_FwdTxnID, dv.bits.fwd_txn_id); \
        cmp(#pfx, "Opcode", ref.pfx##_bits_Opcode, dv.bits.opcode);         \
        cmp(#pfx, "Addr", ref.pfx##_bits_Addr, dv.bits.addr);               \
        cmp(#pfx, "DoNotGoToSD", ref.pfx##_bits_DoNotGoToSD, dv.bits.do_not_go_to_sd); \
        cmp(#pfx, "RetToSrc", ref.pfx##_bits_RetToSrc, dv.bits.ret_to_src); \
    } while (0)

// L2 侧 eject SNP：RTL 端口无 TgtID（CHISNP 无此字段）
#define CMP_SNP_L2(pfx, dv)                                                 \
    do {                                                                    \
        cmp(#pfx, "valid", ref.pfx##_valid, dv.valid);                      \
        cmp(#pfx, "QoS", ref.pfx##_bits_QoS, dv.bits.qos);                  \
        cmp(#pfx, "SrcID", ref.pfx##_bits_SrcID, dv.bits.src_id);           \
        cmp(#pfx, "TxnID", ref.pfx##_bits_TxnID, dv.bits.txn_id);           \
        cmp(#pfx, "FwdNID", ref.pfx##_bits_FwdNID, dv.bits.fwd_nid);        \
        cmp(#pfx, "FwdTxnID", ref.pfx##_bits_FwdTxnID, dv.bits.fwd_txn_id); \
        cmp(#pfx, "Opcode", ref.pfx##_bits_Opcode, dv.bits.opcode);         \
        cmp(#pfx, "Addr", ref.pfx##_bits_Addr, dv.bits.addr);               \
        cmp(#pfx, "DoNotGoToSD", ref.pfx##_bits_DoNotGoToSD, dv.bits.do_not_go_to_sd); \
        cmp(#pfx, "RetToSrc", ref.pfx##_bits_RetToSrc, dv.bits.ret_to_src); \
    } while (0)

#define CMP_RXRDY(pfx, dv) cmp(#pfx, "ready", ref.pfx##_ready, dv)

#define DRV_REQ(pfx, f, vld)                                        \
    do {                                                            \
        ref.pfx##_valid = (vld);                                    \
        ref.pfx##_bits_QoS = f.qos;                                 \
        ref.pfx##_bits_TgtID = f.tgt_id;                            \
        ref.pfx##_bits_SrcID = f.src_id;                            \
        ref.pfx##_bits_TxnID = f.txn_id;                            \
        ref.pfx##_bits_Opcode = f.opcode;                           \
        ref.pfx##_bits_Size = f.size;                               \
        ref.pfx##_bits_Addr = f.addr;                               \
        ref.pfx##_bits_Order = f.order;                             \
        ref.pfx##_bits_MemAttr = f.mem_attr;                        \
        ref.pfx##_bits_SnpAttr = f.snp_attr;                        \
        ref.pfx##_bits_Excl = f.excl;                               \
        ref.pfx##_bits_ExpCompAck = f.exp_comp_ack;                 \
    } while (0)

#define DRV_RESP(pfx, f, vld)                                       \
    do {                                                            \
        ref.pfx##_valid = (vld);                                    \
        ref.pfx##_bits_QoS = f.qos;                                 \
        ref.pfx##_bits_TgtID = f.tgt_id;                            \
        ref.pfx##_bits_SrcID = f.src_id;                            \
        ref.pfx##_bits_TxnID = f.txn_id;                            \
        ref.pfx##_bits_Opcode = f.opcode;                           \
        ref.pfx##_bits_RespErr = f.resp_err;                        \
        ref.pfx##_bits_Resp = f.resp;                               \
        ref.pfx##_bits_FwdState = f.fwd_state;                      \
        ref.pfx##_bits_CBusy = f.c_busy;                            \
        ref.pfx##_bits_DBID = f.dbid;                               \
    } while (0)

#define DRV_DATA(pfx, f, vld)                                       \
    do {                                                            \
        ref.pfx##_valid = (vld);                                    \
        ref.pfx##_bits_QoS = f.qos;                                 \
        ref.pfx##_bits_TgtID = f.tgt_id;                            \
        ref.pfx##_bits_SrcID = f.src_id;                            \
        ref.pfx##_bits_TxnID = f.txn_id;                            \
        ref.pfx##_bits_HomeNID = f.home_nid;                        \
        ref.pfx##_bits_Opcode = f.opcode;                           \
        ref.pfx##_bits_RespErr = f.resp_err;                        \
        ref.pfx##_bits_Resp = f.resp;                               \
        ref.pfx##_bits_DataSource = f.data_source;                  \
        ref.pfx##_bits_CBusy = f.c_busy;                            \
        ref.pfx##_bits_DBID = f.dbid;                               \
        ref.pfx##_bits_DataID = f.data_id;                          \
        ref.pfx##_bits_BE = f.be;                                   \
        for (int w_ = 0; w_ < 4; ++w_) {                            \
            ref.pfx##_bits_Data[2 * w_] = uint32_t(f.data[w_]);     \
            ref.pfx##_bits_Data[2 * w_ + 1] = uint32_t(f.data[w_] >> 32); \
        }                                                           \
    } while (0)

#define DRV_SNP(pfx, f, vld)                                        \
    do {                                                            \
        ref.pfx##_valid = (vld);                                    \
        ref.pfx##_bits_QoS = f.qos;                                 \
        ref.pfx##_bits_TgtID = f.tgt_id;                            \
        ref.pfx##_bits_SrcID = f.src_id;                            \
        ref.pfx##_bits_TxnID = f.txn_id;                            \
        ref.pfx##_bits_FwdNID = f.fwd_nid;                          \
        ref.pfx##_bits_FwdTxnID = f.fwd_txn_id;                     \
        ref.pfx##_bits_Opcode = f.opcode;                           \
        ref.pfx##_bits_Addr = f.addr;                               \
        ref.pfx##_bits_DoNotGoToSD = f.do_not_go_to_sd;             \
        ref.pfx##_bits_RetToSrc = f.ret_to_src;                     \
    } while (0)

namespace {

RReqFlit genReq() {
    RReqFlit f;
    f.qos = randBits(4);
    f.tgt_id = randBits(11);
    f.src_id = randBits(11);
    f.txn_id = randBits(12);
    f.opcode = randBits(7);
    f.size = randBits(3);
    f.addr = randBits(48);
    f.order = randBits(2);
    f.mem_attr = randBits(4);
    f.snp_attr = rng() & 1;
    f.excl = rng() & 1;
    f.exp_comp_ack = rng() & 1;
    return f;
}

RespFlit genResp() {
    RespFlit f;
    f.qos = randBits(4);
    f.tgt_id = randBits(11);
    f.src_id = randBits(11);
    f.txn_id = randBits(12);
    f.opcode = randBits(5);
    f.resp_err = randBits(2);
    f.resp = randBits(3);
    f.fwd_state = randBits(3);
    f.c_busy = randBits(3);
    f.dbid = randBits(12);
    return f;
}

DataFlit genData() {
    DataFlit f;
    f.qos = randBits(4);
    f.tgt_id = randBits(11);
    f.src_id = randBits(11);
    f.txn_id = randBits(12);
    f.home_nid = randBits(11);
    f.opcode = randBits(4);
    f.resp_err = randBits(2);
    f.resp = randBits(3);
    f.data_source = randBits(8);
    f.c_busy = randBits(3);
    f.dbid = randBits(16);
    f.data_id = randBits(2);
    f.be = randBits(32);
    for (auto& x : f.data) x = rng();
    return f;
}

SnoopFlit genSnoop() {
    SnoopFlit f;
    f.qos = randBits(4);
    f.tgt_id = randBits(11);
    f.src_id = randBits(11);
    f.txn_id = randBits(12);
    f.fwd_nid = randBits(11);
    f.fwd_txn_id = randBits(12);
    f.opcode = randBits(5);
    f.addr = randBits(45);
    f.do_not_go_to_sd = rng() & 1;
    f.ret_to_src = rng() & 1;
    return f;
}

}  // namespace

int main(int argc, char** argv) {
    const uint32_t seeds[3] = {7, 2025, 998244353};
    const int      cycles  = argc > 1 ? std::atoi(argv[1]) : 200000;
    bool           allPass = true;

    for (uint32_t seed : seeds) {
        g_seed     = seed;
        g_checks   = 0;
        g_mismatch = 0;
        rng.seed(seed);

        VCcSocket ref;
        CcSocket     dut;
        dut.elaborate();

        // eject REQ 死端（RTL 集成中桥恒置 ready=false）
        dut.l2_tx_req_rdy.set(false);

        // 复位 + 空跑稳定
        ref.reset = 1;
        for (int t = 0; t < 16; ++t) {
            ref.clock = 0;
            ref.eval();
            ref.clock = 1;
            ref.eval();
        }
        ref.reset = 0;
        for (int t = 0; t < 32; ++t) {
            ref.clock = 0;
            ref.eval();
            ref.clock = 1;
            ref.eval();
            dut.clk.set(0);
            dut.eval();
            dut.clk.set(1);
            dut.eval();
        }

        // 驱动通道 pending（l2 inject 3 + ring eject 4）
        bool      pendIReq = false, pendIRsp = false, pendIDat = false;
        bool      pendEReq = false, pendERsp = false, pendEDat = false, pendESnp = false;
        RReqFlit  fIReq, fEReq;
        RespFlit  fIRsp, fERsp;
        DataFlit  fIDat, fEDat;
        SnoopFlit fESnp;

        for (g_cyc = 0; g_cyc < (uint64_t)cycles; ++g_cyc) {
            // ---- 1. 生成激励（同一 RNG，两侧共享）----
            if (!pendIReq && rng() % 10 < 3) { fIReq = genReq(); pendIReq = true; }
            if (!pendIRsp && rng() % 10 < 3) { fIRsp = genResp(); pendIRsp = true; }
            if (!pendIDat && rng() % 10 < 3) { fIDat = genData(); pendIDat = true; }
            if (!pendEReq && rng() % 10 < 2) { fEReq = genReq(); pendEReq = true; }
            if (!pendERsp && rng() % 10 < 3) { fERsp = genResp(); pendERsp = true; }
            if (!pendEDat && rng() % 10 < 3) { fEDat = genData(); pendEDat = true; }
            if (!pendESnp && rng() % 10 < 3) { fESnp = genSnoop(); pendESnp = true; }
            // ready（l2 eject 3 + ring inject 3）
            bool rdy[6];
            for (auto& r : rdy) r = rng() % 10 < 7;

            // ---- 2. 驱动两侧 ----
            DRV_REQ(l2_rx_req, fIReq, pendIReq);
            DRV_RESP(l2_rx_resp, fIRsp, pendIRsp);
            DRV_DATA(l2_rx_data, fIDat, pendIDat);
            DRV_REQ(ring_tx_req, fEReq, pendEReq);
            DRV_RESP(ring_tx_resp, fERsp, pendERsp);
            DRV_DATA(ring_tx_data, fEDat, pendEDat);
            DRV_SNP(ring_tx_snoop, fESnp, pendESnp);
            ref.l2_tx_resp_ready = rdy[0];
            ref.l2_tx_data_ready = rdy[1];
            ref.l2_tx_snoop_ready = rdy[2];
            ref.ring_rx_req_ready = rdy[3];
            ref.ring_rx_resp_ready = rdy[4];
            ref.ring_rx_data_ready = rdy[5];

            dut.l2_rx_req.set(Dec<RReqFlit>{pendIReq, fIReq});
            dut.l2_rx_resp.set(Dec<RespFlit>{pendIRsp, fIRsp});
            dut.l2_rx_data.set(Dec<DataFlit>{pendIDat, fIDat});
            dut.ring_tx_req.set(Dec<RReqFlit>{pendEReq, fEReq});
            dut.ring_tx_resp.set(Dec<RespFlit>{pendERsp, fERsp});
            dut.ring_tx_data.set(Dec<DataFlit>{pendEDat, fEDat});
            dut.ring_tx_snoop.set(Dec<SnoopFlit>{pendESnp, fESnp});
            dut.l2_tx_resp_rdy.set(rdy[0]);
            dut.l2_tx_data_rdy.set(rdy[1]);
            dut.l2_tx_snoop_rdy.set(rdy[2]);
            dut.ring_rx_req_rdy.set(rdy[3]);
            dut.ring_rx_resp_rdy.set(rdy[4]);
            dut.ring_rx_data_rdy.set(rdy[5]);

            // ---- 3. clk=0 eval + 采样比对 ----
            ref.clock = 0;
            ref.eval();
            dut.clk.set(0);
            dut.eval();

            CMP_RXRDY(l2_rx_req, dut.l2_rx_req_rdy.get());
            CMP_RXRDY(l2_rx_resp, dut.l2_rx_resp_rdy.get());
            CMP_RXRDY(l2_rx_data, dut.l2_rx_data_rdy.get());
            CMP_RXRDY(ring_tx_req, dut.ring_tx_req_rdy.get());
            CMP_RXRDY(ring_tx_resp, dut.ring_tx_resp_rdy.get());
            CMP_RXRDY(ring_tx_data, dut.ring_tx_data_rdy.get());
            CMP_RXRDY(ring_tx_snoop, dut.ring_tx_snoop_rdy.get());

            CMP_REQ(ring_rx_req, dut.ring_rx_req.get());
            CMP_RESP(ring_rx_resp, dut.ring_rx_resp.get());
            CMP_DATA(ring_rx_data, dut.ring_rx_data.get());
            CMP_RESP_L2(l2_tx_resp, dut.l2_tx_resp.get());
            CMP_DATA(l2_tx_data, dut.l2_tx_data.get());
            CMP_SNP_L2(l2_tx_snoop, dut.l2_tx_snoop.get());

            // fire 记录（采样时刻 valid && ready）
            if (pendIReq && ref.l2_rx_req_ready) pendIReq = false;
            if (pendIRsp && ref.l2_rx_resp_ready) pendIRsp = false;
            if (pendIDat && ref.l2_rx_data_ready) pendIDat = false;
            if (pendEReq && ref.ring_tx_req_ready) pendEReq = false;
            if (pendERsp && ref.ring_tx_resp_ready) pendERsp = false;
            if (pendEDat && ref.ring_tx_data_ready) pendEDat = false;
            if (pendESnp && ref.ring_tx_snoop_ready) pendESnp = false;

            // ---- 4. clk=1 eval 提交 ----
            ref.clock = 1;
            ref.eval();
            dut.clk.set(1);
            dut.eval();

            if (g_mismatch > 100) break;  // 失配爆炸早停
        }

        const bool pass = g_mismatch == 0;
        allPass         = allPass && pass;
        std::printf("%s seed=%u cycles=%d checks=%lu mismatches=%lu\n",
                    pass ? "PASS" : "FAIL", seed, cycles, (unsigned long)g_checks,
                    (unsigned long)g_mismatch);
        if (!pass) break;
    }
    return allPass ? 0 : 1;
}
