// extract_top_trace.cpp：P4b 全程 trace 提取器（FST 直读，不经 VCD 中间格式）。
//
// 从 emu --dump-wave 的 FST 提取 WolvicZjTop 三边界的逐拍 trace，供
// tests/test_wolvic_top_replay.cpp 重放对拍：
//   L2 CHI 缝：TOP.SimTop.cpu.l_soc.core_with_l2.io_decoupledCHI_{tx_req,tx_rsp,
//     tx_dat,rx_rsp,rx_dat,rx_snp}_{valid,ready,bits_*}     → 列名 l2.<chan>.<field>
//   memAXI：  TOP.SimTop.cpu.l_soc.zhujiang_opt.m_axi_mem_0_{aw,w,b,ar,r}*
//             → 列名 mem.<chan>.<field>（awvalid → mem.aw.valid）
//   cfgAXI：  TOP.SimTop.cpu.l_soc.zhujiang_opt.m_axi_main_* → 列名 cfg.<chan>.<field>
//
// 输出文本格式与 verify/trace/extract_cc_trace.py 相同（首行列名，随后
// cyc + hex 值，>64b 信号拆 _0.._N 低位在前），重放测试的 Trace 读取器复用。
// 复位未撤除的拍跳过；x/z 按 0 处理并计数告警；结束时向 stderr 打 fire 统计。
//
// 用法：extract_top_trace <wave.fst> [-o out.txt] [--start N] [--end M]
//
// 为什么不用 fst2vcd：gtkwave 版 fst2vcd 不支持信号过滤，全程（~32 万拍）
// 全量 VCD 达几十 GB；本提取器经 libfst 的 FacProcessMask 只读目标信号。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "fstapi.h"

