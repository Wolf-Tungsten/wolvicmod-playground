#pragma once

// trace_io.h：emu 边界 trace（cm_full.txt 格式）读取器，供 zjrtl 下各回放
// harness 共享（此前在 test_wolvic_top_replay.cpp / rtl_replay.cpp 各有一份
// 拷贝）。
//
// 格式：首行表头 "cyc <col0> <col1> ..."（cyc 列十进制，其余列十六进制），
// '#' 开头为注释行。列宽 >64b 的字段按 *_0..*_3 拆成 64b 字列。

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace zjrtl {

struct Trace {
    std::vector<std::string>                cols;
    std::unordered_map<std::string, size_t> idx;
    std::vector<std::vector<uint64_t>>      rows;
    std::vector<uint64_t>                   cycs;

    bool load(const char* path) {
        FILE* f = std::fopen(path, "r");
        if (!f) return false;
        std::string line;
        line.reserve(8192);
        int ch;
        auto readLine = [&](std::string& out) -> bool {
            out.clear();
            while ((ch = std::fgetc(f)) != EOF) {
                if (ch == '\n') return true;
                out.push_back(char(ch));
            }
            return !out.empty();
        };
        bool header = false;
        while (readLine(line)) {
            if (line.empty() || line[0] == '#') continue;
            std::vector<uint64_t> vals;
            std::vector<std::string> names;
            const char* p = line.c_str();
            while (*p) {
                while (*p == ' ' || *p == '\t') ++p;
                if (!*p) break;
                const char* tok = p;
                while (*p && *p != ' ' && *p != '\t') ++p;
                if (!header) {
                    names.emplace_back(tok, p - tok);
                } else {
                    uint64_t v = 0;
                    if (vals.empty()) {  // cyc 列：十进制
                        for (const char* q = tok; q < p; ++q) v = v * 10 + unsigned(*q - '0');
                        vals.push_back(v);
                        continue;
                    }
                    for (const char* q = tok; q < p; ++q) {
                        char c = *q;
                        unsigned d = (c >= '0' && c <= '9')   ? unsigned(c - '0')
                                     : (c >= 'a' && c <= 'f') ? unsigned(c - 'a' + 10)
                                     : (c >= 'A' && c <= 'F') ? unsigned(c - 'A' + 10)
                                                               : 0;
                        v = (v << 4) | d;
                    }
                    vals.push_back(v);
                }
            }
            if (!header) {
                if (names.empty() || names[0] != "cyc") {
                    std::fprintf(stderr, "bad trace header in %s\n", path);
                    std::fclose(f);
                    return false;
                }
                cols.assign(names.begin() + 1, names.end());
                for (size_t i = 0; i < cols.size(); ++i) idx[cols[i]] = i;
                header = true;
            } else if (!vals.empty()) {
                cycs.push_back(vals[0]);
                rows.emplace_back(vals.begin() + 1, vals.end());
            }
        }
        std::fclose(f);
        return header && !rows.empty();
    }

    uint64_t get(const char* name, size_t r) const {
        auto it = idx.find(name);
        return it == idx.end() ? 0 : rows[r][it->second];
    }
    void getWide(const char* base, size_t r, uint64_t out[4]) const {
        char buf[96];
        for (int i = 0; i < 4; ++i) {
            std::snprintf(buf, sizeof buf, "%s_%d", base, i);
            out[i] = get(buf, r);
        }
    }
};

}  // namespace zjrtl
