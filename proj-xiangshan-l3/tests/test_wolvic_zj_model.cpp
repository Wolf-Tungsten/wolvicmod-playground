// Public C++ interface only: no WolvicZjTop, DPI or Verilator headers.
#include "cpp/wolvic_zj_model.h"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

#include <doctest/doctest.h>

namespace {
using Model = zj::WolvicZjModel;
using namespace zj;

Model::Inputs receiving() {
    Model::Inputs in;
    in.chi_rx_rsp_rdy = true;
    in.chi_rx_dat_rdy = true;
    in.chi_rx_snp_rdy = true;
    in.mem_aw_rdy = true;
    in.mem_w_rdy = true;
    in.mem_ar_rdy = true;
    return in;  // cfg remains blocked
}

void checkNoUnexpectedTraffic(const Model::Outputs& out) {
    CHECK_FALSE(out.cc_tx_req.valid);
    CHECK_FALSE(out.chi_rx_snp.valid);
    CHECK_FALSE(out.cfg_aw.valid);
    CHECK_FALSE(out.cfg_w.valid);
    CHECK_FALSE(out.cfg_ar.valid);
}

void drainRead(Model& model) {
    for (int i = 0; i < 256; ++i) {
        const auto out = model.eval(receiving());
        checkNoUnexpectedTraffic(out);
        CHECK_FALSE(out.chi_rx_rsp.valid);
        CHECK_FALSE(out.chi_rx_dat.valid);
        CHECK_FALSE(out.mem_ar.valid);
        CHECK_FALSE(out.mem_aw.valid);
        CHECK_FALSE(out.mem_w.valid);
    }
}

std::array<uint64_t, 4> dataFor(uint64_t addr, unsigned beat) {
    std::array<uint64_t, 4> data;
    for (unsigned word = 0; word < data.size(); ++word)
        data[word] = 0xDEADBEEF01234567ULL ^ addr ^ (uint64_t(beat) << 40) ^
                     (0x102030405060708ULL * word);
    return data;
}

// Complete a cacheable ReadOnce, including CompAck routed with returned
// HomeNID/DBID. Count only pre-edge valid && ready, never post-edge readiness.
unsigned readLine(Model& model, uint64_t addr, uint16_t txn, bool backpressure) {
    auto in = receiving();
    in.chi_tx_req.valid = true;
    auto& req = in.chi_tx_req.bits;
    req.opcode = 0x03;  // xscache ReadOnce
    req.addr = addr;
    req.src_id = 0x08;
    req.txn_id = txn;
    req.size = 6;
    req.order = 3;
    req.exp_comp_ack = true;
    req.mem_attr_cacheable = true;
    req.mem_attr_allocate = true;
    req.mem_attr_ewa = true;

    unsigned reqs = 0, ars = 0, receipts = 0, rbeats = 0, dbeats = 0, acks = 0;
    unsigned arStalls = 0, rspStalls = 0, datStalls = 0;
    std::optional<axi::ARFlit> heldAr;
    std::optional<xs::CHIRSP> heldRsp;
    std::optional<xs::CHIDAT> heldDat;
    uint8_t rid = 0;
    uint64_t due = 0;
    std::array<bool, 2> seen{};
    std::optional<std::pair<uint16_t, uint16_t>> ackRoute;

    for (unsigned c = 0; c < 22000 && acks == 0; ++c) {
        in.mem_ar_rdy = !backpressure || arStalls >= 7;
        in.chi_rx_rsp_rdy = !backpressure || rspStalls >= 4;
        in.chi_rx_dat_rdy = !backpressure || datStalls >= 9;
        if (ars && rbeats < 2 && model.cycles() >= due) {
            in.mem_r.valid = true;
            in.mem_r.bits.id = rid;
            in.mem_r.bits.data = dataFor(addr, rbeats);
            in.mem_r.bits.last = rbeats == 1;
        }
        const auto preview = model.outputs();
        const auto out = model.eval(in);
        // Registered boundary contract: changing inputs cannot change outputs
        // before the edge. The existing comb audit independently guards this.
        REQUIRE(out == preview);
        checkNoUnexpectedTraffic(out);
        CHECK_FALSE(out.mem_aw.valid);
        CHECK_FALSE(out.mem_w.valid);

        if (heldAr) REQUIRE(out.mem_ar == Model::Valid<axi::ARFlit>{true, *heldAr});
        if (heldRsp) REQUIRE(out.chi_rx_rsp == Model::Valid<xs::CHIRSP>{true, *heldRsp});
        if (heldDat) REQUIRE(out.chi_rx_dat == Model::Valid<xs::CHIDAT>{true, *heldDat});
        heldAr.reset();
        heldRsp.reset();
        heldDat.reset();
        if (in.chi_tx_req.valid && out.chi_tx_req_rdy) {
            ++reqs;
            in.chi_tx_req = {};
        }
        if (in.chi_tx_rsp.valid && out.chi_tx_rsp_rdy) {
            ++acks;
            in.chi_tx_rsp = {};
        }
        if (out.mem_ar.valid) {
            CHECK(out.mem_ar.bits.addr == addr);
            CHECK(out.mem_ar.bits.len == 1);
            CHECK(out.mem_ar.bits.size == 5);
            CHECK(out.mem_ar.bits.burst == 1);
            if (in.mem_ar_rdy) {
                ++ars;
                REQUIRE(ars == 1);
                rid = out.mem_ar.bits.id;
                due = model.cycles() + 11;
            } else {
                ++arStalls;
                heldAr = out.mem_ar.bits;
            }
        }
        if (in.mem_r.valid && out.mem_r_rdy) {
            ++rbeats;
            in.mem_r = {};
            due = model.cycles() + 5;  // non-contiguous AXI beats
        }
        if (out.chi_rx_rsp.valid) {
            CHECK(out.chi_rx_rsp.bits.opcode == 0x08);  // ReadReceipt
            CHECK(out.chi_rx_rsp.bits.txn_id == txn);
            CHECK(out.chi_rx_rsp.bits.resp_err == 0);
            if (in.chi_rx_rsp_rdy) ++receipts;
            else {
                ++rspStalls;
                heldRsp = out.chi_rx_rsp.bits;
            }
        }
        if (out.chi_rx_dat.valid) {
            const auto& dat = out.chi_rx_dat.bits;
            CHECK(dat.opcode == 0x04);  // CompData
            CHECK(dat.txn_id == txn);
            CHECK(dat.tgt_id == 0x08);
            CHECK(dat.resp_err == 0);
            CHECK(dat.be == 0xffffffffULL);
            REQUIRE((dat.data_id == 0 || dat.data_id == 2));
            const unsigned beat = dat.data_id / 2;
            CHECK(dat.data == dataFor(addr, beat));
            if (in.chi_rx_dat_rdy) {
                REQUIRE_FALSE(seen[beat]);
                seen[beat] = true;
                ++dbeats;
                const auto route = std::make_pair(dat.home_nid, dat.dbid);
                if (ackRoute) CHECK(*ackRoute == route);
                ackRoute = route;
                if (dbeats == 2) {
                    in.chi_tx_rsp.valid = true;
                    in.chi_tx_rsp.bits.opcode = 0x02;  // CompAck
                    in.chi_tx_rsp.bits.src_id = 0x08;
                    in.chi_tx_rsp.bits.tgt_id = dat.home_nid;
                    in.chi_tx_rsp.bits.txn_id = dat.dbid;
                }
            } else {
                ++datStalls;
                heldDat = dat;
            }
        }
    }
    REQUIRE(reqs == 1);
    REQUIRE(receipts == 1);
    REQUIRE(dbeats == 2);
    REQUIRE(acks == 1);
    REQUIRE(rbeats == ars * 2);
    if (backpressure) {
        CHECK(arStalls == 7);
        CHECK(rspStalls == 4);
        CHECK(datStalls == 9);
    }
    drainRead(model);
    return ars;
}

TEST_CASE("Direct C++ construction, instance isolation and optional cycle trace") {
    CHECK_THROWS_AS(Model(16), std::invalid_argument);
    Model first;
    Model second(3);
    CHECK(first.ci() == 0);
    CHECK(second.ci() == 3);
    CHECK(first.cycles() == 0);
    const auto initialSecond = second.outputs();
    std::vector<Model::CycleTrace> trace;
    first.setTraceSink([&](const auto& cycle) { trace.push_back(cycle); });
    for (uint64_t c = 0; c < 4; ++c) {
        const auto before = first.outputs();
        const auto sampled = first.eval({});
        REQUIRE(trace.size() == c + 1);
        CHECK(trace.back().cycle == c);
        CHECK(trace.back().ci == 0);
        CHECK(trace.back().inputs == Model::Inputs{});
        CHECK(trace.back().outputs == before);
        CHECK(trace.back().outputs == sampled);
        CHECK(trace.back().next_outputs == first.outputs());
        CHECK(first.cycles() == c + 1);
    }
    CHECK(second.cycles() == 0);
    CHECK(second.outputs() == initialSecond);
    first.setTraceSink({});
    first.eval({});
    CHECK(trace.size() == 4);
    CHECK(first.cycles() == 5);
    second.eval({});
    CHECK(second.cycles() == 1);
}

TEST_CASE("Direct C++ read miss, delayed AXI return, CompData and CompAck in both LLC banks") {
    Model model;
    // addr[12] selects LLC bank; repeat each address to prove allocation and
    // transaction retirement (a hit must complete without another memory read).
    for (uint64_t addr : {0x80000000ULL, 0x80001040ULL}) {
        CAPTURE(addr);
        CHECK(readLine(model, addr, 0x5a, true) == 1);
        CHECK(readLine(model, addr, 0x6b, false) == 0);
    }
}

TEST_CASE("AXI read IDs may return out of issue order") {
    Model model;
    auto in = receiving();
    const std::array<uint64_t, 2> addrs = {0x80000000ULL, 0x80001040ULL};
    const std::array<uint16_t, 2> txns = {0x121, 0x232};
    unsigned nextReq = 0;
    unsigned reqFires = 0;
    unsigned dataBeats[2] = {};
    unsigned receipts[2] = {};
    unsigned acks = 0;
    std::map<uint8_t, unsigned> idToReq;
    std::map<uint8_t, unsigned> beatById;
    std::deque<uint8_t> responseOrder;
    std::vector<uint8_t> issuedIds;
    std::array<std::array<bool, 2>, 2> seen{};
    bool responseOrderStarted = false;

    for (unsigned cycle = 0; cycle < 50000 && acks != 2; ++cycle) {
        in.chi_tx_req = {};
        if (nextReq < addrs.size()) {
            in.chi_tx_req.valid = true;
            auto &req = in.chi_tx_req.bits;
            req.opcode = 0x03;  // ReadOnce
            req.addr = addrs[nextReq];
            req.src_id = 0x08;
            req.txn_id = txns[nextReq];
            req.size = 6;
            req.order = 3;
            req.exp_comp_ack = true;
            req.mem_attr_cacheable = true;
            req.mem_attr_allocate = true;
        }
        // Once both ARs have been accepted, deliberately service the second
        // ID first. The bridge must route each R beat by ID rather than by
        // address issue order.
        in.mem_r = {};
        if (!responseOrderStarted && idToReq.size() == 2) {
            for (auto it = issuedIds.rbegin(); it != issuedIds.rend(); ++it)
                responseOrder.push_back(*it);
            responseOrderStarted = true;
        }
        if (!responseOrder.empty()) {
            const uint8_t id = responseOrder.front();
            in.mem_r.valid = true;
            in.mem_r.bits.id = id;
            in.mem_r.bits.last = beatById[id] == 1;
            in.mem_r.bits.data = dataFor(addrs[idToReq[id]], beatById[id]);
        }
        const auto out = model.eval(in);

        if (in.chi_tx_req.valid && out.chi_tx_req_rdy) {
            ++reqFires;
            ++nextReq;
        }
        if (in.chi_tx_rsp.valid && out.chi_tx_rsp_rdy) {
            ++acks;
            in.chi_tx_rsp = {};
        }
        checkNoUnexpectedTraffic(out);
        if (out.mem_ar.valid && in.mem_ar_rdy) {
            const auto it = std::find(addrs.begin(), addrs.end(), out.mem_ar.bits.addr);
            REQUIRE(it != addrs.end());
            REQUIRE(idToReq.emplace(out.mem_ar.bits.id, std::distance(addrs.begin(), it)).second);
            issuedIds.push_back(out.mem_ar.bits.id);
        }
        if (in.mem_r.valid && out.mem_r_rdy) {
            if (in.mem_r.bits.last)
                responseOrder.pop_front();
            else
                ++beatById[in.mem_r.bits.id];
        }
        if (out.chi_rx_rsp.valid) {
            const auto op = out.chi_rx_rsp.bits.opcode;
            if (op == 0x08) {
                const auto it = std::find(txns.begin(), txns.end(),
                                          out.chi_rx_rsp.bits.txn_id);
                REQUIRE(it != txns.end());
                ++receipts[std::distance(txns.begin(), it)];
            }
        }
        if (out.chi_rx_dat.valid) {
            const auto it = std::find(txns.begin(), txns.end(),
                                      out.chi_rx_dat.bits.txn_id);
            REQUIRE(it != txns.end());
            const unsigned req = std::distance(txns.begin(), it);
            const auto &d = out.chi_rx_dat.bits;
            REQUIRE((d.data_id == 0 || d.data_id == 2));
            REQUIRE_FALSE(seen[req][d.data_id / 2]);
            seen[req][d.data_id / 2] = true;
            CHECK(d.data == dataFor(addrs[req], d.data_id / 2));
            CHECK(d.resp_err == 0);
            ++dataBeats[req];
            if (dataBeats[req] == 2) {
                in.chi_tx_rsp.valid = true;
                in.chi_tx_rsp.bits.opcode = 0x02;  // CompAck
                in.chi_tx_rsp.bits.src_id = 0x08;
                in.chi_tx_rsp.bits.tgt_id = out.chi_rx_dat.bits.home_nid;
                in.chi_tx_rsp.bits.txn_id = out.chi_rx_dat.bits.dbid;
            }
        }
    }
    CHECK(reqFires == 2);
    CHECK(idToReq.size() == 2);
    CHECK(responseOrder.empty());
    CHECK(receipts[0] == 1);
    CHECK(receipts[1] == 1);
    CHECK(dataBeats[0] == 2);
    CHECK(dataBeats[1] == 2);
    CHECK(acks == 2);
    drainRead(model);
}

TEST_CASE("Direct C++ CHI write data and cfgAXI B return complete a device write") {
    Model model;
    auto in = receiving();
    in.cfg_aw_rdy = true;
    in.cfg_w_rdy = true;
    in.chi_tx_req.valid = true;
    auto& req = in.chi_tx_req.bits;
    req.opcode = 0x1c;  // WriteNoSnpPtl, ewa=0: Comp waits for AXI B
    req.addr = 0x38000048;
    req.size = 3;
    req.src_id = 0x08;
    req.txn_id = 0x7b;
    req.mem_attr_device = true;
    unsigned reqs = 0, dbids = 0, data = 0, aws = 0, ws = 0, bs = 0, comps = 0;
    uint8_t bid = 0;
    constexpr uint64_t payload = 0x1234567890abcdefULL;

    for (unsigned c = 0; c < 1000 && comps == 0; ++c) {
        const auto out = model.eval(in);
        CHECK_FALSE(out.mem_ar.valid);
        CHECK_FALSE(out.mem_aw.valid);
        CHECK_FALSE(out.mem_w.valid);
        CHECK_FALSE(out.cc_tx_req.valid);
        CHECK_FALSE(out.chi_rx_snp.valid);
        CHECK_FALSE(out.chi_rx_dat.valid);
        CHECK_FALSE(out.cfg_ar.valid);
        if (in.chi_tx_req.valid && out.chi_tx_req_rdy) {
            ++reqs;
            in.chi_tx_req = {};
        }
        if (in.chi_tx_dat.valid && out.chi_tx_dat_rdy) {
            ++data;
            in.chi_tx_dat = {};
        }
        if (in.cfg_b.valid && out.cfg_b_rdy) {
            ++bs;
            in.cfg_b = {};
        }
        if (out.chi_rx_rsp.valid) {
            const auto& rsp = out.chi_rx_rsp.bits;
            CHECK(rsp.txn_id == 0x7b);
            CHECK(rsp.resp_err == 0);
            if (rsp.opcode == 0x06) {  // DBIDResp
                ++dbids;
                in.chi_tx_dat.valid = true;
                auto& dat = in.chi_tx_dat.bits;
                dat.opcode = 0x03;  // NonCopyBackWrData
                dat.src_id = 0x08;
                dat.tgt_id = rsp.src_id;
                dat.txn_id = rsp.dbid;
                dat.data_id = 1;
                dat.be = 0x0000ff00;
                dat.data[1] = payload;
            } else {
                CHECK(rsp.opcode == 0x04);  // Comp
                CHECK(bs == 1);
                ++comps;
            }
        }
        if (out.cfg_aw.valid) {
            ++aws;
            CHECK(out.cfg_aw.bits.addr == 0x38000048);
            CHECK(out.cfg_aw.bits.size == 3);
            CHECK(out.cfg_aw.bits.len == 0);
            bid = out.cfg_aw.bits.id;
        }
        if (out.cfg_w.valid) {
            ++ws;
            CHECK(out.cfg_w.bits.last);
            CHECK(out.cfg_w.bits.strb == 0x0000ff00);
            for (auto word : out.cfg_w.bits.data) CHECK(word == payload);
            REQUIRE(aws == 1);
            in.cfg_b = {true, {bid, 0}};
        }
    }
    CHECK(reqs == 1);
    CHECK(dbids == 1);
    CHECK(data == 1);
    CHECK(aws == 1);
    CHECK(ws == 1);
    CHECK(bs == 1);
    CHECK(comps == 1);
    drainRead(model);
}
}  // namespace