namespace {

const char* kL2Prefix = "TOP.SimTop.cpu.l_soc.core_with_l2.io_decoupledCHI_";
const char* kMemPrefix = "TOP.SimTop.cpu.l_soc.zhujiang_opt.m_axi_mem_0_";
const char* kCfgPrefix = "TOP.SimTop.cpu.l_soc.zhujiang_opt.m_axi_main_";
const char* kResetPaths[] = {
    "TOP.SimTop.cpu.l_soc.core_with_l2.reset",
    "TOP.SimTop.cpu.l_soc.zhujiang_opt.reset",
    // 环站 reset（M 节点 resetInject 沿环传播，撤除晚于顶层约 20+ 拍）：
    // trace 起点须等环就绪，否则环端 ready 与模型（全局复位即就绪）不一致
    "TOP.SimTop.cpu.l_soc.zhujiang_opt.ccn_0_0x8.reset",
};

const char* kL2Chans[] = {"tx_req", "tx_rsp", "tx_dat", "rx_rsp", "rx_dat", "rx_snp"};
const char* kAxiChans[] = {"aw", "w", "b", "ar", "r"};
const char* kAxiFields[] = {"valid", "ready", "id",   "addr", "len",  "size", "burst",
                            "lock",  "cache", "prot", "qos",  "data", "strb", "last",
                            "resp"};

// ---- 列注册表 ----
struct ColSet {
    std::vector<std::string> names;                  // 列名（不含 cyc）
    std::unordered_map<std::string, size_t> idx;     // 列名 → 列下标
    // fstHandle → 列组列表（起始列下标, 块数）；块数>1 时 64b 一块、低位在前。
    // Verilator 把恒等信号合并为 alias（共享 handle），故一个 handle 可对应
    // 多个列组（如 m_axi_main_bready / m_axi_mem_0_bready），值变化时同写各组。
    std::unordered_map<fstHandle, std::vector<std::pair<size_t, int>>> handle2col;
    std::vector<uint64_t> vals;                      // 当前各列值
    size_t add(const std::string& name) {
        idx[name] = names.size();
        names.push_back(name);
        return names.size() - 1;
    }
    size_t at(const std::string& name) const {
        auto it = idx.find(name);
        return it == idx.end() ? SIZE_MAX : it->second;
    }
};

ColSet g_cols;
fstHandle g_resetHandles[3] = {0, 0, 0};
int g_resetVals[3] = {1, 1, 1};

bool startsWith(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}
// L2 leaf：`(chan)_(valid|ready|bits_...)`，chan ∈ kL2Chans
bool matchL2Leaf(const char* leaf, std::string& chanOut, std::string& fieldOut) {
    for (const char* ch : kL2Chans) {
        size_t n = std::strlen(ch);
        if (std::strncmp(leaf, ch, n) != 0 || leaf[n] != '_') continue;
        const char* f = leaf + n + 1;
        if (std::strcmp(f, "valid") == 0 || std::strcmp(f, "ready") == 0 ||
            startsWith(f, "bits_")) {
            chanOut = ch;
            fieldOut = f;
            return true;
        }
    }
    return false;
}

// AXI leaf：`(aw|w|b|ar|r)(valid|ready|id|...)`，返回通道与字段
bool matchAxiLeaf(const char* leaf, std::string& chanOut, std::string& fieldOut) {
    for (const char* ch : kAxiChans) {
        size_t n = std::strlen(ch);
        if (std::strncmp(leaf, ch, n) != 0) continue;
        const char* f = leaf + n;
        for (const char* fld : kAxiFields) {
            if (std::strcmp(f, fld) == 0) {
                chanOut = ch;
                fieldOut = fld;
                return true;
            }
        }
    }
    return false;
}

void registerVar(const char* path, fstHandle handle, uint32_t width, int chunkIdx) {
    std::string base;
    std::string chan, field;
    if (startsWith(path, kL2Prefix)) {
        if (!matchL2Leaf(path + std::strlen(kL2Prefix), chan, field)) return;
        base = "l2." + chan + "." + field;
    } else if (startsWith(path, kMemPrefix)) {
        if (!matchAxiLeaf(path + std::strlen(kMemPrefix), chan, field)) return;
        if (field == "region") return;  // ZCI 端口悬空（unused），无对应模型信号
        base = "mem." + chan + "." + field;
    } else if (startsWith(path, kCfgPrefix)) {
        if (!matchAxiLeaf(path + std::strlen(kCfgPrefix), chan, field)) return;
        if (field == "region") return;
        base = "cfg." + chan + "." + field;
    } else {
        return;
    }
    // chunkIdx >= 0：Verilator 64b 拆片（var.name 带 " [msb:lsb]_i" 后缀），
    // 单片即第 i 个列块（低位在前，与 extract_cc_trace.py 的 _0.. 序号一致）；
    // chunkIdx < 0：完整 var，按 width 拆 64b 块。alias 共享 handle 时追加列组。
    int nchunk = chunkIdx >= 0 ? 1 : int((width + 63) / 64);
    size_t first = SIZE_MAX;
    for (int i = 0; i < nchunk; ++i) {
        std::string name = chunkIdx >= 0   ? base + "_" + std::to_string(chunkIdx)
                           : nchunk == 1   ? base
                                           : base + "_" + std::to_string(i);
        size_t c = g_cols.add(name);
        if (first == SIZE_MAX) first = c;
    }
    g_cols.handle2col[handle].push_back({first, nchunk});
}

// ---- 发行（与 extract_cc_trace.py 同语义）----
FILE* g_out = nullptr;
uint64_t g_start = 0, g_end = 0;  // [start, end)
uint64_t g_pending = UINT64_MAX;
uint64_t g_emitted = 0;
uint64_t g_xwarn = 0;
bool g_resetNoted = false;

std::unordered_map<std::string, size_t> g_validIdx, g_readyIdx;
std::unordered_map<std::string, uint64_t> g_fire, g_firstValid;

bool resetActive() {
    for (int i = 0; i < 3; ++i)
        if (g_resetHandles[i] && g_resetVals[i]) return true;
    return false;
}

void emit(uint64_t cyc) {
    std::fprintf(g_out, "%llu", (unsigned long long)cyc);
    for (uint64_t v : g_cols.vals) std::fprintf(g_out, " %llx", (unsigned long long)v);
    std::fputc('\n', g_out);
    ++g_emitted;
    for (auto& [name, i] : g_validIdx) {
        if (!g_cols.vals[i]) continue;
        if (!g_firstValid.count(name)) g_firstValid[name] = cyc;
        auto ri = g_readyIdx.find(name);
        if (ri != g_readyIdx.end() && g_cols.vals[ri->second]) ++g_fire[name];
    }
}

// 时间戳前进到 t：发行 [pending, t) 中落在窗口内且非复位的拍
void advanceTo(uint64_t t) {
    if (g_pending != UINT64_MAX) {
        uint64_t lo = g_pending > g_start ? g_pending : g_start;
        uint64_t hi = t;
        if (g_end && hi > g_end) hi = g_end;
        for (uint64_t c = lo; c < hi; ++c) {
            if (resetActive()) {
                if (!g_resetNoted) {
                    std::fprintf(stderr, "# note: rows skipped while reset active (first at cyc %llu)\n",
                                 (unsigned long long)c);
                    g_resetNoted = true;
                }
                continue;
            }
            emit(c);
        }
    }
    g_pending = t;
}

void applyValue(fstHandle handle, const unsigned char* value) {
    bool isReset = false;
    for (int i = 0; i < 3; ++i)
        if (handle == g_resetHandles[i]) {
            g_resetVals[i] = value[0] == '1' ? 1 : 0;
            isReset = true;
        }
    if (isReset) return;
    auto it = g_cols.handle2col.find(handle);
    if (it == g_cols.handle2col.end()) return;
    for (auto [first, nchunk] : it->second) {
        if (nchunk == 1 && value[1] == '\0') {  // 单 bit 快路
            if (value[0] != '0' && value[0] != '1') ++g_xwarn;
            g_cols.vals[first] = value[0] == '1' ? 1 : 0;
            continue;
        }
        // ASCII 二进制串（MSB 在前）→ 64b 块（低位在前）
        size_t len = std::strlen((const char*)value);
        uint64_t word = 0;
        int nbits = 0, chunk = 0;
        for (size_t i = 0; i < len; ++i) {
            char c = value[len - 1 - i];
            if (c != '0' && c != '1') {
                ++g_xwarn;
                c = '0';
            }
            word |= uint64_t(c == '1') << nbits;
            if (++nbits == 64) {
                g_cols.vals[first + chunk++] = word;
                word = 0;
                nbits = 0;
            }
        }
        if (nbits || chunk == 0) g_cols.vals[first + chunk] = word;
    }
}

void valueChangeCb(void*, uint64_t time, fstHandle facidx, const unsigned char* value) {
    if (g_pending == UINT64_MAX || time > g_pending) advanceTo(time);
    applyValue(facidx, value);
}

}  // namespace

