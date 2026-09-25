#pragma once

// 位段辅助函数：在 uint64_t 字数组（packed bit vector）上做 [hi, lo] 读写。
// 供各 flit struct 的 pack()/unpack() 使用；布局约定 = Chisel asUInt：
// Bundle 先声明字段占高位（QoS 恒在 [3:0]）。仅用于边界打包（DPI 薄壳、
// golden trace 解码），模型内部一律传类型化 struct，不做 pack/unpack。

#include <array>
#include <cstddef>
#include <cstdint>

namespace zj {

// 读 w[hi:lo]，要求 1 <= hi-lo+1 <= 64。跨字（如 Addr[95:48]）正确处理。
template <size_t N>
inline uint64_t getBits(const std::array<uint64_t, N>& w, int hi, int lo) {
    const int n = hi - lo + 1;
    const uint64_t m = (n == 64) ? ~uint64_t{0} : ((uint64_t{1} << n) - 1);
    const int word = lo >> 6, sh = lo & 63;
    uint64_t v = (w[word] >> sh) & m;
    const int hiWord = hi >> 6;
    if (hiWord != word) {
        const int rem = 64 - sh;  // 低字实际容纳的位数
        v |= (w[hiWord] & (m >> rem)) << rem;
    }
    return v;
}

// 写 w[hi:lo] = v（v 超宽部分截断），要求 1 <= hi-lo+1 <= 64。
template <size_t N>
inline void setBits(std::array<uint64_t, N>& w, int hi, int lo, uint64_t v) {
    const int n = hi - lo + 1;
    const uint64_t m = (n == 64) ? ~uint64_t{0} : ((uint64_t{1} << n) - 1);
    v &= m;
    const int word = lo >> 6, sh = lo & 63;
    w[word] = (w[word] & ~(m << sh)) | (v << sh);
    const int hiWord = hi >> 6;
    if (hiWord != word) {
        const int rem = 64 - sh;
        const uint64_t hiMask = m >> rem;  // 落入高字的位
        w[hiWord] = (w[hiWord] & ~hiMask) | (v >> rem);
    }
}

// 宽字段（>64b，如 256b Data）整块写入 w[off + 64*K - 1 : off]，off 任意位对齐。
template <size_t N, size_t K>
inline void setWide(std::array<uint64_t, N>& w, int off, const std::array<uint64_t, K>& f) {
    for (size_t i = 0; i < K; i++) {
        const int bitPos = off + 64 * static_cast<int>(i);
        const int word = bitPos >> 6, sh = bitPos & 63;
        if (sh == 0) {
            w[word] = f[i];
        } else {
            // 低字保留 [sh-1:0]，高字保留 [63:sh]（sh>0 时掩码安全）
            w[word] = (w[word] & ((uint64_t{1} << sh) - 1)) | (f[i] << sh);
            w[word + 1] = (w[word + 1] & ~((uint64_t{1} << sh) - 1)) | (f[i] >> (64 - sh));
        }
    }
}

// 读 w[off + 64*K - 1 : off] 到 K 个字（低位在前）。K 显式指定，N 推导。
template <size_t K, size_t N>
inline std::array<uint64_t, K> getWide(const std::array<uint64_t, N>& w, int off) {
    std::array<uint64_t, K> f{};
    for (size_t i = 0; i < K; i++) {
        const int bitPos = off + 64 * static_cast<int>(i);
        const int word = bitPos >> 6, sh = bitPos & 63;
        f[i] = (sh == 0) ? w[word] : ((w[word] >> sh) | (w[word + 1] << (64 - sh)));
    }
    return f;
}

}  // namespace zj
