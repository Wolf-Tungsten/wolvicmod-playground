// DecodeDump 转储器：读 VDecodeDump 宽输出端口，生成
// model/dj/dj_decode_table.inc（constexpr 四级译码表）。
// 用法：dump_decode_table <out.inc>

#include <cstdio>
#include <cstdint>

#include "VDecodeDump.h"
#include "verilated.h"

// 从 VlWide 宽端口按定宽提取第 idx 项（每项 W ≤ 32 bit，元素 0 在最低位）
template <int W, size_t N>
static uint32_t getw(const VlWide<N>& p, uint32_t idx) {
    const uint32_t bit = idx * W;
    const uint32_t word = bit / 32;
    const uint32_t sh = bit % 32;
    uint64_t v = p[word];
    if (sh + W > 32) v |= static_cast<uint64_t>(p[word + 1]) << 32;
    return static_cast<uint32_t>(v >> sh) & (W == 32 ? 0xFFFFFFFFu : ((1u << W) - 1u));
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <out.inc>\n", argv[0]);
        return 2;
    }
    VDecodeDump d;
    d.eval();
    const uint32_t l_ci = d.dims_0, l_si = d.dims_1, l_ti = d.dims_2, l_sti = d.dims_3;
    const uint32_t w_ci = d.dims_4, w_tc = d.dims_5, w_ti = d.dims_6, w_cc = d.dims_7;
    std::fprintf(stderr, "dims: ci=%u si=%u ti=%u sti=%u | widths: chi=%u tc=%u ti=%u cc=%u\n",
                 l_ci, l_si, l_ti, l_sti, w_ci, w_tc, w_ti, w_cc);
    FILE* f = std::fopen(argv[1], "w");
    if (!f) {
        std::perror("fopen");
        return 1;
    }
    std::fprintf(f, "// 由 DecodeDump（dongjiang.frontend.decode.Decode.parse 真实源码）机械\n");
    std::fprintf(f, "// 转储生成，禁止手改。重生成：verify/refgen/dump_decode_table.sh\n");
    std::fprintf(f, "namespace zj::dj::dectab {\n");
    std::fprintf(f, "constexpr uint32_t kLci = %u, kLsi = %u, kLti = %u, kLsti = %u;\n", l_ci,
                 l_si, l_ti, l_sti);
    std::fprintf(f, "constexpr uint32_t kChiW = %u, kTcW = %u, kTiW = %u, kCcW = %u;\n", w_ci,
                 w_tc, w_ti, w_cc);

    std::fprintf(f, "constexpr uint32_t kChi[%u] = {", l_ci);
    for (uint32_t i = 0; i < l_ci; ++i)
        std::fprintf(f, "%s0x%x", i ? "," : "", getw<18>(d.out_chi, i));
    std::fprintf(f, "};\n");

    std::fprintf(f, "constexpr uint8_t kSi[%u][%u] = {", l_ci, l_si);
    for (uint32_t i = 0; i < l_ci; ++i) {
        std::fprintf(f, "%s{", i ? "," : "");
        for (uint32_t j = 0; j < l_si; ++j)
            std::fprintf(f, "%s0x%x", j ? "," : "", getw<5>(d.out_si, i * l_si + j));
        std::fprintf(f, "}");
    }
    std::fprintf(f, "};\n");

    std::fprintf(f, "constexpr uint32_t kTc[%u][%u] = {", l_ci, l_si);
    for (uint32_t i = 0; i < l_ci; ++i) {
        std::fprintf(f, "%s{", i ? "," : "");
        for (uint32_t j = 0; j < l_si; ++j)
            std::fprintf(f, "%s0x%x", j ? "," : "", getw<24>(d.out_tc, i * l_si + j));
        std::fprintf(f, "}");
    }
    std::fprintf(f, "};\n");

    std::fprintf(f, "constexpr uint32_t kTi[%u][%u][%u] = {", l_ci, l_si, l_ti);
    for (uint32_t i = 0; i < l_ci; ++i) {
        std::fprintf(f, "%s{", i ? "," : "");
        for (uint32_t j = 0; j < l_si; ++j) {
            std::fprintf(f, "%s{", j ? "," : "");
            for (uint32_t k = 0; k < l_ti; ++k)
                std::fprintf(f, "%s0x%x", k ? "," : "", getw<19>(d.out_ti, (i * l_si + j) * l_ti + k));
            std::fprintf(f, "}");
        }
        std::fprintf(f, "}");
    }
    std::fprintf(f, "};\n");

    std::fprintf(f, "constexpr uint32_t kSc[%u][%u][%u] = {", l_ci, l_si, l_ti);
    for (uint32_t i = 0; i < l_ci; ++i) {
        std::fprintf(f, "%s{", i ? "," : "");
        for (uint32_t j = 0; j < l_si; ++j) {
            std::fprintf(f, "%s{", j ? "," : "");
            for (uint32_t k = 0; k < l_ti; ++k)
                std::fprintf(f, "%s0x%x", k ? "," : "", getw<24>(d.out_sc, (i * l_si + j) * l_ti + k));
            std::fprintf(f, "}");
        }
        std::fprintf(f, "}");
    }
    std::fprintf(f, "};\n");

    std::fprintf(f, "constexpr uint32_t kSti[%u][%u][%u][%u] = {", l_ci, l_si, l_ti, l_sti);
    for (uint32_t i = 0; i < l_ci; ++i) {
        std::fprintf(f, "%s{", i ? "," : "");
        for (uint32_t j = 0; j < l_si; ++j) {
            std::fprintf(f, "%s{", j ? "," : "");
            for (uint32_t k = 0; k < l_ti; ++k) {
                std::fprintf(f, "%s{", k ? "," : "");
                for (uint32_t l = 0; l < l_sti; ++l)
                    std::fprintf(f, "%s0x%x", l ? "," : "",
                                 getw<19>(d.out_sti, ((i * l_si + j) * l_ti + k) * l_sti + l));
                std::fprintf(f, "}");
            }
            std::fprintf(f, "}");
        }
        std::fprintf(f, "}");
    }
    std::fprintf(f, "};\n");

    std::fprintf(f, "constexpr uint32_t kCc[%u][%u][%u][%u] = {", l_ci, l_si, l_ti, l_sti);
    for (uint32_t i = 0; i < l_ci; ++i) {
        std::fprintf(f, "%s{", i ? "," : "");
        for (uint32_t j = 0; j < l_si; ++j) {
            std::fprintf(f, "%s{", j ? "," : "");
            for (uint32_t k = 0; k < l_ti; ++k) {
                std::fprintf(f, "%s{", k ? "," : "");
                for (uint32_t l = 0; l < l_sti; ++l)
                    std::fprintf(f, "%s0x%x", l ? "," : "",
                                 getw<29>(d.out_cc, ((i * l_si + j) * l_ti + k) * l_sti + l));
                std::fprintf(f, "}");
            }
            std::fprintf(f, "}");
        }
        std::fprintf(f, "}");
    }
    std::fprintf(f, "};\n}  // namespace zj::dj::dectab\n");
    std::fclose(f);
    return 0;
}
