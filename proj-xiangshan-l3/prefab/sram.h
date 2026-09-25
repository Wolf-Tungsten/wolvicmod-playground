#pragma once

// SRAM 行为模板：对齐 xs-utils 的 SinglePortSramTemplate / DualPortSramTemplate
// （及其底层 SRAMTemplate + SramInstGen 的 SyncReadMem 行为模型）。
//
// 端口贴近源模板：
//   单口 SpSram：In<Dec<SramReqBits>> req（valid/we/addr/wdata/mask 一体）+
//                Out<bool> req_rdy；Out<Dec<SramRespBits>> resp（Valid，无 rdy）
//   双口 DpSram：In<Dec<SramReqBits>> wreq + wreq_rdy；In<Dec<uint32_t>> rreq +
//                rreq_rdy；Out<Dec<SramRespBits>> rresp（Valid，无 rdy）
// mask 为 way 粒度写掩码（bit w 置位写 way w）；Ways==1 时忽略（对应源模板
// way==1 时 mask=None）。
//
// 读时序（与源模板逐配置核对）：
//   kIsc      = ExtraHold ? Setup+1 : Setup            —— 输入稳定拍数
//   宏读采样  = fire 后第 (kIsc==1 ? 0 : kIsc) 拍        —— isc==1 当拍组合读
//              SyncReadMem，isc≥2 经 RegEnable + rreqReg 拉伸后在第 isc 拍采样
//   宏 resp   = 宏读采样后再过 Latency 拍               —— respReg 移位链
//   OutputReg = 再 +1 拍（源模板 RegNext(valid)/RegEnable(data)）
//   即 fire → resp.valid 总延迟 kReadDelay =
//              (kIsc==1 ? Latency : kIsc+Latency) + (OutputReg ? 1 : 0)
//   核对：Directory 配置 (1,2,OutputReg) → 3 拍（resp 对齐 d3）；
//         BeatStorage 配置 (2,2,OutputReg) → 5 拍（= readDsLatency）；
//         replArray 双口 (1,1,OutputReg) → 2 拍（对齐 d2）。
//
// 写时序：isc==1 在 fire 拍末提交（SyncReadMem 写）；isc≥2 推迟 isc-1 拍一次性
// 提交（源模板在 fire+1..fire+isc 拉伸写同一份数据；读侧受 intvCnt 回压，最早
// 也要 fire+interval 拍后才能发起，两种建模对读侧不可区分——DpSram Setup≥2 且
// 读在途中、写在 isc 窗口内落到同地址的极端场景除外，ZhuJiang 双口只用
// Setup==1，不受影响）。
//
// 同拍读写同地址：单口读写互斥（一个 req 口非读即写）天然无冲突；双口读侧在
// fire+1 拍组合采样存储体（对齐 firtool/Verilator 对 SyncReadMem 的下件：读地址
// 打拍、下一拍组合取数）——BypassWrite=false 时同址同拍读写**读到新值**（写已在
// fire 拍末提交；chisel SyncReadMem 此处属 Undefined RDW，本模板对齐 Verilator
// golden 仿真行为；ASIC 宏通常读旧值，ZhuJiang 双口用法不依赖该角落）；
// BypassWrite=true 按掩码前递新写数据（掩码位读旧）——与源模板 bypassData 一致。
//
// 回压：req_rdy/rreq_rdy/wreq_rdy 由 intvCnt 给出，fire 后间隔
// kInterval = max(Latency, kIsc) 拍才恢复（kInterval==1 时恒为 ready）。
// ShouldReset：存储体已由 wolvicmod Mem 零初始化（即源模板复位横扫写入的全
// 零）；横扫窗口对齐 SramResetGen 逐拍推导：复位撤除后 4 拍 resetHold（
// resetDelay=4）+ 每 set 间隔 kIsc 拍写零 +（kIsc>1 时 io.resetState 的
// RegNext 尾巴 1 拍），即 kRstCycles = 4 + Sets*kIsc + (kIsc>1 ? 1 : 0)，
// 期间 req_rdy 拉低。
// 地址合法性同源模板：addr 必须 < Sets（源模板有断言；本模型写口越界由框架
// Mem 检查报错，读口越界属使用方违约）。
//
// 同名解包约定的跨模块写法：读集里的子模块端口按层次路径展开绑定名
// （cappipe.deq → cappipe_deq，层次路径中的点替换为下划线）。

