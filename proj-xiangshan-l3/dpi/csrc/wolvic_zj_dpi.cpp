// wolvic_zj_dpi.cpp：WolvicZjTop 模型的 DPI-C glue（WolvicZjBB.sv 的 C++ 对侧）。
//
// 时序（与 SV 壳注释一致，单调用方案）：
//   wolvic_zj_step：set 输入（边沿前采样）→ clk 0→1 eval 提交 → 读输出打包 →
//     clk 拉回 0。模型边界零组合穿透（tests/test_comb_audit 常驻审计），故输出
//     可在提交后立即读取，等效 RTL 的全寄存边界。
//   wolvic_zj_peek：不推进时钟，仅取当前（初态）输出，SV initial 用。
//   模型构造态 = 复位完成态（P4b 对齐语义）；SV 侧 reset 期间不调用 step。
//
// 调试：WOLVIC_ZJ_TRACE=<path> 每拍落一行 <cyc> in <hex> out <hex>；
// WOLVIC_ZJ_TRACE_MAX=<n> 截断。Verilator --threads-dpi all 下单调用点串行
// 执行，互斥锁为防御性兜底。

#include "wolvic_zj_pack.h"

namespace {

uint64_t g_cyc = 0;
FILE* g_trace = nullptr;
uint64_t g_trace_max = ~uint64_t{0};

void initTraceOnce() {
    static bool done = [] {
        if (const char* p = std::getenv("WOLVIC_ZJ_TRACE")) {
            g_trace = std::fopen(p, "w");
            if (!g_trace) std::fprintf(stderr, "[wolvic-zj] cannot open trace %s\n", p);
        }
        if (const char* p = std::getenv("WOLVIC_ZJ_TRACE_MAX"))
            g_trace_max = std::strtoull(p, nullptr, 0);
        return true;
    }();
    (void)done;
}

void traceStep(const wzj::InPack& in, const wzj::OutPack& out) {
    if (!g_trace || g_cyc >= g_trace_max) return;
    std::fprintf(g_trace, "%llu in ", (unsigned long long)g_cyc);
    wzj::printHex(g_trace, in.data(), wzj::kInW);
    std::fprintf(g_trace, " out ");
    wzj::printHex(g_trace, out.data(), wzj::kOutW);
    std::fprintf(g_trace, "\n");
    std::fflush(g_trace);  // 冒烟测试台同步读取；仅 trace 开启时付出
}

}  // namespace

extern "C" void wolvic_zj_step(const svBitVecVal* in_pack, svBitVecVal* out_pack) {
    std::lock_guard<std::mutex> lk(wzj::mtx());
    initTraceOnce();
    zj::WolvicZjTop& m = wzj::model();
    const wzj::InPack in = wzj::fromSv<std::tuple_size<wzj::InPack>::value>(in_pack, wzj::kInW);
    wzj::unpackInputs(m, in);
    m.clk.set(1);
    m.eval();  // posedge：采样边沿前输入，提交状态，组合稳态
    const wzj::OutPack out = wzj::packOutputs(m);
    wzj::toSv(out_pack, out, wzj::kOutW);
    traceStep(in, out);
    ++g_cyc;
    m.clk.set(0);
    m.eval();  // 回到低相（无触发，快速返回）
}

extern "C" void wolvic_zj_peek(svBitVecVal* out_pack) {
    std::lock_guard<std::mutex> lk(wzj::mtx());
    zj::WolvicZjTop& m = wzj::model();
    m.eval();
    wzj::toSv(out_pack, wzj::packOutputs(m), wzj::kOutW);
}
