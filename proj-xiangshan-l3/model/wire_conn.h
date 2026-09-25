#pragma once

// 纯连线接线（无字段变换）：child.Out→child.In、parent.Out→child.Out 等
// 跨方向连接的统一形式（同方向直连用 operator=，见 wolvicmod Signal 快路径）。

namespace zj::detail {

template <class D, class S>
inline void wireConn(D& dst, S& src) {
    dst.assign().reads(src) = [](auto s) { return std::get<0>(s); };
}

}  // namespace zj::detail