#include <array>
#include <cstdint>

#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/dec.h"
#include "wolvicmod/prefab/pipe.h"

namespace zj::prefab {

using wolvicmod::prefab::Dec;
using wolvicmod::prefab::ValidPipe;

// 请求/响应载荷（贴近 SpSramReq / DpSramWrite / SpSramResp）
template <class T, uint32_t Ways>
struct SramReqBits {
    bool write = false;
    uint32_t addr = 0;
    uint32_t mask = 0;  // way 写掩码；Ways==1 时忽略（源模板 mask=None）
    std::array<T, Ways> data{};

    bool operator==(const SramReqBits&) const = default;
};

template <class T, uint32_t Ways>
struct SramRespBits {
    std::array<T, Ways> data{};

    bool operator==(const SramRespBits&) const = default;
};

namespace detail {

// 按 way 掩码合并一行：掩码位取 newRow，其余保持 oldRow。Ways==1 掩码恒为 1。
template <class T, uint32_t Ways>
std::array<T, Ways> sramMergeWays(const std::array<T, Ways>& oldRow,
                                  const std::array<T, Ways>& newRow, uint32_t mask) {
    std::array<T, Ways> row = oldRow;
    for (uint32_t w = 0; w < Ways; ++w)
        if (Ways == 1 || ((mask >> w) & 1u) != 0) row[w] = newRow[w];
    return row;
}

}  // namespace detail

// ---------------- SpSram：SinglePortSramTemplate ----------------

template <class T, uint32_t Sets, uint32_t Ways = 1, uint32_t Setup = 1, uint32_t Latency = 1,
          bool ExtraHold = false, bool OutputReg = false, bool ShouldReset = false>
class SpSram : public wolvicmod::Module {
public:
    static_assert(Sets >= 1 && Ways >= 1 && Setup >= 1 && Latency >= 1);
    using WayRow = std::array<T, Ways>;
    using ReqBits = SramReqBits<T, Ways>;
    using RespBits = SramRespBits<T, Ways>;
    using DecReq = Dec<ReqBits>;
    using DecResp = Dec<RespBits>;

    static constexpr uint32_t kIsc = ExtraHold ? Setup + 1 : Setup;
    static constexpr uint32_t kInterval = (Latency > kIsc) ? Latency : kIsc;
    static constexpr uint32_t kCapDelay = (kIsc == 1) ? 0 : kIsc;
    static constexpr uint32_t kRamDelay = (kIsc == 1) ? Latency : kIsc + Latency;
    static constexpr uint32_t kHoldDelay = kRamDelay - kCapDelay + (OutputReg ? 1u : 0u);
    static constexpr uint32_t kReadDelay = kRamDelay + (OutputReg ? 1u : 0u);
    // SramResetGen 横扫窗口：4 拍 resetHold + Sets 次写（间隔 kIsc）+
    // （kIsc>1 时 resetState 的 RegNext 尾巴 1 拍）
    static constexpr uint32_t kRstCycles =
        ShouldReset ? 4 + Sets * kIsc + (kIsc > 1 ? 1u : 0u) : 0;

    struct RstState {
        uint32_t cnt = kRstCycles;

        bool operator==(const RstState&) const = default;
    };

    IN(bool, clk);
    IN(DecReq, req);
    OUT(bool, req_rdy);
    OUT(DecResp, resp);  // Valid 通道（无 rdy）

    MEM(WayRow, Sets, ram);
    REG(uint32_t, intv);
    REG(RstState, rst);

    WIRE(bool, w_fire);
    WIRE(bool, w_read_fire);
    WIRE(bool, w_write_fire);
    WIRE(uint32_t, w_addr);
    WIRE(bool, w_wr_pop);
    WIRE(uint32_t, w_wr_addr);

