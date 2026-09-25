#pragma once

// RingSlot：环链路一站一拍的内容，对齐 xijiang/router/base/BaseRouter.scala:29-32
// 的 ChannelBundle（flit: Valid(RingFlit) + rsvd: Valid(UInt(niw.W))）。
// RTL 中环上跑的是压平的 RingFlit(UInt)；本模型保留类型化 flit（路由只访问
// tgt/src/txn/qos/data_id 字段，等价），rsvd 为防饿死令牌旁带（ChannelTap.scala:36）。
//
// 每站每通道每方向一个 Reg<RingSlot>，打 1 拍（层次文档 §3.3）。

#include <cstdint>

namespace zj {

// FlitT 须提供 ::Cfg（含 kNiw）。zj::chi::ReqFlitT/RespFlitT/SnoopFlitT/DataFlitT 均满足。
template <class FlitT>
struct RingSlot {
    bool     valid = false;
    FlitT    flit{};
    bool     rsvd_valid = false;
    uint16_t rsvd_payload = 0;  // niw 位（= Cat(matchTag 高位 nid, tapIdx)，ChannelTap.scala:39）

    bool operator==(const RingSlot&) const = default;
};

}  // namespace zj
