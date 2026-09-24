// Doc §3.1: combinational logic — 32-bit adder, both registration forms.

#include <cassert>
#include <cstdint>
#include <cstdio>

#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

// Full form: explicit read set + lambda.
struct AdderFull : Module {
    IN(uint32_t, a);
    IN(uint32_t, b);
    OUT(uint32_t, sum);

    AdderFull() {
        sum.assign().reads(a, b) = [](auto src) {
            auto [a, b] = src;
            return a + b;
        };
    }
};

// Fast path: the operator expression registers an equivalent Assign.
struct AdderFast : Module {
    IN(uint32_t, a);
    IN(uint32_t, b);
    OUT(uint32_t, sum);

    AdderFast() { sum = a + b; }
};

int main() {
    AdderFull full;
    full.elaborate();
    AdderFast fast;
    fast.elaborate();

    for (uint32_t x : {0u, 1u, 100u, 0xdeadbeefu}) {
        full.a.set(x);
        full.b.set(x + 7);
        full.eval();
        fast.a.set(x);
        fast.b.set(x + 7);
        fast.eval();
        printf("adder: %u + %u = %u\n", x, x + 7, full.sum.get());
        assert(full.sum.get() == 2 * x + 7);
        assert(fast.sum.get() == full.sum.get());
    }
    return 0;
}
