# Wolvic ZhuJiang 直接 C++ 周期接口

入口是 [`cpp/wolvic_zj_model.h`](../cpp/wolvic_zj_model.h) 中的 `zj::WolvicZjModel`，CMake target 为 `wolviczj_cpp`，产物为 `build/libwolviczj_cpp.a`。接口封装 `WolvicZjTop`，直接传递 `xs::CHIREQ/CHIRSP/CHIDAT/CHISNP` 和 `axi::*Flit`，不调用 DPI，也不依赖 Verilator 或 gem5。

## 生命周期和周期约定

- 构造时分配独立模型、执行 `elaborate()`、设置 `ci` 和低电平 `clk`，初始化所有输入并让组合逻辑稳定。`ci` 默认 0，接受 4 位取值，构造后固定。
- `cycles()` 是已提交的周期数，构造后为 0。构造不执行隐藏的预热周期；DongJiang 的目录初始化与请求唤醒仍按原模型推进，首次 miss 可能等待约 8.2k 拍。
- 每个仿真周期调用一次 `eval(inputs)`。内部先沉定输入，再执行一次上升沿提交，最后拉低时钟。框架要求三个内部 `WolvicZjTop::eval()` 调用，只有上升沿推进状态；不能省掉输入沉定调用。
- `eval(inputs)` **返回本次上升沿之前的输出快照**，必须用它与本次 `inputs` 的 `valid && ready` 判断握手。`outputs()` 则读取当前已稳定的输出，调用 `eval` 后看到的是沿后输出，供下个周期使用。若需要跨周期保存，复制快照，不要保留 `outputs()` 的引用。
- wrapper 不缓存事务或自动重试。调用方应保持输入 `valid/bits`，直到本拍返回的 `ready` 表示已接受；同样只在 `valid && ready` 时消费输出。
- 每个实例拥有自己的模型、周期计数器和 trace sink；销毁时释放。禁止复制和移动；同一实例的调用须串行。重新构造得到新的初态，不提供运行中 reset 或 checkpoint。

现有 `test_comb_audit` 验证模型边界不存在输入到输出的组合直通，新接口测试也检查每次调用的沿前返回值等于调用前的 `outputs()`。未来若模型修改破坏该约定，需要重新评估宿主调度方式。

## 信号与默认值

| 方向 | `Inputs` / `Outputs` 字段 | 载荷 |
|---|---|---|
| 上游 → LLC | `chi_tx_req/rsp/dat`；对应 ready 在输出中 | `Valid<xs::CHI*>` |
| LLC → 上游 | `chi_rx_rsp/dat/snp`；对应 ready 在输入中 | `Valid<xs::CHI*>` |
| LLC → 内存 | `mem_aw/w/ar`；对应 ready 在输入中 | `Valid<axi::*Flit>` |
| 内存 → LLC | `mem_b/r`；对应 ready 在输出中 | `Valid<axi::*Flit>` |
| cfgAXI | `cfg_aw/w/ar/b/r`，方向与 memAXI 相同 | `Valid<axi::*Flit>` |
| 观察口 | `cc_tx_req`，内部不接受该通道 | `Valid<chi::RReqFlit>` |

ready 字段统一以 `_rdy` 结尾。`Inputs{}` 的所有 valid 和 ready 都为 false，避免自动吞掉未处理事务。单核宿主应显式打开所支持的 CHI/内存接收通道。cfgAXI 默认保持无返回、拒绝请求；后续 gem5 接入应检查不支持的 cfg 请求并报错。

最小调度示例：

```cpp
#include "cpp/wolvic_zj_model.h"

zj::WolvicZjModel llc(0);
zj::WolvicZjModel::Inputs in;
in.chi_rx_rsp_rdy = true;
in.chi_rx_dat_rdy = true;
in.chi_rx_snp_rdy = true;
// 填入请求；内存侧是否 ready 由宿主实际接收能力决定。
// in.chi_tx_req = {true, request};

const auto sampled = llc.eval(in);  // 一个周期
if (in.chi_tx_req.valid && sampled.chi_tx_req_rdy)
    in.chi_tx_req = {};            // 下一拍撤销已接受的请求
if (sampled.mem_ar.valid && in.mem_ar_rdy) {
    const zj::axi::ARFlit ar = sampled.mem_ar.bits;
    // 宿主保存 ar.id、地址、突发信息，并在内存完成后驱动 in.mem_r。
    (void)ar;
}
```

完整可执行事务驱动见 [`tests/test_wolvic_zj_model.cpp`](../tests/test_wolvic_zj_model.cpp)，包含 AXI R 延迟、两拍返回、CHI CompAck 和背压处理。