    using HoldPipe = ValidPipe<RespBits, kHoldDelay>;
    SUB(HoldPipe, holdpipe);
    // kIsc>1 才例化（请求位寄存 + 采样/写提交推迟），kIsc==1 为纯组合直通
    using CapPipe = ValidPipe<uint32_t, (kCapDelay > 0 ? kCapDelay : 1)>;
    using WrPipe = ValidPipe<ReqBits, (kIsc > 1 ? kIsc - 1 : 1)>;

    SpSram() {
        holdpipe.clk = clk;
        registerPorts();
        if constexpr (kIsc == 1) {
            registerWriteDirect();
            registerReadDirect();
        } else {
            // 条件例化的子模块：局部引用即可——所有权由框架持有（children_），
            // 接线全部发生在构造期内，无需成员指针
            auto& cappipe = createChildModule<CapPipe>("cappipe");
            auto& wrpipe = createChildModule<WrPipe>("wrpipe");
            cappipe.clk = clk;
            wrpipe.clk = clk;
            registerWritePiped(wrpipe);
            registerReadPiped(cappipe);
        }
    }

private:
    void registerPorts() {
        req_rdy.assign().reads(intv, rst) = [](auto src) {
            auto [intv, rst] = src;
            return intv == 0 && rst.cnt == 0;
        };
        w_fire.assign().reads(req, req_rdy) = [](auto src) {
            auto [req, req_rdy] = src;
            return req.valid && req_rdy;
        };
        w_read_fire.assign().reads(w_fire, req) = [](auto src) {
            auto [w_fire, req] = src;
            return w_fire && !req.bits.write;
        };
        w_write_fire.assign().reads(w_fire, req) = [](auto src) {
            auto [w_fire, req] = src;
            return w_fire && req.bits.write;
        };
        w_addr.assign().reads(req) = [](auto src) {
            auto [req] = src;
            return req.bits.addr;
        };
        intv.update().on(posedge(clk)).reads(intv, w_fire) = [](auto src) -> uint32_t {
            auto [intv, w_fire] = src;
            if (w_fire) return kInterval - 1;
            return intv > 0 ? intv - 1 : 0;
        };
        rst.update().on(posedge(clk)).reads(rst) = [](auto src) {
            auto [rst] = src;
            RstState next = rst;
            if (next.cnt > 0) --next.cnt;
            return next;
        };
        resp = holdpipe.deq;
        if constexpr (kIsc == 1) {  // 恒驱动的占位（写直通时无弹出级）
            w_wr_pop = false;
            w_wr_addr = 0u;
        }
    }

    void registerWriteDirect() {  // kIsc==1：fire 拍末提交
        ram.update().on(posedge(clk)).addr(w_addr).en(w_write_fire).reads(req, ram, w_addr) =
            [](auto src) {
                auto [req, ram, w_addr] = src;
                return detail::sramMergeWays<T, Ways>(ram[w_addr], req.bits.data, req.bits.mask);
            };
    }

    void registerWritePiped(WrPipe& wrpipe) {  // kIsc>1：请求位寄存，推迟 kIsc-1 拍提交
        wrpipe.enq.assign().reads(w_write_fire, req) = [](auto src) {
            auto [w_write_fire, req] = src;
            DecReq d;
            d.valid = w_write_fire;
            d.bits = req.bits;
            return d;
        };
        w_wr_pop.assign().reads(wrpipe.deq) = [](auto src) {
            auto [wrpipe_deq] = src;
            return wrpipe_deq.valid;
        };
        w_wr_addr.assign().reads(wrpipe.deq) = [](auto src) {
            auto [wrpipe_deq] = src;
            return wrpipe_deq.bits.addr;
        };
        ram.update().on(posedge(clk)).addr(w_wr_addr).en(w_wr_pop).reads(wrpipe.deq, ram, w_wr_addr) =
            [](auto src) {
                auto [wrpipe_deq, ram, w_wr_addr] = src;
                return detail::sramMergeWays<T, Ways>(ram[w_wr_addr], wrpipe_deq.bits.data,
                                                      wrpipe_deq.bits.mask);
            };
    }

