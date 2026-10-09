#pragma once

// Standalone, typed cycle interface. No gem5, Verilator or DPI dependency.
#include <cstdint>
#include <functional>
#include <memory>

#include "model/bridge/axi_flit.h"
#include "model/flit/xs_flit.h"
#include "model/flit/zj_flit.h"
#include "wolvicmod/prefab/valid.h"

namespace zj {

class WolvicZjModel {
public:
    template <class T> using Valid = wolvicmod::prefab::Valid<T>;

    // Directions match WolvicZjTop: chi_tx goes into the LLC, chi_rx comes out.
    // Default inputs drive no traffic and accept nothing (including cfgAXI).
    struct Inputs {
        Valid<xs::CHIREQ> chi_tx_req{};
        Valid<xs::CHIRSP> chi_tx_rsp{};
        Valid<xs::CHIDAT> chi_tx_dat{};
        bool chi_rx_rsp_rdy = false;
        bool chi_rx_dat_rdy = false;
        bool chi_rx_snp_rdy = false;
        bool mem_aw_rdy = false;
        bool mem_w_rdy = false;
        bool mem_ar_rdy = false;
        Valid<axi::BFlit> mem_b{};
        Valid<axi::RFlit> mem_r{};
        bool cfg_aw_rdy = false;
        bool cfg_w_rdy = false;
        bool cfg_ar_rdy = false;
        Valid<axi::BFlit> cfg_b{};
        Valid<axi::RFlit> cfg_r{};

        bool operator==(const Inputs&) const = default;
    };

    struct Outputs {
        bool chi_tx_req_rdy = false;
        bool chi_tx_rsp_rdy = false;
        bool chi_tx_dat_rdy = false;
        Valid<xs::CHIRSP> chi_rx_rsp{};
        Valid<xs::CHIDAT> chi_rx_dat{};
        Valid<xs::CHISNP> chi_rx_snp{};
        Valid<chi::RReqFlit> cc_tx_req{};  // observation only; never accepted
        Valid<axi::AWFlit> mem_aw{};
        Valid<axi::WFlit> mem_w{};
        Valid<axi::ARFlit> mem_ar{};
        bool mem_b_rdy = false;
        bool mem_r_rdy = false;
        Valid<axi::AWFlit> cfg_aw{};
        Valid<axi::WFlit> cfg_w{};
        Valid<axi::ARFlit> cfg_ar{};
        bool cfg_b_rdy = false;
        bool cfg_r_rdy = false;

        bool operator==(const Outputs&) const = default;
    };

    struct CycleTrace {
        uint64_t cycle;  // zero-based index of the edge just committed
        uint8_t ci;
        Inputs inputs;
        Outputs outputs;       // before edge: use these for valid && ready
        Outputs next_outputs;  // after edge: visible at outputs()
    };
    using TraceSink = std::function<void(const CycleTrace&)>;

    // Construction elaborates and settles at clk=0, with no hidden warmup.
    // Directory startup remains cycle-accurate. ci is fixed for this instance.
    explicit WolvicZjModel(uint8_t ci = 0);
    ~WolvicZjModel();
    WolvicZjModel(const WolvicZjModel&) = delete;
    WolvicZjModel& operator=(const WolvicZjModel&) = delete;
    WolvicZjModel(WolvicZjModel&&) = delete;
    WolvicZjModel& operator=(WolvicZjModel&&) = delete;

    // One call = one rising edge. Returns PRE-edge signals for this call's
    // handshakes; outputs() subsequently exposes POST-edge signals. Hold each
    // input valid/bits until the returned ready accepts it. No implicit queues.
    Outputs eval(const Inputs& inputs);
    const Outputs& outputs() const;
    uint64_t cycles() const;
    uint8_t ci() const;

    // Optional synchronous observer, invoked after each completed cycle.
    // No global files/env vars. Sink must not reenter/mutate this instance;
    // copy the record to retain it. Empty sink disables tracing. If it throws,
    // the cycle is already committed. Calls on one instance must be serialized.
    void setTraceSink(TraceSink sink);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zj
