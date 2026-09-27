// fst_probe：打印指定信号的所有值变化 (time, sig, val)
// 用法：fst_probe <wave.fst> <sig_path_substring1> [sig_path_substring2 ...]
//   子串匹配；--exact 前缀表示精确匹配
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include "fstapi.h"

std::vector<std::string> pats;
std::vector<std::string> exacts;
std::unordered_map<fstHandle, std::string> watched;

void cb(void*, uint64_t time, fstHandle h, const unsigned char* v) {
    printf("%8lu  %s = %s\n", (unsigned long)time, watched[h].c_str(), (const char*)v);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: fst_probe <fst> <pat...>\n"); return 1; }
    for (int i = 2; i < argc; ++i) {
        if (strncmp(argv[i], "--exact=", 8) == 0) exacts.push_back(argv[i] + 8);
        else pats.push_back(argv[i]);
    }
    fstReaderContext* ctx = fstReaderOpen(argv[1]);
    if (!ctx) { fprintf(stderr, "cannot open\n"); return 1; }
    std::string path;
    fstReaderClrFacProcessMaskAll(ctx);
    struct fstHier* h;
    while ((h = fstReaderIterateHier(ctx)) != nullptr) {
        if (h->htyp == FST_HT_SCOPE) { if (!path.empty()) path.push_back('.'); path += h->u.scope.name; }
        else if (h->htyp == FST_HT_UPSCOPE) { size_t p = path.rfind('.'); path.resize(p == std::string::npos ? 0 : p); }
        else if (h->htyp == FST_HT_VAR) {
            size_t saved = path.size();
            if (!path.empty()) path.push_back('.');
            path += h->u.var.name;
            bool match = false;
            for (auto& e : exacts) if (path == e) match = true;
            for (auto& p : pats) if (path.find(p) != std::string::npos) match = true;
            if (match && !watched.count(h->u.var.handle)) {
                watched[h->u.var.handle] = path;
                fstReaderSetFacProcessMask(ctx, h->u.var.handle);
            }
            path.resize(saved);
        }
    }
    fprintf(stderr, "watching %zu signals\n", watched.size());
    fstReaderIterBlocks(ctx, cb, nullptr, nullptr);
    fstReaderClose(ctx);
    return 0;
}