    void registerReadDirect() {  // kCapDelay==0：fire 拍组合采样
        holdpipe.enq.assign().reads(w_read_fire, ram, w_addr) = [](auto src) {
            auto [w_read_fire, ram, w_addr] = src;
            DecResp d;
            d.valid = w_read_fire;
            d.bits.data = w_read_fire ? ram[w_addr] : WayRow{};
            return d;
        };
    }

    void registerReadPiped(CapPipe& cappipe) {  // kCapDelay>0：第 kIsc 拍组合采样
        cappipe.enq.assign().reads(w_read_fire, w_addr) = [](auto src) {
            auto [w_read_fire, w_addr] = src;
            Dec<uint32_t> d;
            d.valid = w_read_fire;
            d.bits = w_addr;
            return d;
        };
        holdpipe.enq.assign().reads(cappipe.deq, ram) = [](auto src) {
            auto [cappipe_deq, ram] = src;
            DecResp d;
            d.valid = cappipe_deq.valid;
            d.bits.data = cappipe_deq.valid ? ram[cappipe_deq.bits] : WayRow{};
            return d;
        };
    }
};

// ---------------- DpSram：DualPortSramTemplate（1R1W） ----------------

template <class T, uint32_t Sets, uint32_t Ways = 1, bool BypassWrite = false,
          uint32_t Setup = 1, uint32_t Latency = 1, bool ExtraHold = false,
          bool OutputReg = false, bool ShouldReset = false>
class DpSram : public wolvicmod::Module {
public:
    static_assert(Sets >= 1 && Ways >= 1 && Setup >= 1 && Latency >= 1);
    using WayRow = std::array<T, Ways>;
    using WrBits = SramReqBits<T, Ways>;  // write 字段忽略（wreq 恒为写）
    using RespBits = SramRespBits<T, Ways>;
    using DecWr = Dec<WrBits>;
    using DecRd = Dec<uint32_t>;
    using DecResp = Dec<RespBits>;

    static constexpr uint32_t kIsc = ExtraHold ? Setup + 1 : Setup;
    static constexpr uint32_t kInterval = (Latency > kIsc) ? Latency : kIsc;
    static constexpr uint32_t kCapDelay = (kIsc == 1) ? 0 : kIsc;
    static constexpr uint32_t kRamDelay = (kIsc == 1) ? Latency : kIsc + Latency;
    static constexpr uint32_t kReadDelay = kRamDelay + (OutputReg ? 1u : 0u);
    // 双口读路径（isc==1）：读地址寄存一拍、fire+1 拍组合采样存储体——对齐
    // firtool/Verilator 对 SyncReadMem 的下件（_R0_addr_d0 打拍、下一拍组合取数）。
    // 后果：同址同拍读写时读侧见**新值**（写已提交；chisel SyncReadMem 此处属
    // Undefined RDW，本模板对齐 Verilator golden，ASIC 宏通常读旧值）。
    // isc≥2 时保持 fire+kIsc 拍采样（输入寄存 + 拉伸使能路径，见头注释）。
    static constexpr uint32_t kDpCapDepth = (kCapDelay > 0) ? kCapDelay : 1;
    static constexpr uint32_t kDpHoldDepth = kReadDelay - kDpCapDepth;
    static constexpr uint32_t kRstCycles =
        ShouldReset ? 4 + Sets * kIsc + (kIsc > 1 ? 1u : 0u) : 0;

    struct RstState {
        uint32_t cnt = kRstCycles;

        bool operator==(const RstState&) const = default;
    };

    // 读地址 + 并发写前递（bypass）随拍携带的信息
    struct CapInfo {
        uint32_t addr = 0;
        uint32_t bmask = 0;
        WayRow bdata{};

        bool operator==(const CapInfo&) const = default;
    };

    IN(bool, clk);
    IN(DecWr, wreq);
    OUT(bool, wreq_rdy);
    IN(DecRd, rreq);
    OUT(bool, rreq_rdy);
    OUT(DecResp, rresp);  // Valid 通道（无 rdy）

