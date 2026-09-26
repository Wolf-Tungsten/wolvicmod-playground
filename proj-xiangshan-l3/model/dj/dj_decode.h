#pragma once

// DongJiang 译码库：对齐 frontend/decode/Bundle.scala（Decode 对象）+
// backend/Decode.scala（Third/Fourth + GetDecRes）。
// 四级内容寻址表由 verify/refgen/dump_decode_table.cpp 从真实源码机械转储
// （dj_decode_table.inc，禁止手改）；本文件实现匹配与查码算法。
//
// Bundle 位序规则（经 flit pack 与 DecodeDump 双重锚定）：
//   先声明字段在 MSB；trait 链按声明次序（先挂者更显著）；嵌套 Bundle 字段连续。

#include <cstdint>

#include "model/dj/dj_decode_table.inc"

namespace zj::dj {

namespace dectab {

// ---------------- 指令/码位段（MSB 先排） ----------------

// ChiInst(18)
constexpr uint32_t chiValid(uint32_t v) { return (v >> 17) & 1; }
constexpr uint32_t chiChannel(uint32_t v) { return (v >> 15) & 3; }
constexpr uint32_t chiOpcode(uint32_t v) { return (v >> 6) & 0x7F; }
constexpr uint32_t chiExpCompAck(uint32_t v) { return (v >> 5) & 1; }
constexpr uint32_t chiAllocate(uint32_t v) { return (v >> 4) & 1; }
constexpr uint32_t chiEwa(uint32_t v) { return (v >> 3) & 1; }
constexpr uint32_t chiOrder(uint32_t v) { return (v >> 1) & 3; }
constexpr uint32_t chiFullSize(uint32_t v) { return v & 1; }

// StateInst(5)
constexpr uint32_t siValid(uint32_t v) { return (v >> 4) & 1; }
constexpr uint32_t siSrcHit(uint32_t v) { return (v >> 3) & 1; }
constexpr uint32_t siOthHit(uint32_t v) { return (v >> 2) & 1; }
constexpr uint32_t siLlcState(uint32_t v) { return v & 3; }

// TaskInst(19)
constexpr uint32_t tiValid(uint32_t v) { return (v >> 18) & 1; }
constexpr uint32_t tiFwdValid(uint32_t v) { return (v >> 17) & 1; }
constexpr uint32_t tiChannel(uint32_t v) { return (v >> 15) & 3; }
constexpr uint32_t tiOpcode(uint32_t v) { return (v >> 10) & 0x1F; }
constexpr uint32_t tiResp(uint32_t v) { return (v >> 7) & 7; }
constexpr uint32_t tiFwdResp(uint32_t v) { return (v >> 4) & 7; }
constexpr uint32_t tiGetXCBResp(uint32_t v) { return (v >> 3) & 1; }
constexpr uint32_t tiXCBResp(uint32_t v) { return v & 7; }

// TaskCode(24)：ops(4) + dataOp(5) + opcode(7) + 6 标志 + snpTgt(2) + fullSize
constexpr uint32_t tcSnoop(uint32_t v) { return (v >> 23) & 1; }
constexpr uint32_t tcRead(uint32_t v) { return (v >> 22) & 1; }
constexpr uint32_t tcDataless(uint32_t v) { return (v >> 21) & 1; }
constexpr uint32_t tcWrite(uint32_t v) { return (v >> 20) & 1; }
constexpr uint32_t tcOpRepl(uint32_t v) { return (v >> 19) & 1; }
constexpr uint32_t tcOpRead(uint32_t v) { return (v >> 18) & 1; }
constexpr uint32_t tcOpSend(uint32_t v) { return (v >> 17) & 1; }
constexpr uint32_t tcOpSave(uint32_t v) { return (v >> 16) & 1; }
constexpr uint32_t tcOpMerge(uint32_t v) { return (v >> 15) & 1; }
constexpr uint32_t tcOpcode(uint32_t v) { return (v >> 8) & 0x7F; }
constexpr uint32_t tcNeedDB(uint32_t v) { return (v >> 7) & 1; }
constexpr uint32_t tcReturnDBID(uint32_t v) { return (v >> 6) & 1; }
constexpr uint32_t tcExpCompAck(uint32_t v) { return (v >> 5) & 1; }
constexpr uint32_t tcDoDMT(uint32_t v) { return (v >> 4) & 1; }
constexpr uint32_t tcRetToSrc(uint32_t v) { return (v >> 3) & 1; }
constexpr uint32_t tcSnpTgt(uint32_t v) { return (v >> 1) & 3; }
constexpr uint32_t tcFullSize(uint32_t v) { return v & 1; }
// TaskCode.isValid = opsIsValid | returnDBID
constexpr bool tcIsValid(uint32_t v) {
    return tcSnoop(v) || tcRead(v) || tcDataless(v) || tcWrite(v) || tcReturnDBID(v);
}

// CommitCode(29)：wri(7) + dataOp(5) + waitSecDone/sendResp/sendfwdResp + channel(2)
//                  + opcode(5) + resp(3) + fwdResp(3) + fullSize
constexpr uint32_t ccWriSRC(uint32_t v) { return (v >> 28) & 1; }
constexpr uint32_t ccWriSNP(uint32_t v) { return (v >> 27) & 1; }
constexpr uint32_t ccWriLLC(uint32_t v) { return (v >> 26) & 1; }
constexpr uint32_t ccSrcValid(uint32_t v) { return (v >> 25) & 1; }
constexpr uint32_t ccSnpValid(uint32_t v) { return (v >> 24) & 1; }
constexpr uint32_t ccLlcState(uint32_t v) { return (v >> 22) & 3; }
constexpr uint32_t ccOpRepl(uint32_t v) { return (v >> 21) & 1; }
constexpr uint32_t ccOpRead(uint32_t v) { return (v >> 20) & 1; }
constexpr uint32_t ccOpSend(uint32_t v) { return (v >> 19) & 1; }
constexpr uint32_t ccOpSave(uint32_t v) { return (v >> 18) & 1; }
constexpr uint32_t ccOpMerge(uint32_t v) { return (v >> 17) & 1; }
constexpr uint32_t ccWaitSecDone(uint32_t v) { return (v >> 16) & 1; }
constexpr uint32_t ccSendResp(uint32_t v) { return (v >> 15) & 1; }
constexpr uint32_t ccSendFwdResp(uint32_t v) { return (v >> 14) & 1; }
constexpr uint32_t ccChannel(uint32_t v) { return (v >> 12) & 3; }
constexpr uint32_t ccOpcode(uint32_t v) { return (v >> 7) & 0x1F; }
constexpr uint32_t ccResp(uint32_t v) { return (v >> 4) & 7; }
constexpr uint32_t ccFwdResp(uint32_t v) { return (v >> 1) & 7; }
constexpr uint32_t ccFullSize(uint32_t v) { return v & 1; }

// ---------------- 内容寻址译码（PriorityEncoder；无匹配 → 0） ----------------

// decode("chi")：ChiInst 全等匹配（断言唯一）
inline uint32_t decChi(uint32_t inst) {
    for (uint32_t i = 0; i < kLci; ++i)
        if (kChi[i] == inst) return i;
    return 0;
}
// decode("state")：在 ci 行内 StateInst 全等匹配
inline uint32_t decState(uint32_t ci, uint32_t inst) {
    for (uint32_t j = 0; j < kLsi; ++j)
        if (kSi[ci][j] == inst) return j;
    return 0;
}
// decode("task")：在 (ci, si) 行内 TaskInst 全等匹配
inline uint32_t decTask(uint32_t ci, uint32_t si, uint32_t inst) {
    for (uint32_t k = 0; k < kLti; ++k)
        if (kTi[ci][si][k] == inst) return k;
    return 0;
}
// decode("secTask")：在 (ci, si, ti) 行内 SecTaskInst 全等匹配
inline uint32_t decSec(uint32_t ci, uint32_t si, uint32_t ti, uint32_t inst) {
    for (uint32_t l = 0; l < kLsti; ++l)
        if (kSti[ci][si][ti][l] == inst) return l;
    return 0;
}

// ---------------- GetDecRes：按 decList 索引查码 ----------------

inline uint32_t getTaskCode(uint32_t ci, uint32_t si) { return kTc[ci][si]; }
inline uint32_t getSecTaskCode(uint32_t ci, uint32_t si, uint32_t ti) { return kSc[ci][si][ti]; }
inline uint32_t getCommitCode(uint32_t ci, uint32_t si, uint32_t ti, uint32_t sti) {
    return kCc[ci][si][ti][sti];
}

}  // namespace dectab

}  // namespace zj::dj
