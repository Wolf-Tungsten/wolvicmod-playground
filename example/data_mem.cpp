// Doc §3.4: memory — single-write-port data memory with row-level commit.

#include <cassert>
#include <cstdint>
#include <cstdio>

#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

struct DataMem : Module {
    IN(bool, clk);
    IN(bool, wen);
    IN(uint32_t, waddr);
    IN(uint32_t, wdata);
    IN(uint32_t, raddr);
    OUT(uint32_t, rdata);
    MEM(uint32_t, 1024, mem);  // 1024 rows of 32 bits

    DataMem() {
        // Write port: on clk posedge while wen, write wdata into row waddr.
        mem.update().on(posedge(clk)).addr(waddr).en(wen).reads(wdata) = [](auto src) {
            auto [d] = src;
            return d;
        };

        // Read port: combinational read of committed content.
        rdata.assign().reads(mem, raddr) = [](auto src) {
            auto [m, a] = src;
            return m[a];
        };
    }
};

int main() {
    DataMem top;
    top.elaborate();

    // Write 0xc0de into row 7.
    top.wen.set(1);
    top.waddr.set(7);
    top.wdata.set(0xc0de);
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();
    top.wen.set(0);
    top.eval();

    top.raddr.set(7);
    top.eval();
    printf("data_mem: mem[7] = 0x%x\n", top.rdata.get());
    assert(top.rdata.get() == 0xc0de);

    top.raddr.set(8);
    top.eval();
    printf("data_mem: mem[8] = 0x%x (untouched row)\n", top.rdata.get());
    assert(top.rdata.get() == 0);
    return 0;
}
