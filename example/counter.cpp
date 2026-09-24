// Doc §3.2 + §3.5: sequential logic — counter with reset and enable, plus the
// root-module usage flow (construct, elaborate, drive).

#include <cassert>
#include <cstdint>
#include <cstdio>

#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

struct Counter : Module {
    IN(bool, clk);
    IN(bool, rst_n);
    IN(bool, en);
    OUT(uint32_t, dout);
    REG(uint32_t, cnt);

    Counter() {
        // Reset: registered first, highest priority.
        cnt.update().on(negedge(rst_n)) = 0;

        // Count: on clk posedge while en is high.
        cnt.update().on(posedge(clk)).en(en).reads(cnt) = [](auto src) {
            auto [c] = src;
            return c + 1;
        };

        dout = cnt;
    }
};

int main() {
    Counter top;         // construction done: all structure registered (§4.1)
    top.elaborate();     // flatten, structural checks, graph building (§4.2–4.3)

    // Reset: triggered by the falling edge of rst_n.
    top.rst_n.set(1);
    top.eval();
    top.rst_n.set(0);
    top.eval();  // reset takes effect, cnt <- 0
    top.rst_n.set(1);
    top.eval();

    // Clock drive: each toggle corresponds to one eval() (§5.1).
    top.en.set(1);
    for (int cycle = 0; cycle < 10; ++cycle) {
        top.clk.set(0);
        top.eval();
        top.clk.set(1);
        top.eval();  // posedge, cnt increments
        printf("%d: %u\n", cycle, top.dout.get());
        assert(top.dout.get() == static_cast<uint32_t>(cycle) + 1);
    }
    return 0;
}
