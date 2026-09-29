// DataBlock 对拍 harness：wolvicmod DataBlock vs refgenDj RTL DataBlock
// （dongjiang/data 真实源码，kunminghu-v3 单核 32MB 配置）。
// 激励：事务级调度器——alloc(reqDB) → task（fetch/save/repl/replMergeSave 四种
// dataOp 组合）→（save/merge 类经 rxDat 送写数据）→ resp → clean 释放；
// 附带 5% abandon（alloc 后直接 clean）与 5% updHnTxnID 改名。txDat 随机反压。
// 每拍比对：txDat 全字段、rxDat_rdy、reqDB_rdy、resp。

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

#include "VDataBlock.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include "model/dj/data.h"

using namespace wolvicmod;
using namespace zj::dj;

namespace {

uint64_t traceStart() {
    const char* s = std::getenv("DB_TRACE_START");
    return s ? std::strtoull(s, nullptr, 10) : UINT64_MAX;
}
uint64_t traceEnd() {
    const char* s = std::getenv("DB_TRACE_END");
    return s ? std::strtoull(s, nullptr, 10) : 0;
}
uint64_t dumpAt() {
    const char* s = std::getenv("DB_DUMP_AT");
    return s ? std::strtoull(s, nullptr, 10) : UINT64_MAX;
}

// DataFlit 字段直填（模型侧与 RTL 侧共用此结构驱动/比对）
void setRefFlit(VDataBlock& ref, const char* /*unused*/, bool rx, const DataFlit& f) {
    if (rx) {
        for (int w = 0; w < 4; ++w) {
            ref.io_rxDat_bits_Data[2 * w] = static_cast<uint32_t>(f.data[w]);
            ref.io_rxDat_bits_Data[2 * w + 1] = static_cast<uint32_t>(f.data[w] >> 32);
        }
        ref.io_rxDat_bits_BE = static_cast<uint32_t>(f.be);
        ref.io_rxDat_bits_DataID = f.data_id;
        ref.io_rxDat_bits_DBID = f.dbid;
        ref.io_rxDat_bits_CBusy = f.c_busy;
        ref.io_rxDat_bits_DataSource = f.data_source;
        ref.io_rxDat_bits_Resp = f.resp;
        ref.io_rxDat_bits_RespErr = f.resp_err;
        ref.io_rxDat_bits_Opcode = f.opcode;
        ref.io_rxDat_bits_HomeNID = f.home_nid;
        ref.io_rxDat_bits_TxnID = f.txn_id;
        ref.io_rxDat_bits_SrcID = f.src_id;
        ref.io_rxDat_bits_TgtID = f.tgt_id;
        ref.io_rxDat_bits_QoS = f.qos;
    }
}

void setRefTask(VDataBlock& ref, const DataTask& t) {
    ref.io_task_valid = true;
    ref.io_task_bits_hnTxnID = t.hnTxnID;
    ref.io_task_bits_dataOp_repl = t.dataOp.repl;
    ref.io_task_bits_dataOp_read = t.dataOp.read;
    ref.io_task_bits_dataOp_send = t.dataOp.send;
    ref.io_task_bits_dataOp_save = t.dataOp.save;
    ref.io_task_bits_dataOp_merge = t.dataOp.merge;
    ref.io_task_bits_ds_bank = t.ds.bank;
    ref.io_task_bits_ds_idx = t.ds.idx;
    ref.io_task_bits_dataVec_0 = t.dataVec & 1;
    ref.io_task_bits_dataVec_1 = (t.dataVec >> 1) & 1;
    ref.io_task_bits_qos = t.qos;
    for (int w = 0; w < 4; ++w) {
        ref.io_task_bits_txDat_Data[2 * w] = static_cast<uint32_t>(t.txDat.data[w]);
        ref.io_task_bits_txDat_Data[2 * w + 1] = static_cast<uint32_t>(t.txDat.data[w] >> 32);
    }
    ref.io_task_bits_txDat_BE = static_cast<uint32_t>(t.txDat.be);
    ref.io_task_bits_txDat_DataID = t.txDat.data_id;
    ref.io_task_bits_txDat_DBID = t.txDat.dbid;
    ref.io_task_bits_txDat_CBusy = t.txDat.c_busy;
    ref.io_task_bits_txDat_DataSource = t.txDat.data_source;
    ref.io_task_bits_txDat_Resp = t.txDat.resp;
    ref.io_task_bits_txDat_RespErr = t.txDat.resp_err;
    ref.io_task_bits_txDat_Opcode = t.txDat.opcode;
    ref.io_task_bits_txDat_HomeNID = t.txDat.home_nid;
    ref.io_task_bits_txDat_TxnID = t.txDat.txn_id;
    ref.io_task_bits_txDat_SrcID = t.txDat.src_id;
    ref.io_task_bits_txDat_TgtID = t.txDat.tgt_id;
    ref.io_task_bits_txDat_QoS = t.txDat.qos;
}

void clearRefInputs(VDataBlock& ref) {
    ref.io_txDat_ready = 0;
    ref.io_rxDat_valid = 0;
    ref.io_updHnTxnID_valid = 0;
    ref.io_reqDB_valid = 0;
    ref.io_task_valid = 0;
    ref.io_cleanDB_valid = 0;
    setRefFlit(ref, "", true, DataFlit{});
    setRefTask(ref, DataTask{});
    ref.io_task_valid = 0;
    ref.io_updHnTxnID_bits_before = 0;
    ref.io_updHnTxnID_bits_next = 0;
    ref.io_reqDB_bits_hnTxnID = 0;
    ref.io_reqDB_bits_dataVec_0 = 0;
    ref.io_reqDB_bits_dataVec_1 = 0;
    ref.io_cleanDB_bits_hnTxnID = 0;
    ref.io_cleanDB_bits_dataVec_0 = 0;
    ref.io_cleanDB_bits_dataVec_1 = 0;
}

// 事务调度状态
struct Txn {
    bool active = false;
    uint8_t id = 0;
    uint8_t dataVec = 0;
    uint8_t op = 0;  // 0=fetch 1=save 2=repl 3=replMergeSave
    uint8_t beatsSent = 0;
    uint64_t taskAt = UINT64_MAX;
    uint64_t cleanAt = UINT64_MAX;
    uint64_t updAt = UINT64_MAX;
    uint64_t rxAt = UINT64_MAX;
    bool taskDone = false;
    bool respSeen = false;
    DataTask task;
    std::array<DataFlit, kNrBeat> rxFlits;
};

DataFlit randFlit(std::mt19937& rng, uint8_t txnId, uint8_t dataId, uint8_t opcode) {
    DataFlit f;
    for (auto& w : f.data) w = (static_cast<uint64_t>(rng()) << 32) | rng();
    f.be = opcode == dat_op::kSnpRespData || opcode == dat_op::kSnpRespDataFwded
               ? 0xFFFFFFFFull
               : rng();  // 合并覆盖：随机 BE
    f.data_id = dataId;
    f.dbid = rng() & 0xFF;
    f.c_busy = rng() & 7;
    f.data_source = rng() & 0xFF;
    f.resp = rng() & 7;
    f.resp_err = rng() & 3;
    f.opcode = opcode;
    f.home_nid = rng() & 0x7FF;
    f.txn_id = txnId;
    f.src_id = rng() & 0x7FF;
    f.tgt_id = rng() & 0x7FF;
    f.qos = rng() & 0xF;
    return f;
}

void checkFlit(cosim::Stats& st, uint32_t seed, uint64_t c, const VDataBlock& ref,
               const DataFlit& d, cosim::Replay& rp) {
    for (int w = 0; w < 4; ++w) {
        const uint64_t rv = (static_cast<uint64_t>(ref.io_txDat_bits_Data[2 * w + 1]) << 32) |
                            ref.io_txDat_bits_Data[2 * w];
        cosim::check(st, "db", "DataBlock", seed, c,
                     (std::string("txdat_data") + std::to_string(w)).c_str(), rv, d.data[w], rp);
    }
    cosim::check(st, "db", "DataBlock", seed, c, "txdat_be", ref.io_txDat_bits_BE, d.be, rp);
    cosim::check(st, "db", "DataBlock", seed, c, "txdat_dataid", ref.io_txDat_bits_DataID,
                 d.data_id, rp);
    cosim::check(st, "db", "DataBlock", seed, c, "txdat_dbid", ref.io_txDat_bits_DBID, d.dbid,
                 rp);
    cosim::check(st, "db", "DataBlock", seed, c, "txdat_cbusy", ref.io_txDat_bits_CBusy,
                 d.c_busy, rp);
    cosim::check(st, "db", "DataBlock", seed, c, "txdat_datasource",
                 ref.io_txDat_bits_DataSource, d.data_source, rp);
    cosim::check(st, "db", "DataBlock", seed, c, "txdat_resp", ref.io_txDat_bits_Resp, d.resp,
                 rp);
    cosim::check(st, "db", "DataBlock", seed, c, "txdat_resperr", ref.io_txDat_bits_RespErr,
                 d.resp_err, rp);
    cosim::check(st, "db", "DataBlock", seed, c, "txdat_opcode", ref.io_txDat_bits_Opcode,
                 d.opcode, rp);
    cosim::check(st, "db", "DataBlock", seed, c, "txdat_homenid", ref.io_txDat_bits_HomeNID,
                 d.home_nid, rp);
    cosim::check(st, "db", "DataBlock", seed, c, "txdat_txnid", ref.io_txDat_bits_TxnID,
                 d.txn_id, rp);
    cosim::check(st, "db", "DataBlock", seed, c, "txdat_srcid", ref.io_txDat_bits_SrcID,
                 d.src_id, rp);
    cosim::check(st, "db", "DataBlock", seed, c, "txdat_tgtid", ref.io_txDat_bits_TgtID,
                 d.tgt_id, rp);
    cosim::check(st, "db", "DataBlock", seed, c, "txdat_qos", ref.io_txDat_bits_QoS, d.qos, rp);
}

uint64_t cosimDb(uint32_t seed, uint64_t cycles) {
    VDataBlock ref;
    DataBlock dut;
    dut.elaborate();
    dut.clk_en.set(true);
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;

    cosim::resetRef(ref, [&] { clearRefInputs(ref); });
    dut.tx_dat_rdy.set(false);
    dut.rx_dat.set({false, {}});
    dut.upd_hn_txn_id.set({false, {}});
    dut.req_db.set({false, {}});
    dut.task.set({false, {}});
    dut.clean_db.set({false, {}});
    dut.clk.set(0);
    dut.eval();

    std::array<Txn, 128> txns{};
    uint8_t nextTxn = 1;
    // 当前挂起（Decoupled 保持）
    bool pendReq = false;
    ReqDB pendReqBits{};
    bool pendRx = false;
    DataFlit pendRxFlit{};
    uint8_t pendRxTxn = 0;

    for (uint64_t c = 0; c < cycles; ++c) {
        const uint32_t pct = cosim::densityAt(c, cycles);

        // ---- 新 alloc 计划 ----
        uint32_t activeCnt = 0;
        for (const auto& t : txns) activeCnt += t.active;
        if (!pendReq && activeCnt < 40 && cosim::roll(rng, pct / 4)) {
            // 找空闲 txnID
            for (uint32_t k = 0; k < 128; ++k) {
                const uint8_t id = static_cast<uint8_t>((nextTxn + k) & 0x7F);
                if (id != 0 && !txns[id].active) {
                    auto& t = txns[id];
                    t = Txn{};
                    t.active = true;
                    t.id = id;
                    t.dataVec = 1 + (rng() % 3);  // 1/2/3
                    t.op = rng() % 4;
                    nextTxn = id + 1;
                    pendReq = true;
                    pendReqBits = {id, t.dataVec};
                    break;
                }
            }
        }

        // ---- 按调度发 task / upd / clean / rxDat ----
        Valid<DataTask> taskV{false, {}};
        Valid<UpdHnTxnID> updV{false, {}};
        Valid<CleanBits> cleanV{false, {}};
        if (!pendRx) {
            for (auto& t : txns) {
                if (!t.active) continue;
                // 同拍同类型动作只发一路；占不到窗口的顺延（否则 clean/task 被覆盖
                // → entry 永远等不到 → 僵尸 entry 与同 hnTxnID 重复 entry）
                if (t.updAt <= c && !updV.valid) {
                    updV = {true, {t.id, t.task.hnTxnID}};
                    t.updAt = UINT64_MAX;
                }
                if (t.taskAt <= c && !t.taskDone && !taskV.valid) {
                    taskV = {true, t.task};
                    t.taskDone = true;
                    t.taskAt = UINT64_MAX;
                }
                if (t.cleanAt <= c && !cleanV.valid) {
                    cleanV = {true, {t.task.hnTxnID, t.dataVec}};
                    t.cleanAt = UINT64_MAX;
                    t.active = false;  // 释放（全部 dataVec 一次放完；entry 内部随即 release）
                    continue;
                }
                if (t.taskDone && t.beatsSent != t.dataVec && t.rxAt <= c &&
                    (t.op == 1 || t.op == 3)) {
                    for (uint32_t b = 0; b < kNrBeat; ++b)
                        if (((t.dataVec >> b) & 1) && !((t.beatsSent >> b) & 1)) {
                            pendRx = true;
                            pendRxFlit = t.rxFlits[b];
                            pendRxTxn = t.id;
                            t.beatsSent |= (1u << b);
                            break;
                        }
                    t.rxAt = UINT64_MAX;
                }
            }
        }

        // task / upd / clean 计划的创建在 alloc fire 后（见下方 fire 处理）

        // ---- 驱动两侧 ----
        const bool txRdy = cosim::roll(rng, 70);
        dut.tx_dat_rdy.set(txRdy);
        ref.io_txDat_ready = txRdy;
        dut.task.set(taskV);
        setRefTask(ref, taskV.valid ? taskV.bits : DataTask{});
        ref.io_task_valid = taskV.valid;
        dut.upd_hn_txn_id.set(updV);
        ref.io_updHnTxnID_valid = updV.valid;
        ref.io_updHnTxnID_bits_before = updV.bits.before;
        ref.io_updHnTxnID_bits_next = updV.bits.next;
        dut.clean_db.set(cleanV);
        ref.io_cleanDB_valid = cleanV.valid;
        ref.io_cleanDB_bits_hnTxnID = cleanV.bits.hnTxnID;
        ref.io_cleanDB_bits_dataVec_0 = cleanV.bits.dataVec & 1;
        ref.io_cleanDB_bits_dataVec_1 = (cleanV.bits.dataVec >> 1) & 1;
        dut.req_db.set({pendReq, pendReqBits});
        ref.io_reqDB_valid = pendReq;
        ref.io_reqDB_bits_hnTxnID = pendReqBits.hnTxnID;
        ref.io_reqDB_bits_dataVec_0 = pendReqBits.dataVec & 1;
        ref.io_reqDB_bits_dataVec_1 = (pendReqBits.dataVec >> 1) & 1;
        dut.rx_dat.set({pendRx, pendRxFlit});
        setRefFlit(ref, "", true, pendRx ? pendRxFlit : DataFlit{});
        ref.io_rxDat_valid = pendRx;

        rp.push("cyc=" + std::to_string(c) + " req(v=" + std::to_string(pendReq) + ") task(v=" +
                std::to_string(taskV.valid) + ") rx(v=" + std::to_string(pendRx) + ")");

        cosim::phaseLow(ref, dut);

        if (c == dumpAt()) {
            for (uint32_t i = 0; i < kNrDataCM; ++i) {
                const auto st = dut.data_cm.w_views.get().states[i];
                if (st.valid)
                    std::cout << "  [dump] dcid=" << i << " txn=0x" << std::hex
                              << (uint32_t)st.bits.hnTxnID << " dv=" << (uint32_t)st.bits.dataVec
                              << " dbid=(" << (uint32_t)st.bits.dbidVec[0] << ","
                              << (uint32_t)st.bits.dbidVec[1] << ")" << std::dec << "\n";
            }
        }

        if (c >= traceStart() && c <= traceEnd()) {
            std::cout << "  [trace] cyc=" << c << " txdat ref(v=" << (int)ref.io_txDat_valid
                      << ",txn=0x" << std::hex << ref.io_txDat_bits_TxnID << ",did="
                      << (int)ref.io_txDat_bits_DataID << ",d0=0x" << ref.io_txDat_bits_Data[0]
                      << ",be=0x" << ref.io_txDat_bits_BE << ") dut(v="
                      << (int)dut.tx_dat.get().valid << ",txn=0x"
                      << (uint32_t)dut.tx_dat.get().bits.txn_id << ",did="
                      << (uint32_t)dut.tx_dat.get().bits.data_id << ",d0=0x"
                      << dut.tx_dat.get().bits.data[0] << ",be=0x" << dut.tx_dat.get().bits.be
                      << ")" << std::dec << " resp(v=" << (int)ref.io_resp_valid << ")\n";
            // datBuf 写口白盒：本拍提交的写（wr.wval）与 mask 组成
            if (dut.dat_buf.wr.get().wval) {
                std::cout << "    [buf] wr dbid=" << (uint32_t)dut.dat_buf.wr.get().waddr
                          << " dsWri=" << (int)dut.dat_buf.wr.get().dsWri
                          << " repl=" << (int)dut.dat_buf.wr.get().repl << " maskReg=0x"
                          << std::hex << dut.dat_buf.wr.get().mask << " beReg=0x"
                          << dut.dat_buf.wr.get().be << " ros=" << (int)dut.dat_buf.wr.get().readOrSnp
                          << " d0=0x" << ((uint64_t)dut.dat_buf.wr.get().wdata[3] << 24 |
                                          (uint64_t)dut.dat_buf.wr.get().wdata[2] << 16 |
                                          (uint64_t)dut.dat_buf.wr.get().wdata[1] << 8 |
                                          dut.dat_buf.wr.get().wdata[0])
                          << std::dec << "\n";
            }
            // 原 w_wri_val 窥探点：中转线已内联，按定义由 dsResp/fromCHI 重算
            if (dut.dat_buf.ds_resp.get().valid || dut.dat_buf.from_chi.get().valid) {
                std::cout << "    [buf] wrIn dsResp(v=" << (int)dut.dat_buf.ds_resp.get().valid
                          << ",dbid=" << (uint32_t)dut.dat_buf.ds_resp.get().bits.dbid
                          << ") fromCHI(v=" << (int)dut.dat_buf.from_chi.get().valid
                          << ",dbid=" << (uint32_t)dut.dat_buf.from_chi.get().bits.dbid
                          << ",be=0x" << std::hex << dut.dat_buf.from_chi.get().bits.dat.be
                          << std::dec << ")\n";
            }
            std::cout << "    [chi] cm.read_to_chi(v=" << (int)dut.data_cm.read_to_chi.get().valid
                      << ",dcid=" << (uint32_t)dut.data_cm.read_to_chi.get().bits.dcid
                      << ",beat=" << (uint32_t)dut.data_cm.read_to_chi.get().bits.beatNum
                      << ",dbid=" << (uint32_t)dut.data_cm.read_to_chi.get().bits.dbid
                      << ") buf.rdy=" << (int)dut.dat_buf.read_to_chi_rdy.get()
                      << " tochiq.free=" << dut.dat_buf.to_chi_q.free_num.get()
                      << " rchi_sft=" << (uint32_t)dut.dat_buf.rd_ctl.get().chiSft << "\n";
        }

        // ---- 比对 ----
        cosim::check(st, "db", "DataBlock", seed, c, "txdat_valid", ref.io_txDat_valid,
                     dut.tx_dat.get().valid, rp);
        if (ref.io_txDat_valid && dut.tx_dat.get().valid)
            checkFlit(st, seed, c, ref, dut.tx_dat.get().bits, rp);
        cosim::check(st, "db", "DataBlock", seed, c, "rxdat_rdy", ref.io_rxDat_ready,
                     dut.rx_dat_rdy.get(), rp);
        cosim::check(st, "db", "DataBlock", seed, c, "reqdb_rdy", ref.io_reqDB_ready,
                     dut.req_db_rdy.get(), rp);
        cosim::check(st, "db", "DataBlock", seed, c, "resp_valid", ref.io_resp_valid,
                     dut.resp.get().valid, rp);
        if (ref.io_resp_valid && dut.resp.get().valid)
            cosim::check(st, "db", "DataBlock", seed, c, "resp_txnid",
                         ref.io_resp_bits_hnTxnID, dut.resp.get().bits, rp);

        // fire 采样
        const bool reqFire = pendReq && dut.req_db_rdy.get();
        const bool rxFire = pendRx && dut.rx_dat_rdy.get();
        const uint8_t respId = dut.resp.get().bits;
        const bool respFire = dut.resp.get().valid;

        cosim::phaseHigh(ref, dut);

        // ---- fire 后续调度 ----
        if (reqFire) {
            pendReq = false;
            auto& t = txns[pendReqBits.hnTxnID];
            const bool abandon = cosim::roll(rng, 5);
            const bool rename = !abandon && cosim::roll(rng, 5);  // 与 abandon 互斥            // task 构造
            t.task.hnTxnID = t.id;
            t.task.dataVec = t.dataVec;
            t.task.qos = (rng() % 5 == 0) ? 0xF : (rng() & 0x7);
            t.task.dataOp = {};
            switch (t.op) {
                case 0:
                    t.task.dataOp.read = true;
                    t.task.dataOp.send = true;
                    break;
                case 1: t.task.dataOp.save = true; break;
                case 2: t.task.dataOp.repl = true; break;
                default:
                    t.task.dataOp.repl = true;
                    t.task.dataOp.merge = true;
                    t.task.dataOp.save = true;
                    break;
            }
            t.task.ds.set(catAddr(0, rng() % 24, rng() % 4, 13, rng() & 1), rng() % 16);
            t.task.txDat = randFlit(rng, t.id, 0, dat_op::kCompData);
            if (rename) {
                uint8_t newId = 0;
                for (uint32_t k = 1; k < 128; ++k) {
                    const uint8_t cand = static_cast<uint8_t>((nextTxn + k) & 0x7F);
                    if (cand != 0 && !txns[cand].active) {
                        newId = cand;
                        break;
                    }
                }
                if (newId != 0) {
                    t.task.hnTxnID = newId;
                    t.task.txDat.txn_id = newId;
                    t.updAt = c + 1;
                    // 调度记录整体迁往新 id 槽位（后续 resp/clean 按新 id 对齐）
                    txns[newId] = t;
                    txns[t.id].active = false;
                }
            }
            const auto& cur = txns[t.task.hnTxnID];
            if (abandon) {
                txns[t.id].cleanAt = c + 1 + rng() % 3;  // ALLOC 态直接 clean
            } else {
                txns[t.task.hnTxnID].taskAt = c + 2 + rng() % 3;  // ≥ alloc+2（改名留窗）
                if (cur.op == 1 || cur.op == 3) {
                    for (uint32_t b = 0; b < kNrBeat; ++b)
                        if ((cur.dataVec >> b) & 1) {
                            const uint8_t opc =
                                (cur.op == 3) ? dat_op::kCopyBackWriteData
                                              : (rng() % 2 ? dat_op::kNonCopyBackWriteData
                                                           : dat_op::kNCBWrDataCompAck);
                            txns[t.task.hnTxnID].rxFlits[b] =
                                randFlit(rng, t.task.hnTxnID, static_cast<uint8_t>(b * 2), opc);
                        }
                    txns[t.task.hnTxnID].rxAt =
                        txns[t.task.hnTxnID].taskAt + 1 + rng() % 4;
                }
            }
        }
        if (rxFire) pendRx = false;
        if (respFire && txns[respId].active && !txns[respId].respSeen) {
            txns[respId].respSeen = true;
            txns[respId].cleanAt = c + 1 + rng() % 16;
        }
        if (c >= traceStart() && c <= traceEnd()) {
            if (reqFire)
                std::cout << "  [txn] cyc=" << c << " ALLOC id=0x" << std::hex
                          << (uint32_t)pendReqBits.hnTxnID << " dv=" << (uint32_t)pendReqBits.dataVec
                          << std::dec << "\n";
            if (taskV.valid)
                std::cout << "  [txn] cyc=" << c << " TASK id=0x" << std::hex
                          << (uint32_t)taskV.bits.hnTxnID << " op(r=" << taskV.bits.dataOp.repl
                          << ",r=" << taskV.bits.dataOp.read << ",s=" << taskV.bits.dataOp.send
                          << ",v=" << taskV.bits.dataOp.save << ",m=" << taskV.bits.dataOp.merge
                          << ") ds(b=" << (uint32_t)taskV.bits.ds.bank << ",i=0x" << taskV.bits.ds.idx
                          << ") dv=" << (uint32_t)taskV.bits.dataVec << std::dec << "\n";
            if (rxFire)
                std::cout << "  [txn] cyc=" << c << " RX txn=0x" << std::hex
                          << (uint32_t)pendRxFlit.txn_id << " did=" << (uint32_t)pendRxFlit.data_id
                          << " be=0x" << pendRxFlit.be << " d0=0x" << pendRxFlit.data[0] << std::dec
                          << "\n";
            if (cleanV.valid)
                std::cout << "  [txn] cyc=" << c << " CLEAN id=0x" << std::hex
                          << (uint32_t)cleanV.bits.hnTxnID << " dv=" << (uint32_t)cleanV.bits.dataVec
                          << std::dec << "\n";
            if (respFire)
                std::cout << "  [txn] cyc=" << c << " RESP id=0x" << std::hex << (uint32_t)respId
                          << std::dec << "\n";
        }
    }

    std::cout << (st.mismatches == 0 ? "PASS" : "FAIL") << " db DataBlock seed=" << seed
              << " cycles=" << cycles << " checks=" << st.checks
              << " mismatches=" << st.mismatches << "\n";
    return st.mismatches;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {1u, 2u, 3u}) bad += cosimDb(seed, 100000);
    return bad == 0 ? 0 : 1;
}
