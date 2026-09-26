// 译码库锚点单测：从 Scala 表（frontend/decode/*Table.scala）手工推导若干
// ChiInst/StateInst/TaskInst 位图，验证 dj_decode_table.inc 转储与
// dj_decode.h 的匹配/查码算法。（Backend 全量行为由 run.sh backend 对拍覆盖。）

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

#include "model/dj/dj_decode.h"

using namespace zj::dj::dectab;

namespace {

// ChiInst 打包（先声明在 MSB）
constexpr uint32_t packChi(bool valid, uint32_t channel, bool fromLAN, bool toLAN,
                           uint32_t opcode, bool eca, bool alloc, bool ewa, uint32_t order,
                           bool full) {
    return (valid << 17) | (channel << 15) | (fromLAN << 14) | (toLAN << 13) |
           (opcode << 6) | (eca << 5) | (alloc << 4) | (ewa << 3) | (order << 1) | full;
}
constexpr uint32_t packSi(bool valid, bool src, bool oth, uint32_t llc) {
    return (valid << 4) | (src << 3) | (oth << 2) | llc;
}
constexpr uint32_t packTi(bool valid, bool fwd, uint32_t ch, uint32_t op, uint32_t resp,
                          uint32_t fwdResp, bool getX, uint32_t xcb) {
    return (valid << 18) | (fwd << 17) | (ch << 15) | (op << 10) | (resp << 7) |
           (fwdResp << 4) | (getX << 3) | xcb;
}

TEST_CASE("dj decode: ChiInst 匹配（readNoSnp 三态与 readOnce 家族）") {
    // readNoSnp_noExpCompAck_EO（表 ci=0）
    CHECK(decChi(packChi(1, 0, 1, 1, 4, 0, 0, 0, 3, 0)) == 0);
    // readNoSnp_expCompAck_EO（ci=1）
    CHECK(decChi(packChi(1, 0, 1, 1, 4, 1, 0, 0, 3, 0)) == 1);
    // readNoSnp_expCompAck_EO_ewa（ci=2）
    CHECK(decChi(packChi(1, 0, 1, 1, 4, 1, 0, 1, 3, 0)) == 2);
    // readOnce_noAllocate（ci=3）：opcode=3(ReadOnce)，EO，expCompAck
    CHECK(decChi(packChi(1, 0, 1, 1, 3, 1, 0, 0, 3, 0)) == 3);
    // readOnce_allocate_ewa_fullSize（ci=10）
    CHECK(decChi(packChi(1, 0, 1, 1, 3, 1, 1, 1, 3, 1)) == 10);
    // writeEvictOrEvict（Write 表内）：opcode=0x42，eca+allocate+ewa+noOrder+fullSize
    // 索引 = 14(Read) + 5(Dataless) + 11(Write 前 11 项) = 30
    const uint32_t ci_ev = decChi(packChi(1, 0, 1, 1, 0x42, 1, 1, 1, 0, 1));
    CHECK(ci_ev == 30);
}

TEST_CASE("dj decode: StateInst/TaskInst 匹配与查码") {
    // ci=0 仅一行 state：sfMiss|llcIs(I) = valid,src=0,oth=0,llc=I(0)
    CHECK(decState(0, packSi(1, 0, 0, 0)) == 0);
    // ci=3(readOnce_noAllocate)：行 0..4 = sfMiss|llc(I/SC/UC/UD)、srcMiss|othHit|llc(I)
    CHECK(decState(3, packSi(1, 0, 0, 0)) == 0);
    CHECK(decState(3, packSi(1, 0, 0, 1)) == 1);
    CHECK(decState(3, packSi(1, 0, 0, 2)) == 3);
    CHECK(decState(3, packSi(1, 0, 1, 0)) == 4);
    // (3,4) 行 TaskInst[0] = rspIs(SnpRespFwded=9)|respIs(UC=2)|fwdIs(I=0)
    CHECK(decTask(3, 4, packTi(1, 1, 2, 9, 2, 0, 0, 0)) == 0);
    // (3,4) 行 TaskInst[4] = datIs(SnpRespDataFwded=6)|respIs(SC_PD=5)|fwdIs(I)
    CHECK(decTask(3, 4, packTi(1, 1, 1, 6, 5, 0, 0, 0)) == 4);
}

TEST_CASE("dj decode: TaskCode/CommitCode 查码") {
    // (0,0)：readNoSnp_noExpCompAck_EO 的 first(read(ReadNoSnp)|needDB, ...)
    const uint32_t tc = getTaskCode(0, 0);
    CHECK(tcRead(tc) == 1);
    CHECK(tcOpcode(tc) == 4);
    CHECK(tcNeedDB(tc) == 1);
    // (0,0,0,0) commit：cdop("send") | cmtDat(CompData=4) | respIs(I=0)
    const uint32_t cc = getCommitCode(0, 0, 0, 0);
    CHECK(ccOpSend(cc) == 1);
    CHECK(ccChannel(cc) == 1);  // DAT
    CHECK(ccOpcode(cc) == 4);
    CHECK(ccResp(cc) == 0);
    // (3,0) readOnce_noAllocate 的 sfMiss|llc(I)：first(read(ReadNoSnp)|doDMT, noCmt)
    const uint32_t tc2 = getTaskCode(3, 0);
    CHECK(tcRead(tc2) == 1);
    CHECK(tcDoDMT(tc2) == 1);
    const uint32_t cc2 = getCommitCode(3, 0, 0, 0);
    CHECK(cc2 == 0);  // noCmt
}

}  // namespace