int main(int argc, char** argv) {
    const char* fstPath = nullptr;
    const char* outPath = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) outPath = argv[++i];
        else if (std::strcmp(argv[i], "--start") == 0 && i + 1 < argc) g_start = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--end") == 0 && i + 1 < argc) g_end = std::strtoull(argv[++i], nullptr, 10);
        else if (!fstPath) fstPath = argv[i];
        else {
            std::fprintf(stderr, "usage: extract_top_trace <wave.fst> [-o out.txt] [--start N] [--end M]\n");
            return 1;
        }
    }
    if (!fstPath) {
        std::fprintf(stderr, "usage: extract_top_trace <wave.fst> [-o out.txt] [--start N] [--end M]\n");
        return 1;
    }
    g_out = outPath ? std::fopen(outPath, "w") : stdout;
    if (!g_out) {
        std::perror(outPath);
        return 1;
    }

    fstReaderContext* ctx = fstReaderOpen(fstPath);
    if (!ctx) {
        std::fprintf(stderr, "cannot open %s\n", fstPath);
        return 1;
    }

    // ---- 第一遍：层次遍历，注册目标信号 ----
    std::string path;
    fstReaderClrFacProcessMaskAll(ctx);
    struct fstHier* h;
    while ((h = fstReaderIterateHier(ctx)) != nullptr) {
        if (h->htyp == FST_HT_SCOPE) {
            if (!path.empty()) path.push_back('.');
            path += h->u.scope.name;
        } else if (h->htyp == FST_HT_UPSCOPE) {
            size_t p = path.rfind('.');
            path.resize(p == std::string::npos ? 0 : p);
        } else if (h->htyp == FST_HT_VAR) {
            // var.name 可能带 Verilator 位选后缀："name [msb:lsb]"（完整 var）
            // 或 "name [msb:lsb]_i"（64b 拆片，i 为低位在前的块序号）
            std::string varName = h->u.var.name;
            int chunkIdx = -1;
            auto sp = varName.find(" [");
            if (sp != std::string::npos) {
                auto us = varName.find("]_", sp);
                if (us != std::string::npos) chunkIdx = std::atoi(varName.c_str() + us + 2);
                varName = varName.substr(0, sp);
            }
            size_t saved = path.size();
            if (!path.empty()) path.push_back('.');
            path += varName;
            for (int i = 0; i < 3; ++i)
                if (path == kResetPaths[i] && !g_resetHandles[i]) {
                    // alias（与同值信号共享 handle）也接受：值相同，监视效果一致
                    g_resetHandles[i] = h->u.var.handle;
                    fstReaderSetFacProcessMask(ctx, h->u.var.handle);
                }
            registerVar(path.c_str(), h->u.var.handle, h->u.var.length, chunkIdx);
            if (g_cols.handle2col.count(h->u.var.handle))
                fstReaderSetFacProcessMask(ctx, h->u.var.handle);
            path.resize(saved);
        }
    }
    g_cols.vals.assign(g_cols.names.size(), 0);
    std::fprintf(stderr, "# %zu signal columns, resets watched: %d\n", g_cols.names.size(),
                 (g_resetHandles[0] ? 1 : 0) + (g_resetHandles[1] ? 1 : 0) + (g_resetHandles[2] ? 1 : 0));
    if (g_cols.names.empty()) {
        std::fprintf(stderr, "no boundary signals found — hierarchy path mismatch?\n");
        return 1;
    }
    // 必备通道检查 + fire 统计索引
    auto need = [&](const char* side, const char* const* chans, int n) {
        for (int i = 0; i < n; ++i)
            for (const char* f : {"valid", "ready"}) {
                std::string c = std::string(side) + "." + chans[i] + "." + f;
                if (g_cols.at(c) == SIZE_MAX)
                    std::fprintf(stderr, "# warn: missing column %s\n", c.c_str());
                else if (f[0] == 'v')
                    g_validIdx[std::string(side) + "." + chans[i]] = g_cols.at(c);
                else
                    g_readyIdx[std::string(side) + "." + chans[i]] = g_cols.at(c);
            }
    };
    need("l2", kL2Chans, 6);
    need("mem", kAxiChans, 5);
    need("cfg", kAxiChans, 5);

    // ---- 输出头 ----
    std::fprintf(g_out, "cyc");
    for (auto& n : g_cols.names) std::fprintf(g_out, " %s", n.c_str());
    std::fputc('\n', g_out);

    // ---- 第二遍：值变化遍历 ----
    if (g_start || g_end) fstReaderSetLimitTimeRange(ctx, g_start, g_end ? g_end : ~0ull);
    fstReaderIterBlocks(ctx, valueChangeCb, nullptr, nullptr);
    fstReaderClose(ctx);
    if (g_out != stdout) std::fclose(g_out);

    std::fprintf(stderr, "# emitted %llu rows (start=%llu end=%llu)\n",
                 (unsigned long long)g_emitted, (unsigned long long)g_start,
                 (unsigned long long)g_end);
    if (g_xwarn)
        std::fprintf(stderr, "# warn: %llu x/z values coerced to 0\n", (unsigned long long)g_xwarn);
    std::fprintf(stderr, "# channel fires (valid&&ready) / first valid cyc:\n");
    for (auto& [name, i] : g_validIdx)
        std::fprintf(stderr, "#   %-12s fires=%8llu first_valid=%llu\n", name.c_str(),
                     (unsigned long long)g_fire[name],
                     g_firstValid.count(name) ? (unsigned long long)g_firstValid[name] : 0ull);
    return 0;
}
