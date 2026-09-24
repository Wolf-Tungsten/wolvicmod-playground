// Doc §3.3: hierarchy and parameterization — an N-beat shift register
// embedded in a sorting pipeline.

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

// Child module: parameterized N-beat shift register.
template <class T, int N>
struct ShiftReg : Module {
    using Stages = std::array<T, N>;  // alias first when T contains commas

    IN(T, din);
    IN(bool, clk);
    OUT(T, dout);
    REG(Stages, q);  // one Reg holds all N stages

    ShiftReg() {
        q.update().on(posedge(clk)).reads(din, q) = [](auto src) {
            auto [d, q] = src;
            Stages next;
            next[0] = d;
            for (int i = 1; i < N; ++i) next[i] = q[i - 1];
            return next;
        };
        dout.assign().reads(q) = [](auto src) {
            auto [q] = src;
            return q[N - 1];  // element access uses the full form
        };
    }
};

// Top level: sort + 3-beat pipeline delay.
struct SortPipeline : Module {
    using Vec = std::vector<int>;
    using SR = ShiftReg<Vec, 3>;

    IN(Vec, din);
    IN(bool, clk);
    OUT(Vec, dout);
    REG(Vec, sorted);  // beat 0: din sorted and registered
    SUB(SR, sr);       // child: 3 more beats

    SortPipeline() {
        sorted.update().on(posedge(clk)).reads(din) = [](auto src) {
            auto [v] = src;
            auto w = v;
            std::sort(w.begin(), w.end());
            return w;
        };

        sr.din = sorted;  // down: parent Assign drives child In
        sr.clk = clk;     // clock distribution likewise

        dout = sr.dout;   // up: parent reads child Out
    }
};

static void clkCycle(SortPipeline& top) {
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();
}

int main() {
    SortPipeline top;
    top.elaborate();

    top.din.set({5, 1, 4, 1, 9});
    const std::vector<int> expect = {1, 1, 4, 5, 9};
    // From sampling to output: 4 beats (§3.3).
    for (int beat = 1; beat <= 4; ++beat) clkCycle(top);

    printf("sort_pipeline: ");
    for (int v : top.dout.get()) printf("%d ", v);
    printf("\n");
    assert(top.dout.get() == expect);
    return 0;
}
