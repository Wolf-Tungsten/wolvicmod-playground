// 环（Ring）合成流量对拍：wolvicmod zj::ring::Ring vs RTL ZRING2X1C1P1D1M1G32
// （XiangShan emu 构建产物 build/rtl，kunminghu-v3 DefaultConfig + ZhuJiang，
// 与目标配置同源同参）。
//
// 协议（对齐 cosim/common.h）：每拍 = 驱动输入 → clk=0 eval → 采样比对全部
// 边界输出（rx_*_ready + tx_*_valid/bits 全字段）→ clk=1 eval 提交。
// 复位：mn_id_40_resetInject_0/1 拉高 16 拍后撤除，空跑 32 拍等复位链稳定
// （wolvicmod 侧初始态即复位后态，空跑期间无流量两侧状态等价）。
// 激励：每 rx 通道独立 pending（valid 保持到 fire），空时 30% 概率造新 flit；
// tx ready 70%。激励序列由同一 RNG 生成、两侧共享（先存数组再分别驱动）。
// 目标合法性：REQ（CC/RI 注入）由 RnRouter 译码产生；其余通道 tgt 随机选自
// 有对应弹出口的节点（否则 flit 永绕环——两侧等价但浪费覆盖率）。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "VZRing.h"
#include "verilated.h"
#include "wolvicmod/wolvicmod.h"
#include "model/ring/ring.h"

using namespace zj;
using namespace zj::chi;
using namespace zj::ring;

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

