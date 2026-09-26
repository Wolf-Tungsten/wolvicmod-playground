#pragma once

// AXI4 通道 flit：zhujiang/axi/package.scala 的 AXFlit(AW/AR)/WFlit/RFlit/BFlit
// 的 C++ 镜像。kunminghu-v3 两路 master 口（ZhuJiangNoCTopology.scala +
// SoC.scala:148 L3OuterBusWidth=256；AxiDeviceParams 默认 buffers=0 →
// IoWrapper 的 AxiBufferChain 直通，边界即桥自身 axi 端口）：
//   memAXI（SNodeAxiBridge）：id 6b（= log2Ceil(64 CM)）/ addr 48b / data 256b
//   cfgAXI（HiNodeAxiLiteBridge）：id 3b（= log2Ceil(8 CM)）/ addr 48b / data 256b
// 零宽字段（user/len 以外的 AxiLite 缩减位）不例化；lock/prot/region 在桥内
// DontCare → 生成 SV 恒 0（AxiBridge.sv:20105-20107 核实），模型同取 0。

#include <array>
#include <cstdint>

namespace zj::axi {

// AWFlit/ARFlit 同构（RTL 中也是同一 AXFlit 的两个别名）
struct AxFlit {
    uint8_t  id     = 0;  // [5:0]（cfg 侧 [2:0]，高位恒 0）
    uint64_t addr   = 0;  // [47:0]
    uint8_t  len    = 0;  // [7:0]
    uint8_t  size   = 0;  // [2:0]
    uint8_t  burst  = 0;  // [1:0]，桥恒 1（INCR）
    bool     lock   = false;
    uint8_t  cache  = 0;  // [3:0]
    uint8_t  prot   = 0;  // [2:0]
    uint8_t  qos    = 0;  // [3:0]
    uint8_t  region = 0;  // [3:0]

    bool operator==(const AxFlit&) const = default;
};
using AWFlit = AxFlit;
using ARFlit = AxFlit;

struct WFlit {
    std::array<uint64_t, 4> data{};  // [255:0]，低位在前
    uint32_t strb = 0;               // [31:0]
    bool     last = false;

    bool operator==(const WFlit&) const = default;
};

struct RFlit {
    uint8_t                 id   = 0;  // [5:0]（cfg 侧 [2:0]）
    std::array<uint64_t, 4> data{};
    uint8_t                 resp = 0;  // [1:0]
    bool                    last = false;

    bool operator==(const RFlit&) const = default;
};

struct BFlit {
    uint8_t id   = 0;  // [5:0]（cfg 侧 [2:0]）
    uint8_t resp = 0;  // [1:0]

    bool operator==(const BFlit&) const = default;
};

}  // namespace zj::axi