    MEM(WayRow, Sets, ram);
    REG(uint32_t, r_intv);
    REG(uint32_t, w_intv);
    REG(RstState, rst);

    WIRE(bool, w_r_fire);
    WIRE(bool, w_w_fire);
    WIRE(uint32_t, w_raddr);
    WIRE(uint32_t, w_waddr);
    WIRE(uint32_t, w_bmask);
    WIRE(bool, w_wr_pop);
    WIRE(uint32_t, w_wr_addr);

    using CapPipe = ValidPipe<CapInfo, kDpCapDepth>;
    using WrPipe = ValidPipe<WrBits, (kIsc > 1 ? kIsc - 1 : 1)>;
    using HoldPipe = ValidPipe<RespBits, (kDpHoldDepth > 0 ? kDpHoldDepth : 1)>;

    DpSram() {
        // 条件例化的子模块：局部引用即可——所有权由框架持有（children_），
        // 接线全部发生在构造期内，无需成员指针
        auto& cappipe = createChildModule<CapPipe>("cappipe");  // 读路径恒有（打拍采样）
        cappipe.clk = clk;
        registerPorts();
        registerReadPath(cappipe);
        if constexpr (kIsc == 1) {
            registerWriteDirect();
        } else {
            auto& wrpipe = createChildModule<WrPipe>("wrpipe");
            wrpipe.clk = clk;
            registerWritePiped(wrpipe);
        }
        if constexpr (kDpHoldDepth > 0) {
            auto& holdpipe = createChildModule<HoldPipe>("holdpipe");
            holdpipe.clk = clk;
            registerHoldPipe(cappipe, holdpipe);
            rresp = holdpipe.deq;
        } else {
            registerRespDirect(cappipe);  // kDpHoldDepth==0：采样拍即 resp 拍
        }
    }

private:
    void registerPorts() {
        rreq_rdy.assign().reads(r_intv, rst) = [](auto src) {
            auto [r_intv, rst] = src;
            return r_intv == 0 && rst.cnt == 0;
        };
        wreq_rdy.assign().reads(w_intv, rst) = [](auto src) {
            auto [w_intv, rst] = src;
            return w_intv == 0 && rst.cnt == 0;
        };
        w_r_fire.assign().reads(rreq, rreq_rdy) = [](auto src) {
            auto [rreq, rreq_rdy] = src;
            return rreq.valid && rreq_rdy;
        };
        w_w_fire.assign().reads(wreq, wreq_rdy) = [](auto src) {
            auto [wreq, wreq_rdy] = src;
            return wreq.valid && wreq_rdy;
        };
        w_raddr.assign().reads(rreq) = [](auto src) {
            auto [rreq] = src;
            return rreq.bits;
        };
        w_waddr.assign().reads(wreq) = [](auto src) {
            auto [wreq] = src;
            return wreq.bits.addr;
        };
        if constexpr (BypassWrite) {
            // bypassWrite && !singlePort：同拍读写同址时前递写数据（掩码位）；
            // Ways==1 时源模板 waymask=None 等价掩码全 1
            w_bmask.assign().reads(w_r_fire, w_w_fire, w_raddr, w_waddr, wreq) = [](auto src) -> uint32_t {
                auto [w_r_fire, w_w_fire, w_raddr, w_waddr, wreq] = src;
                if (!(w_r_fire && w_w_fire && w_raddr == w_waddr)) return 0;
                return Ways == 1 ? 1u : wreq.bits.mask;
            };
        } else {
            w_bmask = 0u;
        }
        r_intv.update().on(posedge(clk)).reads(r_intv, w_r_fire) = [](auto src) -> uint32_t {
            auto [r_intv, w_r_fire] = src;
            if (w_r_fire) return kInterval - 1;
            return r_intv > 0 ? r_intv - 1 : 0;
        };
        w_intv.update().on(posedge(clk)).reads(w_intv, w_w_fire) = [](auto src) -> uint32_t {
            auto [w_intv, w_w_fire] = src;
            if (w_w_fire) return kInterval - 1;
            return w_intv > 0 ? w_intv - 1 : 0;
        };
        rst.update().on(posedge(clk)).reads(rst) = [](auto src) {
            auto [rst] = src;
            RstState next = rst;
            if (next.cnt > 0) --next.cnt;
            return next;
        };
        if constexpr (kIsc == 1) {
            w_wr_pop = false;
            w_wr_addr = 0u;
        }
    }