#define CMP_HREQ(pfx, dv)                                                   \
    do {                                                                    \
        CMP_REQ(pfx, dv);                                                   \
        cmp(#pfx, "ReturnNID", ref.pfx##_bits_ReturnNID, dv.bits.return_nid);     \
        cmp(#pfx, "ReturnTxnID", ref.pfx##_bits_ReturnTxnID, dv.bits.return_txn_id); \
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

#define DRV_HREQ(pfx, f, vld)                                       \
    do {                                                            \
        DRV_REQ(pfx, f, vld);                                       \
        ref.pfx##_bits_ReturnNID = f.return_nid;                    \
        ref.pfx##_bits_ReturnTxnID = f.return_txn_id;               \
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

// ---- 裁剪子集（RI/HI 桩节点的设备侧只消费这些字段，其余端口在 RTL 中不存在）----
#define CMP_RESP_RNI(pfx, dv)                                       \
    do {                                                            \
        cmp(#pfx, "valid", ref.pfx##_valid, dv.valid);              \
        cmp(#pfx, "SrcID", ref.pfx##_bits_SrcID, dv.bits.src_id);   \
        cmp(#pfx, "TxnID", ref.pfx##_bits_TxnID, dv.bits.txn_id);   \
        cmp(#pfx, "Opcode", ref.pfx##_bits_Opcode, dv.bits.opcode); \
        cmp(#pfx, "RespErr", ref.pfx##_bits_RespErr, dv.bits.resp_err); \
        cmp(#pfx, "DBID", ref.pfx##_bits_DBID, dv.bits.dbid);       \
    } while (0)

#define CMP_RESP_HNI(pfx, dv)                                       \
    do {                                                            \
        cmp(#pfx, "valid", ref.pfx##_valid, dv.valid);              \
        cmp(#pfx, "TxnID", ref.pfx##_bits_TxnID, dv.bits.txn_id);   \
        cmp(#pfx, "Opcode", ref.pfx##_bits_Opcode, dv.bits.opcode); \
    } while (0)

#define CMP_DATA_HNI(pfx, dv)                                       \
    do {                                                            \
        cmp(#pfx, "valid", ref.pfx##_valid, dv.valid);              \
        cmp(#pfx, "TxnID", ref.pfx##_bits_TxnID, dv.bits.txn_id);   \
        cmp(#pfx, "Opcode", ref.pfx##_bits_Opcode, dv.bits.opcode); \
        for (int w_ = 0; w_ < 4; ++w_) {                            \
            uint64_t rw_ = (uint64_t(ref.pfx##_bits_Data[2 * w_ + 1]) << 32) | \
                           ref.pfx##_bits_Data[2 * w_];             \
            cmp(#pfx, "Data", rw_, dv.bits.data[w_]);               \
        }                                                           \
    } while (0)

#define CMP_REQ_HNI(pfx, dv)                                            \
    do {                                                                \
        cmp(#pfx, "valid", ref.pfx##_valid, dv.valid);                  \
        cmp(#pfx, "QoS", ref.pfx##_bits_QoS, dv.bits.qos);              \
        cmp(#pfx, "SrcID", ref.pfx##_bits_SrcID, dv.bits.src_id);       \
        cmp(#pfx, "TxnID", ref.pfx##_bits_TxnID, dv.bits.txn_id);       \
        cmp(#pfx, "Opcode", ref.pfx##_bits_Opcode, dv.bits.opcode);     \
        cmp(#pfx, "Size", ref.pfx##_bits_Size, dv.bits.size);           \
        cmp(#pfx, "Addr", ref.pfx##_bits_Addr, dv.bits.addr);           \
        cmp(#pfx, "Order", ref.pfx##_bits_Order, dv.bits.order);        \
        cmp(#pfx, "MemAttr", ref.pfx##_bits_MemAttr, dv.bits.mem_attr); \
        cmp(#pfx, "ExpCompAck", ref.pfx##_bits_ExpCompAck, dv.bits.exp_comp_ack); \
    } while (0)

// ---- 注入侧裁剪子集：只写存在的端口；被裁字段在激励中必须为 0 ----
#define DRV_REQ_RNI(pfx, f, vld)                                \
    do {                                                        \
        ref.pfx##_valid = (vld);                                \
        ref.pfx##_bits_QoS = f.qos;                             \
        ref.pfx##_bits_SrcID = f.src_id;                        \
        ref.pfx##_bits_TxnID = f.txn_id;                        \
        ref.pfx##_bits_Opcode = f.opcode;                       \
        ref.pfx##_bits_Size = f.size;                           \
        ref.pfx##_bits_Addr = f.addr;                           \
        ref.pfx##_bits_Order = f.order;                         \
        ref.pfx##_bits_MemAttr = f.mem_attr;                    \
        ref.pfx##_bits_SnpAttr = f.snp_attr;                    \
        ref.pfx##_bits_ExpCompAck = f.exp_comp_ack;             \
    } while (0)  /* 无 TgtID（RnRouter 译码覆写，端口被裁） */

#define DRV_RESP_RNI(pfx, f, vld)                               \
    do {                                                        \
        ref.pfx##_valid = (vld);                                \
        ref.pfx##_bits_Opcode = f.opcode;                       \
        ref.pfx##_bits_SrcID = f.src_id;                        \
        ref.pfx##_bits_TgtID = f.tgt_id;                        \
        ref.pfx##_bits_TxnID = f.txn_id;                        \
    } while (0)  /* 其余字段 RTL 侧绑 0 */

#define DRV_RESP_HNI(pfx, f, vld)                               \
    do {                                                        \
        ref.pfx##_valid = (vld);                                \
        ref.pfx##_bits_QoS = f.qos;                             \
        ref.pfx##_bits_TgtID = f.tgt_id;                        \
        ref.pfx##_bits_TxnID = f.txn_id;                        \
        ref.pfx##_bits_Opcode = f.opcode;                       \
        ref.pfx##_bits_DBID = f.dbid;                           \
    } while (0)  /* SrcID 亦被绑 0（盖章 aid=0） */

namespace {

// 合法目标表（有对应弹出口的节点 gid；REQ 由译码产生故不在此列）
// RI(3)/HI(4) 的 RSP/DAT 弹出口在实际集成中被裁剪（桩节点无此流量，见
// ZRING 端口清单：rni/hni 的 tx_resp/tx_data ready 不存在），不作为目标
constexpr int kRspTgts[] = {0, 1, 2, 5, 7};
constexpr int kDatTgts[] = {0, 1, 2, 5, 6, 7};

int pickTgt(const int* tbl, int n, int self) {  // n 须与表长一致
    int t;
    do {
        t = tbl[rng() % n];
    } while (t == self);
    return t;
}

RReqFlit genReq(int selfGid) {
    RReqFlit f;
    f.qos = randBits(4);
    f.txn_id = randBits(12);
    f.src_id = randBits(11);
    f.opcode = randBits(7);
    f.size = 6;
    f.order = randBits(2);
    f.snp_attr = rng() & 1;
    f.excl = rng() & 1;
    f.exp_comp_ack = rng() & 1;
    if (selfGid == 3) f.excl = false;  // RI 的 Excl 端口被裁（绑 0）
    // RnRouter 译码地址：80% 非 device（addr[12] 随机 → HF bank），
    // 10% device 非 CC 窗口（→defaultHni HI），10% device CC 窗口（仅 RI 合法；
    // CC 自注自身在环上不可路由，RTL 触发 PopCount 断言）
    const unsigned r = rng() % 10;
    if (r < 8) {
        f.mem_attr = 0;
        f.addr = randBits(44);
    } else if (r == 8 || selfGid == 1) {
        f.mem_attr = 0b0010;
        f.addr = (randBits(24) | 1) << 20;  // addr[43:20] != 0
    } else {
        f.mem_attr = 0b0010;
        f.addr = randBits(20);  // addr[43:20]==0 → CC 窗口
    }
    f.tgt_id = 0;  // 由 RnRouter 译码覆写
    return f;
}

RespFlit genResp(int selfGid) {
    RespFlit f;
    f.qos = randBits(4);
    f.tgt_id = uint16_t(pickTgt(kRspTgts, 5, selfGid) << 3);
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

// RI(3)/HI(4) 注入的 RSP：被绑 0 的字段必须在激励中清零（与 RTL 端口裁剪一致）
void pruneRspForSrc(int selfGid, RespFlit& f) {
    if (selfGid == 3) {  // RI：仅 Opcode/SrcID/TgtID/TxnID 存在
        f.qos = 0; f.resp_err = 0; f.resp = 0; f.fwd_state = 0; f.c_busy = 0; f.dbid = 0;
    } else if (selfGid == 4) {  // HI：SrcID/RespErr/Resp/FwdState/CBusy 绑 0
        f.src_id = 0; f.resp_err = 0; f.resp = 0; f.fwd_state = 0; f.c_busy = 0;
    }
}

DataFlit genData(int selfGid) {
    DataFlit f;
    f.qos = randBits(4);
    f.tgt_id = uint16_t(pickTgt(kDatTgts, 6, selfGid) << 3);
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

SnoopFlit genSnoop() {  // 仅 HF 注入，tgt 恒 CC（gid1）
    SnoopFlit f;
    f.qos = randBits(4);
    f.tgt_id = 0x08;
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

HReqFlit genErq() {  // HF/HI 注入，tgt 恒 S（gid6）
    HReqFlit f;
    f.qos = randBits(4);
    f.tgt_id = 0x30;
    f.src_id = randBits(11);
    f.txn_id = randBits(12);
    f.return_nid = randBits(11);
    f.return_txn_id = randBits(12);
    f.opcode = randBits(7);
    f.size = 6;
    f.addr = randBits(48);
    f.order = randBits(2);
    f.mem_attr = randBits(4);
    f.snp_attr = rng() & 1;
    f.excl = rng() & 1;
    f.exp_comp_ack = rng() & 1;
    return f;
}

}  // namespace

int main(int argc, char** argv) {
    const uint32_t seeds[3] = {7, 2025, 998244353};
    const int      cycles   = argc > 1 ? std::atoi(argv[1]) : 50000;
    bool           allPass  = true;

    for (uint32_t seed : seeds) {
        g_seed     = seed;
        g_checks   = 0;
        g_mismatch = 0;
        rng.seed(seed);

        VZRing ref;
        Ring   dut;
        dut.elaborate();

        // 复位 + 空跑稳定
        ref.io_ci                = 0;
        dut.ci.set(0);
        // RTL ZRING 边界被裁剪的口：HI 的 ERQ 注入（rx_req）不存在 → 常 0；
        // RI/HI 的 tx_resp/data ready 被裁 → 常 1（无流量到达，行为无关）
        dut.n4_rx_req.set(Dec<HReqFlit>{});
        dut.n3_tx_resp_rdy.set(true);
        dut.n4_tx_resp_rdy.set(true);
        dut.n4_tx_data_rdy.set(true);
        ref.mn_id_40_resetInject_0 = 1;
        ref.mn_id_40_resetInject_1 = 1;
        for (int t = 0; t < 16; ++t) {
            ref.clock = 0;
            ref.eval();
            ref.clock = 1;
            ref.eval();
        }
        ref.mn_id_40_resetInject_0 = 0;
        ref.mn_id_40_resetInject_1 = 0;
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

        // 通道驱动状态（索引 = gid）
        bool      pendReq[10]{}, pendRsp[10]{}, pendDat[10]{}, pendSnp[10]{}, pendErq[10]{};
        RReqFlit  fReq[10];
        RespFlit  fRsp[10];
        DataFlit  fDat[10];
        SnoopFlit fSnp[10];
        HReqFlit  fErq[10];

        for (g_cyc = 0; g_cyc < (uint64_t)cycles; ++g_cyc) {
            // ---- 1. 生成激励（同一 RNG，两侧共享）----
            for (int i : {1, 3})
                if (!pendReq[i] && rng() % 10 < 3) {
                    fReq[i]    = genReq(i);
                    pendReq[i] = true;
                }
            for (int i : {0, 1, 2, 3, 4, 5, 6, 7}) {
                if (!pendRsp[i] && rng() % 10 < 3) {
                    fRsp[i]    = genResp(i);
                    pruneRspForSrc(i, fRsp[i]);
                    pendRsp[i] = true;
                }
                if (!pendDat[i] && rng() % 10 < 3) {
                    fDat[i]    = genData(i);
                    pendDat[i] = true;
                }
            }
            for (int i : {0, 2, 5, 7})
                if (!pendSnp[i] && rng() % 10 < 2) {
                    fSnp[i]    = genSnoop();
                    pendSnp[i] = true;
                }
            for (int i : {0, 2, 5, 7})
                if (!pendErq[i] && rng() % 10 < 2) {
                    fErq[i]    = genErq();
                    pendErq[i] = true;
                }
            // tx ready（23 个 tx 通道，顺序固定）
            bool rdy[20];
            for (auto& r : rdy) r = rng() % 10 < 7;

            // ---- 2. 驱动两侧 ----
            DRV_REQ(ccn_id_8_rx_req, fReq[1], pendReq[1]);
            DRV_REQ_RNI(rni_id_18_rx_req, fReq[3], pendReq[3]);
            DRV_RESP(hnf_bank_0_id_0_rx_resp, fRsp[0], pendRsp[0]);
            DRV_RESP(ccn_id_8_rx_resp, fRsp[1], pendRsp[1]);
            DRV_RESP(hnf_bank_1_id_10_rx_resp, fRsp[2], pendRsp[2]);
            DRV_RESP_RNI(rni_id_18_rx_resp, fRsp[3], pendRsp[3]);
            DRV_RESP_HNI(hni_id_20_rx_resp, fRsp[4], pendRsp[4]);
            DRV_RESP(hnf_bank_1_id_28_rx_resp, fRsp[5], pendRsp[5]);
            DRV_RESP(sn_id_30_rx_resp, fRsp[6], pendRsp[6]);
            DRV_RESP(hnf_bank_0_id_38_rx_resp, fRsp[7], pendRsp[7]);
            DRV_DATA(hnf_bank_0_id_0_rx_data, fDat[0], pendDat[0]);
            DRV_DATA(ccn_id_8_rx_data, fDat[1], pendDat[1]);
            DRV_DATA(hnf_bank_1_id_10_rx_data, fDat[2], pendDat[2]);
            DRV_DATA(rni_id_18_rx_data, fDat[3], pendDat[3]);
            DRV_DATA(hni_id_20_rx_data, fDat[4], pendDat[4]);
            DRV_DATA(hnf_bank_1_id_28_rx_data, fDat[5], pendDat[5]);
            DRV_DATA(sn_id_30_rx_data, fDat[6], pendDat[6]);
            DRV_DATA(hnf_bank_0_id_38_rx_data, fDat[7], pendDat[7]);
            DRV_SNP(hnf_bank_0_id_0_rx_snoop, fSnp[0], pendSnp[0]);
            DRV_SNP(hnf_bank_1_id_10_rx_snoop, fSnp[2], pendSnp[2]);
            DRV_SNP(hnf_bank_1_id_28_rx_snoop, fSnp[5], pendSnp[5]);
            DRV_SNP(hnf_bank_0_id_38_rx_snoop, fSnp[7], pendSnp[7]);
            DRV_HREQ(hnf_bank_0_id_0_rx_req, fErq[0], pendErq[0]);
            DRV_HREQ(hnf_bank_1_id_10_rx_req, fErq[2], pendErq[2]);
            DRV_HREQ(hnf_bank_1_id_28_rx_req, fErq[5], pendErq[5]);
            DRV_HREQ(hnf_bank_0_id_38_rx_req, fErq[7], pendErq[7]);

            ref.hnf_bank_0_id_0_tx_req_ready = rdy[0];
            ref.hnf_bank_0_id_0_tx_resp_ready = rdy[1];
            ref.hnf_bank_0_id_0_tx_data_ready = rdy[2];
            ref.ccn_id_8_tx_req_ready = rdy[3];
            ref.ccn_id_8_tx_resp_ready = rdy[4];
            ref.ccn_id_8_tx_data_ready = rdy[5];
            ref.ccn_id_8_tx_snoop_ready = rdy[6];
            ref.hnf_bank_1_id_10_tx_req_ready = rdy[7];
            ref.hnf_bank_1_id_10_tx_resp_ready = rdy[8];
            ref.hnf_bank_1_id_10_tx_data_ready = rdy[9];
            ref.rni_id_18_tx_data_ready = rdy[10];
            ref.hni_id_20_tx_req_ready = rdy[11];
            ref.hnf_bank_1_id_28_tx_req_ready = rdy[12];
            ref.hnf_bank_1_id_28_tx_resp_ready = rdy[13];
            ref.hnf_bank_1_id_28_tx_data_ready = rdy[14];
            ref.sn_id_30_tx_req_ready = rdy[15];
            ref.sn_id_30_tx_data_ready = rdy[16];
            ref.hnf_bank_0_id_38_tx_req_ready = rdy[17];
            ref.hnf_bank_0_id_38_tx_resp_ready = rdy[18];
            ref.hnf_bank_0_id_38_tx_data_ready = rdy[19];

            dut.n1_rx_req.set(Dec<RReqFlit>{pendReq[1], fReq[1]});
            dut.n3_rx_req.set(Dec<RReqFlit>{pendReq[3], fReq[3]});
            dut.n0_rx_resp.set(Dec<RespFlit>{pendRsp[0], fRsp[0]});
            dut.n1_rx_resp.set(Dec<RespFlit>{pendRsp[1], fRsp[1]});
            dut.n2_rx_resp.set(Dec<RespFlit>{pendRsp[2], fRsp[2]});
            dut.n3_rx_resp.set(Dec<RespFlit>{pendRsp[3], fRsp[3]});
            dut.n4_rx_resp.set(Dec<RespFlit>{pendRsp[4], fRsp[4]});
            dut.n5_rx_resp.set(Dec<RespFlit>{pendRsp[5], fRsp[5]});
            dut.n6_rx_resp.set(Dec<RespFlit>{pendRsp[6], fRsp[6]});
            dut.n7_rx_resp.set(Dec<RespFlit>{pendRsp[7], fRsp[7]});
            dut.n0_rx_data.set(Dec<DataFlit>{pendDat[0], fDat[0]});
            dut.n1_rx_data.set(Dec<DataFlit>{pendDat[1], fDat[1]});
            dut.n2_rx_data.set(Dec<DataFlit>{pendDat[2], fDat[2]});
            dut.n3_rx_data.set(Dec<DataFlit>{pendDat[3], fDat[3]});
            dut.n4_rx_data.set(Dec<DataFlit>{pendDat[4], fDat[4]});
            dut.n5_rx_data.set(Dec<DataFlit>{pendDat[5], fDat[5]});
            dut.n6_rx_data.set(Dec<DataFlit>{pendDat[6], fDat[6]});
            dut.n7_rx_data.set(Dec<DataFlit>{pendDat[7], fDat[7]});
            dut.n0_rx_snoop.set(Dec<SnoopFlit>{pendSnp[0], fSnp[0]});
            dut.n2_rx_snoop.set(Dec<SnoopFlit>{pendSnp[2], fSnp[2]});
            dut.n5_rx_snoop.set(Dec<SnoopFlit>{pendSnp[5], fSnp[5]});
            dut.n7_rx_snoop.set(Dec<SnoopFlit>{pendSnp[7], fSnp[7]});
            dut.n0_rx_req.set(Dec<HReqFlit>{pendErq[0], fErq[0]});
            dut.n2_rx_req.set(Dec<HReqFlit>{pendErq[2], fErq[2]});
            dut.n4_rx_req.set(Dec<HReqFlit>{pendErq[4], fErq[4]});
            dut.n5_rx_req.set(Dec<HReqFlit>{pendErq[5], fErq[5]});
            dut.n7_rx_req.set(Dec<HReqFlit>{pendErq[7], fErq[7]});

            dut.n0_tx_req_rdy.set(rdy[0]);
            dut.n0_tx_resp_rdy.set(rdy[1]);
            dut.n0_tx_data_rdy.set(rdy[2]);
            dut.n1_tx_req_rdy.set(rdy[3]);
            dut.n1_tx_resp_rdy.set(rdy[4]);
            dut.n1_tx_data_rdy.set(rdy[5]);
            dut.n1_tx_snoop_rdy.set(rdy[6]);
            dut.n2_tx_req_rdy.set(rdy[7]);
            dut.n2_tx_resp_rdy.set(rdy[8]);
            dut.n2_tx_data_rdy.set(rdy[9]);
            dut.n3_tx_data_rdy.set(rdy[10]);
            dut.n4_tx_req_rdy.set(rdy[11]);
            dut.n5_tx_req_rdy.set(rdy[12]);
            dut.n5_tx_resp_rdy.set(rdy[13]);
            dut.n5_tx_data_rdy.set(rdy[14]);
            dut.n6_tx_req_rdy.set(rdy[15]);
            dut.n6_tx_data_rdy.set(rdy[16]);
            dut.n7_tx_req_rdy.set(rdy[17]);
            dut.n7_tx_resp_rdy.set(rdy[18]);
            dut.n7_tx_data_rdy.set(rdy[19]);

            // ---- 3. clk=0 eval + 采样比对 ----
            ref.clock = 0;
            ref.eval();
            dut.clk.set(0);
            dut.eval();

            CMP_RXRDY(ccn_id_8_rx_req, dut.n1_rx_req_rdy.get());
            CMP_RXRDY(rni_id_18_rx_req, dut.n3_rx_req_rdy.get());
            CMP_RXRDY(hnf_bank_0_id_0_rx_resp, dut.n0_rx_resp_rdy.get());
            CMP_RXRDY(ccn_id_8_rx_resp, dut.n1_rx_resp_rdy.get());
            CMP_RXRDY(hnf_bank_1_id_10_rx_resp, dut.n2_rx_resp_rdy.get());
            CMP_RXRDY(rni_id_18_rx_resp, dut.n3_rx_resp_rdy.get());
            CMP_RXRDY(hni_id_20_rx_resp, dut.n4_rx_resp_rdy.get());
            CMP_RXRDY(hnf_bank_1_id_28_rx_resp, dut.n5_rx_resp_rdy.get());
            CMP_RXRDY(sn_id_30_rx_resp, dut.n6_rx_resp_rdy.get());
            CMP_RXRDY(hnf_bank_0_id_38_rx_resp, dut.n7_rx_resp_rdy.get());
            CMP_RXRDY(hnf_bank_0_id_0_rx_data, dut.n0_rx_data_rdy.get());
            CMP_RXRDY(ccn_id_8_rx_data, dut.n1_rx_data_rdy.get());
            CMP_RXRDY(hnf_bank_1_id_10_rx_data, dut.n2_rx_data_rdy.get());
            CMP_RXRDY(rni_id_18_rx_data, dut.n3_rx_data_rdy.get());
            CMP_RXRDY(hni_id_20_rx_data, dut.n4_rx_data_rdy.get());
            CMP_RXRDY(hnf_bank_1_id_28_rx_data, dut.n5_rx_data_rdy.get());
            CMP_RXRDY(sn_id_30_rx_data, dut.n6_rx_data_rdy.get());
            CMP_RXRDY(hnf_bank_0_id_38_rx_data, dut.n7_rx_data_rdy.get());
            CMP_RXRDY(hnf_bank_0_id_0_rx_snoop, dut.n0_rx_snoop_rdy.get());
            CMP_RXRDY(hnf_bank_1_id_10_rx_snoop, dut.n2_rx_snoop_rdy.get());
            CMP_RXRDY(hnf_bank_1_id_28_rx_snoop, dut.n5_rx_snoop_rdy.get());
            CMP_RXRDY(hnf_bank_0_id_38_rx_snoop, dut.n7_rx_snoop_rdy.get());
            CMP_RXRDY(hnf_bank_0_id_0_rx_req, dut.n0_rx_req_rdy.get());
            CMP_RXRDY(hnf_bank_1_id_10_rx_req, dut.n2_rx_req_rdy.get());
            CMP_RXRDY(hnf_bank_1_id_28_rx_req, dut.n5_rx_req_rdy.get());
            CMP_RXRDY(hnf_bank_0_id_38_rx_req, dut.n7_rx_req_rdy.get());

            CMP_REQ(hnf_bank_0_id_0_tx_req, dut.n0_tx_req.get());
            CMP_REQ(ccn_id_8_tx_req, dut.n1_tx_req.get());
            CMP_REQ(hnf_bank_1_id_10_tx_req, dut.n2_tx_req.get());
            CMP_REQ_HNI(hni_id_20_tx_req, dut.n4_tx_req.get());
            CMP_REQ(hnf_bank_1_id_28_tx_req, dut.n5_tx_req.get());
            CMP_REQ(hnf_bank_0_id_38_tx_req, dut.n7_tx_req.get());
            CMP_RESP(hnf_bank_0_id_0_tx_resp, dut.n0_tx_resp.get());
            CMP_RESP(ccn_id_8_tx_resp, dut.n1_tx_resp.get());
            CMP_RESP(hnf_bank_1_id_10_tx_resp, dut.n2_tx_resp.get());
            CMP_RESP_RNI(rni_id_18_tx_resp, dut.n3_tx_resp.get());
            CMP_RESP_HNI(hni_id_20_tx_resp, dut.n4_tx_resp.get());
            CMP_RESP(hnf_bank_1_id_28_tx_resp, dut.n5_tx_resp.get());
            CMP_RESP(hnf_bank_0_id_38_tx_resp, dut.n7_tx_resp.get());
            CMP_DATA(hnf_bank_0_id_0_tx_data, dut.n0_tx_data.get());
            CMP_DATA(ccn_id_8_tx_data, dut.n1_tx_data.get());
            CMP_DATA(hnf_bank_1_id_10_tx_data, dut.n2_tx_data.get());
            CMP_DATA(rni_id_18_tx_data, dut.n3_tx_data.get());
            CMP_DATA_HNI(hni_id_20_tx_data, dut.n4_tx_data.get());
            CMP_DATA(hnf_bank_1_id_28_tx_data, dut.n5_tx_data.get());
            CMP_DATA(sn_id_30_tx_data, dut.n6_tx_data.get());
            CMP_DATA(hnf_bank_0_id_38_tx_data, dut.n7_tx_data.get());
            CMP_HREQ(sn_id_30_tx_req, dut.n6_tx_req.get());
            CMP_SNP(ccn_id_8_tx_snoop, dut.n1_tx_snoop.get());

            // fire 记录（采样时刻 valid && ready）
            if (pendReq[1] && ref.ccn_id_8_rx_req_ready) pendReq[1] = false;
            if (pendReq[3] && ref.rni_id_18_rx_req_ready) pendReq[3] = false;
            struct RdyRec {
                bool* pend;
                bool  rdy;
            };
            const RdyRec rspFire[8] = {
                {&pendRsp[0], (bool)ref.hnf_bank_0_id_0_rx_resp_ready},
                {&pendRsp[1], (bool)ref.ccn_id_8_rx_resp_ready},
                {&pendRsp[2], (bool)ref.hnf_bank_1_id_10_rx_resp_ready},
                {&pendRsp[3], (bool)ref.rni_id_18_rx_resp_ready},
                {&pendRsp[4], (bool)ref.hni_id_20_rx_resp_ready},
                {&pendRsp[5], (bool)ref.hnf_bank_1_id_28_rx_resp_ready},
                {&pendRsp[6], (bool)ref.sn_id_30_rx_resp_ready},
                {&pendRsp[7], (bool)ref.hnf_bank_0_id_38_rx_resp_ready},
            };
            const RdyRec datFire[8] = {
                {&pendDat[0], (bool)ref.hnf_bank_0_id_0_rx_data_ready},
                {&pendDat[1], (bool)ref.ccn_id_8_rx_data_ready},
                {&pendDat[2], (bool)ref.hnf_bank_1_id_10_rx_data_ready},
                {&pendDat[3], (bool)ref.rni_id_18_rx_data_ready},
                {&pendDat[4], (bool)ref.hni_id_20_rx_data_ready},
                {&pendDat[5], (bool)ref.hnf_bank_1_id_28_rx_data_ready},
                {&pendDat[6], (bool)ref.sn_id_30_rx_data_ready},
                {&pendDat[7], (bool)ref.hnf_bank_0_id_38_rx_data_ready},
            };
            const RdyRec snpFire[4] = {
                {&pendSnp[0], (bool)ref.hnf_bank_0_id_0_rx_snoop_ready},
                {&pendSnp[2], (bool)ref.hnf_bank_1_id_10_rx_snoop_ready},
                {&pendSnp[5], (bool)ref.hnf_bank_1_id_28_rx_snoop_ready},
                {&pendSnp[7], (bool)ref.hnf_bank_0_id_38_rx_snoop_ready},
            };
            const RdyRec erqFire[4] = {
                {&pendErq[0], (bool)ref.hnf_bank_0_id_0_rx_req_ready},
                {&pendErq[2], (bool)ref.hnf_bank_1_id_10_rx_req_ready},
                {&pendErq[5], (bool)ref.hnf_bank_1_id_28_rx_req_ready},
                {&pendErq[7], (bool)ref.hnf_bank_0_id_38_rx_req_ready},
            };
            for (const auto& r : rspFire)
                if (*r.pend && r.rdy) *r.pend = false;
            for (const auto& r : datFire)
                if (*r.pend && r.rdy) *r.pend = false;
            for (const auto& r : snpFire)
                if (*r.pend && r.rdy) *r.pend = false;
            for (const auto& r : erqFire)
                if (*r.pend && r.rdy) *r.pend = false;

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
