#include "cpp/wolvic_zj_model.h"

#include <stdexcept>
#include <utility>

#include "model/wolvic_zj_top.h"
#include "wolvicmod/wolvicmod.h"

namespace zj {

struct WolvicZjModel::Impl {
    WolvicZjTop top;
    const uint8_t ci;
    uint64_t cycles = 0;
    Outputs outputs;
    TraceSink trace;

    explicit Impl(uint8_t cluster) : ci(cluster) {
        top.elaborate();
        top.clk.set(false);
        top.ci.set(ci);
        drive(Inputs{});
        top.eval();
        outputs = sample();
    }

    void drive(const Inputs& in) {
        top.chi_tx_req.set(in.chi_tx_req);
        top.chi_tx_rsp.set(in.chi_tx_rsp);
        top.chi_tx_dat.set(in.chi_tx_dat);
        top.chi_rx_rsp_rdy.set(in.chi_rx_rsp_rdy);
        top.chi_rx_dat_rdy.set(in.chi_rx_dat_rdy);
        top.chi_rx_snp_rdy.set(in.chi_rx_snp_rdy);
        top.mem_aw_rdy.set(in.mem_aw_rdy);
        top.mem_w_rdy.set(in.mem_w_rdy);
        top.mem_ar_rdy.set(in.mem_ar_rdy);
        top.mem_b.set(in.mem_b);
        top.mem_r.set(in.mem_r);
        top.cfg_aw_rdy.set(in.cfg_aw_rdy);
        top.cfg_w_rdy.set(in.cfg_w_rdy);
        top.cfg_ar_rdy.set(in.cfg_ar_rdy);
        top.cfg_b.set(in.cfg_b);
        top.cfg_r.set(in.cfg_r);
    }

    Outputs sample() const {
        Outputs out;
        out.chi_tx_req_rdy = top.chi_tx_req_rdy.get();
        out.chi_tx_rsp_rdy = top.chi_tx_rsp_rdy.get();
        out.chi_tx_dat_rdy = top.chi_tx_dat_rdy.get();
        out.chi_rx_rsp = top.chi_rx_rsp.get();
        out.chi_rx_dat = top.chi_rx_dat.get();
        out.chi_rx_snp = top.chi_rx_snp.get();
        out.cc_tx_req = top.cc_tx_req.get();
        out.mem_aw = top.mem_aw.get();
        out.mem_w = top.mem_w.get();
        out.mem_ar = top.mem_ar.get();
        out.mem_b_rdy = top.mem_b_rdy.get();
        out.mem_r_rdy = top.mem_r_rdy.get();
        out.cfg_aw = top.cfg_aw.get();
        out.cfg_w = top.cfg_w.get();
        out.cfg_ar = top.cfg_ar.get();
        out.cfg_b_rdy = top.cfg_b_rdy.get();
        out.cfg_r_rdy = top.cfg_r_rdy.get();
        return out;
    }
};

WolvicZjModel::WolvicZjModel(uint8_t ci) {
    if (ci > 0xf) throw std::invalid_argument("WolvicZjModel: ci must fit in 4 bits");
    impl_ = std::make_unique<Impl>(ci);
}

WolvicZjModel::~WolvicZjModel() = default;

WolvicZjModel::Outputs WolvicZjModel::eval(const Inputs& inputs) {
    auto& m = *impl_;
    m.drive(inputs);
    m.top.eval();  // settle input-dependent guards before pulsing clk
    const auto before = m.sample();
    m.top.clk.set(true);
    m.top.eval();  // exactly one state-advancing edge
    m.outputs = m.sample();
    m.top.clk.set(false);
    m.top.eval();  // rearm posedge; no state advance
    const auto cycle = m.cycles++;
    if (m.trace) m.trace(CycleTrace{cycle, m.ci, inputs, before, m.outputs});
    return before;
}

const WolvicZjModel::Outputs& WolvicZjModel::outputs() const { return impl_->outputs; }
uint64_t WolvicZjModel::cycles() const { return impl_->cycles; }
uint8_t WolvicZjModel::ci() const { return impl_->ci; }
void WolvicZjModel::setTraceSink(TraceSink sink) { impl_->trace = std::move(sink); }

}  // namespace zj