## 可选周期 trace

`setTraceSink()` 接受同步回调，默认关闭，无环境变量或全局文件。每拍回调收到 `CycleTrace`，含零起始 `cycle`、`ci`、本拍完整 `inputs`、沿前 `outputs` 和沿后 `next_outputs`，均为类型化快照。仅开启 trace 时构建该记录。

调用方可将所需字段写入项目内日志，或在 gem5 调试时交给 debug flag 输出。例如：

```cpp
std::ofstream trace("build/runs/llc-cycles.txt");  // 另需 #include <fstream>
llc.setTraceSink([&trace](const zj::WolvicZjModel::CycleTrace& t) {
    trace << t.cycle
          << " req=" << (t.inputs.chi_tx_req.valid && t.outputs.chi_tx_req_rdy)
          << " ar=" << (t.outputs.mem_ar.valid && t.inputs.mem_ar_rdy)
          << " r=" << (t.inputs.mem_r.valid && t.outputs.mem_r_rdy)
          << " dat=" << (t.outputs.chi_rx_dat.valid && t.inputs.chi_rx_dat_rdy)
          << '\n';
});
// llc.eval(in) ...
llc.setTraceSink({});  // 在 trace 流销毁前解除回调
```

回调不得重入或修改本实例；若需保留完整记录，应复制。回调在周期提交和计数更新后执行，即使回调抛出异常，该周期也已提交。

## 构建和验证

从项目目录执行，缓存、临时文件和产物均保留在项目内：

```bash
cd proj-xiangshan-l3
mkdir -p build/cache/ccache build/tmp
export CCACHE_DIR="$PWD/build/cache/ccache"
export TMPDIR="$PWD/build/tmp"
cmake -S . -B build
cmake --build build -j16 --target wolviczj_cpp test_wolvic_zj_model
ctest --test-dir build -R '^test_wolvic_zj_model$' --output-on-failure

# 现有模型回归
cmake --build build -j16
ctest --test-dir build --output-on-failure -j8
```

CMake 消费者使用 `target_link_libraries(your_target PRIVATE wolviczj_cpp)`，由 target 传递模型库、头文件路径和 C++20 要求。直接接口测试的链接链为 `wolviczj_cpp → zjmodel → wolvicmod`（当前开启可选 FST，附带 libfst/zlib），不链接 `wolviczj_dpi` 或 Verilator runtime。gem5 适配层沿用这一 C++20 编译要求，具体连接方式见 [XiangShan gem5 集成说明](gem5-wolvic-zhujiang-l3-integration.md)。

2026-10-03 验证结果：

- 新增测试 3 项全部通过，146,580 个断言通过：构造/实例隔离/trace；两个 LLC bank 的 miss、两拍内存返回、ReadReceipt/CompData/CompAck、背压稳定性与后续 hit；device 写的 CHI 数据注入、cfgAXI AW/W/B 和 Comp 返回。
- 两个读 miss 均只发出一次 AR，每次返回 2 个 256-bit beat，按 DataID 检查全部 64B；后续同地址读命中且不再发 AR。请求完成后继续空跑 256 拍，检查没有重复响应或额外内存事务。
- 全量 CTest 首轮 17 项通过，前端 trace 测试因缺文件跳过。完整 CoreMark 模型回放通过：316,748 行、5,423,490 次比较、0 mismatch。
- 从已有 `build/trace/cm_full.fst` 提取前 20,000 拍，补跑前端 trace 测试通过：19,965 行、265,459 次比较、0 mismatch。合计 18/18 项均已实际执行通过。

日志位于 `build/runs/gem5-stage1-cpp-20261003/`：`configure.log`、`build.log`、`build-all.log`、`cpp-test.log`、`ctest.log`、`extract-cc.log`、`cc-replay.log`。前端补测可复现为：

```bash
fst2vcd build/trace/cm_full.fst | \
  python3 verify/trace/extract_cc_trace.py --start 0 --end 20000 \
    -o build/runs/gem5-stage1-cpp-20261003/cc_front.txt
CC_TRACE="$PWD/build/runs/gem5-stage1-cpp-20261003/cc_front.txt" \
  ctest --test-dir build -R '^test_trace_replay$' -V
```

该接口是 gem5 适配层的宿主无关基础：`WolvicZjL3` 在 gem5 中负责 Packet↔CHI 和 AXI↔memory controller 转换，直接接口测试仍使用 synthetic 驱动验证模型本身。完整连接拓扑、运行命令和 Linux 验证证据见 [XiangShan gem5 集成说明](gem5-wolvic-zhujiang-l3-integration.md)。
