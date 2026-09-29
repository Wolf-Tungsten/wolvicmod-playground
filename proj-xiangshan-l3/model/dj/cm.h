#pragma once

// SnoopCM / ReadCM / WriteCM：对齐 backend/{SnoopCM,ReadCM,WriteCM}.scala
// （语义 §5.5-5.7；本配置无 BBN，nest/ack 路径不到达）。
// 拍平建模：SnoopEntry/ReadEntry/WriteEntry 不做子模块——N 项表项状态各并为
// 父模块内 REG(std::array<EntryV, N>) 一条 update 循环；per-entry 组合输出
// 数组化为 wire 直喂仲裁器；Alloc/QosRR 仲裁器子模块不动。

#include <array>
#include <cstdint>

#include "model/dj/backend_types.h"
#include "model/dj/qosrr.h"
#include "prefab/xsarb.h"
#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"

namespace zj::dj {

using wolvicmod::In;
using wolvicmod::Out;
using wolvicmod::prefab::Valid;

// ---------------- SnoopCM ----------------

class SnoopCM : public wolvicmod::Module {
public:
    static constexpr uint32_t kEntries = 32;
    static constexpr uint8_t kFree = 0, kPreSnp = 1, kSendSnp = 2, kWaitResp = 3, kRespCmt = 4;

    struct EntryV {  // 一个表项的全部状态（原 SnoopEntry::SnpReg）
        uint8_t state = kFree;
        bool alrSend = false;    // alrSendVec（nrSfMetas=1）
        bool getResp = false;    // getRespVec
        uint8_t getData = 0;     // getDataVec（2bit）
        CMTask task;
        uint32_t taskInst = 0;   // TaskInst 打包
        uint8_t respErr = 0;

        bool operator==(const EntryV&) const = default;
    };
    using EntryArr = std::array<EntryV, kEntries>;

    IN(bool, clk);
    IN(uint8_t, cfg_ci);
    IN(Valid<CMTask>, alloc);
    OUT(bool, alloc_rdy);
    OUT(Valid<CMResp>, resp);
    IN(bool, resp_rdy);
    OUT(Valid<SnoopFlit>, tx_snp);
    IN(bool, tx_snp_rdy);
    IN(Valid<RespFlit>, rx_rsp);
    IN(Valid<DataFlit>, rx_dat);

    REG(EntryArr, entries);
    using AllocT = zj::prefab::Alloc<CMTask, kEntries>;
    MOD(AllocT, alloc_arb);
    using TxSnpArbT = QosRRArb<SnoopFlit, kEntries>;
    using RespArbT = QosRRArb<CMResp, kEntries>;
    MOD(TxSnpArbT, tx_snp_arb);
    MOD(RespArbT, resp_arb);

    using TxSnpInArr = std::array<Valid<SnoopFlit>, kEntries>;
    using RespInArr = std::array<Valid<CMResp>, kEntries>;
    using RdyArrN = std::array<bool, kEntries>;
    WIRE(TxSnpInArr, w_tx_snp_in);
    WIRE(RdyArrN, w_alloc_rdy_all);
    WIRE(RespInArr, w_resp_in);

    SnoopCM();
};

// ---------------- ReadCM ----------------

class ReadCM : public wolvicmod::Module {
public:
    static constexpr uint32_t kEntries = 64;
    static constexpr uint8_t kFree = 0, kCanNest = 1, kSendReq = 2, kWaitData0 = 3,
                             kWaitData1 = 4, kCantNest = 5, kSendAck = 6, kRespCmt = 7;

    struct EntryV {  // 原 ReadEntry::RdReg
        uint8_t state = kFree;
        CMTask task;
        uint32_t taskInst = 0;  // 仅用 resp 字段
        uint8_t respErr = 0;

        bool operator==(const EntryV&) const = default;
    };
    using EntryArr = std::array<EntryV, kEntries>;

    IN(bool, clk);
    IN(uint8_t, cfg_ci);
    IN(Valid<CMTask>, alloc);
    OUT(bool, alloc_rdy);
    OUT(Valid<CMResp>, resp);
    IN(bool, resp_rdy);
    OUT(Valid<HReqFlit>, tx_req);
    IN(bool, tx_req_rdy);
    IN(Valid<DataFlit>, rx_dat);

    REG(EntryArr, entries);
    using AllocT = zj::prefab::Alloc<CMTask, kEntries>;
    MOD(AllocT, alloc_arb);
    using TxReqArbT = QosRRArb<HReqFlit, kEntries>;
    using RespArbT = QosRRArb<CMResp, kEntries>;
    MOD(TxReqArbT, tx_req_arb);
    MOD(RespArbT, resp_arb);

    using TxReqInArr = std::array<Valid<HReqFlit>, kEntries>;
    using RespInArr = std::array<Valid<CMResp>, kEntries>;
    using RdyArrN = std::array<bool, kEntries>;
    WIRE(TxReqInArr, w_tx_req_in);
    WIRE(RdyArrN, w_alloc_rdy_all);
    WIRE(RespInArr, w_resp_in);

    ReadCM();
};

// ---------------- WriteCM ----------------

class WriteCM : public wolvicmod::Module {
public:
    static constexpr uint32_t kEntries = 32;
    static constexpr uint8_t kFree = 0, kCanNest = 1, kSendReq = 2, kWaitDbid = 3,
                             kDataTask = 4, kWaitData = 5, kCantNest = 6, kRespCmt = 7;

    struct EntryV {  // 原 WriteEntry::WrReg
        uint8_t state = kFree;
        bool alrGetComp = false;
        CMTask task;
        uint8_t respErr = 0;

        bool operator==(const EntryV&) const = default;
    };
    using EntryArr = std::array<EntryV, kEntries>;

    IN(bool, clk);
    IN(uint8_t, cfg_ci);
    IN(Valid<CMTask>, alloc);
    OUT(bool, alloc_rdy);
    OUT(Valid<CMResp>, resp);
    IN(bool, resp_rdy);
    OUT(Valid<HReqFlit>, tx_req);
    IN(bool, tx_req_rdy);
    IN(Valid<RespFlit>, rx_rsp);
    OUT(Valid<DataTask>, data_task);
    IN(bool, data_task_rdy);
    IN(Valid<uint8_t>, data_resp);

    REG(EntryArr, entries);
    using AllocT = zj::prefab::Alloc<CMTask, kEntries>;
    MOD(AllocT, alloc_arb);
    using TxReqArbT = QosRRArb<HReqFlit, kEntries>;
    using RespArbT = QosRRArb<CMResp, kEntries>;
    using DataTaskArbT = QosRRArb<DataTask, kEntries>;
    MOD(TxReqArbT, tx_req_arb);
    MOD(RespArbT, resp_arb);
    MOD(DataTaskArbT, data_task_arb);

    using TxReqInArr = std::array<Valid<HReqFlit>, kEntries>;
    using RespInArr = std::array<Valid<CMResp>, kEntries>;
    using RdyArrN = std::array<bool, kEntries>;
    using DataTaskInArr = std::array<Valid<DataTask>, kEntries>;
    WIRE(TxReqInArr, w_tx_req_in);
    WIRE(RdyArrN, w_alloc_rdy_all);
    WIRE(RespInArr, w_resp_in);
    WIRE(DataTaskInArr, w_data_task_in);

    WriteCM();
};

}  // namespace zj::dj