    void registerWriteDirect() {
        ram.update().on(posedge(clk)).addr(w_waddr).en(w_w_fire).reads(wreq, ram, w_waddr) =
            [](auto src) {
                auto [wreq, ram, w_waddr] = src;
                return detail::sramMergeWays<T, Ways>(ram[w_waddr], wreq.bits.data, wreq.bits.mask);
            };
    }

    void registerWritePiped(WrPipe& wrpipe) {
        wrpipe.enq.assign().reads(w_w_fire, wreq) = [](auto src) {
            auto [w_w_fire, wreq] = src;
            DecWr d;
            d.valid = w_w_fire;
            d.bits = wreq.bits;
            return d;
        };
        w_wr_pop.assign().reads(wrpipe.deq) = [](auto src) {
            auto [wrpipe_deq] = src;
            return wrpipe_deq.valid;
        };
        w_wr_addr.assign().reads(wrpipe.deq) = [](auto src) {
            auto [wrpipe_deq] = src;
            return wrpipe_deq.bits.addr;
        };
        ram.update().on(posedge(clk)).addr(w_wr_addr).en(w_wr_pop).reads(wrpipe.deq, ram, w_wr_addr) =
            [](auto src) {
                auto [wrpipe_deq, ram, w_wr_addr] = src;
                return detail::sramMergeWays<T, Ways>(ram[w_wr_addr], wrpipe_deq.bits.data,
                                                      wrpipe_deq.bits.mask);
            };
    }

    // 宏读采样数据：bypass 掩码位取前递写数据，其余取 ram
    static RespBits mergeRead(const WayRow& row, uint32_t bmask, const WayRow& bdata) {
        RespBits d;
        for (uint32_t w = 0; w < Ways; ++w)
            d.data[w] = ((bmask >> w) & 1u) != 0 ? bdata[w] : row[w];
        return d;
    }

    // fire 拍：读地址 + 并发写前递信息入 cappipe（kDpCapDepth 拍后到采样拍）
    void registerReadPath(CapPipe& cappipe) {
        cappipe.enq.assign().reads(w_r_fire, w_raddr, w_bmask, wreq) = [](auto src) {
            auto [w_r_fire, w_raddr, w_bmask, wreq] = src;
            Dec<CapInfo> d;
            d.valid = w_r_fire;
            d.bits.addr = w_raddr;
            d.bits.bmask = w_bmask;
            d.bits.bdata = wreq.bits.data;
            return d;
        };
    }

    // 采样拍（fire + kDpCapDepth）：组合读存储体 + bypass 合并，入 holdpipe
    void registerHoldPipe(CapPipe& cappipe, HoldPipe& holdpipe) {
        holdpipe.enq.assign().reads(cappipe.deq, ram) = [](auto src) {
            auto [cappipe_deq, ram] = src;
            DecResp d;
            d.valid = cappipe_deq.valid;
            d.bits = cappipe_deq.valid
                         ? mergeRead(ram[cappipe_deq.bits.addr], cappipe_deq.bits.bmask,
                                     cappipe_deq.bits.bdata)
                         : RespBits{};
            return d;
        };
    }

    // kDpHoldDepth==0：采样拍即 resp 拍（组合直出）
    void registerRespDirect(CapPipe& cappipe) {
        rresp.assign().reads(cappipe.deq, ram) = [](auto src) {
            auto [cappipe_deq, ram] = src;
            DecResp d;
            d.valid = cappipe_deq.valid;
            d.bits = cappipe_deq.valid
                         ? mergeRead(ram[cappipe_deq.bits.addr], cappipe_deq.bits.bmask,
                                     cappipe_deq.bits.bdata)
                         : RespBits{};
            return d;
        };
    }
};

}  // namespace zj::prefab
