# wolvicmod L3 版 emu 性能拆解（性能优化基线）

> 目的：记录 wolvicmod ZhuJiang L3 版 emu 相对纯 RTL 版的性能差距的**精确拆解**，
> 作为后续性能优化的基线与决策依据。所有数据同机 fresh 测量（32 核 / 186GB，
> Verilator 5.047，coremark-2-iteration + difftest）。
>
> 测量窗口说明：full = coremark 全程（316,801 拍）；50k = 前 5 万拍 boot 窗口
> （`-C 50000`，两侧同一窗口，可比）。

## 1. 速度基线总表

| 配置 | 线程 | 每拍 wall | 全程 host time | 备注 |
|---|---|---|---|---|
| RTL ZhuJiang | 8 | 1.00 µs | **316s** | full；两次独立复现 313s/316s |
| wolvicmod L3 | 16 | 2.67 µs | 876s | full；**wolvic 版最优点** |
| wolvicmod L3 | 8 | 3.83 µs | 1212s | full |
| wolvicmod L3 | 1 | 11.65 µs | ~3690s（推算） | 50k 实测 582.5s 推算 |
| RTL ZhuJiang | 1 | 7.23 µs | ~2290s（推算） | 50k 实测 361.6s 推算 |

所有运行 cycleCnt 均为 316,801，指令流一致——差异纯粹是仿真宿主开销。

## 2. 核心拆解：1T 对 1T（无多线程同步干扰）

wolvic 版比纯 RTL 多出的部分 = 11.65 − 7.23 = **4.42 µs/拍**（1.61×）。
按 perf 符号归组 + 生成代码 zhujiang 信号归因（1569 个生成函数，覆盖 97.6% 采样）：

| 构成 | µs/拍 | 占多出部分 | 归属 |
|---|---|---|---|
| wolvicmod 模型 + DPI glue | ~2.5 | ~55% | **wolvicmod 框架可优化** |
| BlackBox 边界结构成本 | ~2.4 | ~55% | Verilator 侧 |
| （减去）省掉的 L3 RTL 工作 | −0.9 | — | RTL 版需付、wolvic 版省掉 |
| **净多出** | **~4.4** | | 对账：6.44+2.4+2.5≈11.9 ✓；6.44+0.9≈7.5 ✓ |

两侧共有的 SoC 基线 = **6.44 µs/拍**（RTL 侧 sequent 3.89 + comb 2.55，已扣除
zhujiang 归因份额 0.55+0.34）。

### 2.1 模型侧 2.5 µs/拍：框架调度粒度主导

- `wolvicmod::Module::eval()` 调度循环本身：**5.6%**（0.66 µs/拍）——遍历展平
  动作表 + 逐动作虚调用；
- 其余 ~15% 散布在**几千个** `AssignAction<T>::run()` / `UpdateAction<T>::run()` /
  `std::_Function_handler::_M_invoke`——每信号一个 action 对象、一次
  `std::function` 间接调用，单个 <0.4%，聚合是大头。
- 模型/glue 全部已是 **-O3**（CMAKE_BUILD_TYPE=Release，18 个源文件逐个核对）——
  优化空间在框架结构，不在编译选项。

### 2.2 边界侧 ~2.4 µs/拍：不是变量重组

- **变量重组实测免费**：in_pack 收集 + DPI 调用 + 输出拷贝（`nba_sequent__TOP__21`）
  仅 **0.17%**（~20ns/拍）；VlWide 包装、C++ 侧 pack/unpack 均 ~0.00%；
- 真实成本弥漫在两个桶里（相对 SoC 基线 sequent +1.55 / comb +0.82）：
  BlackBox 端口使 L2 侧供给逻辑（如 txdat 仲裁 mux）全部保持 live 无法死码消除、
  o_out NBA 更新后下游组合逻辑每拍整体重估（额外 settle 波次）。
- 注：zhujiang 归因按函数体是否引用 zhujiang 信号计，是**下界**（跨界内联的
  L3 逻辑不含该字样），故边界成本 ±0.5 µs 不确定度。

## 3. 多线程同步损耗（叠加上述固有差距）

wolvic 版 MT 扩展性差：8T→16T 仅 1.43×（RTL 8T 近线性 7.2×）。

- **证据**：16T profile 中 ~89% 采样在 16 个 `__Vthread__nba__s0__t*` 线程函数，
  annotate 到指令级后热点是两个 `pause` 自旋环（各 44%）——Verilator MTask
  依赖计数器轮询（5 万次空转后 `sched_yield`）；
- **机制**：DPI 调用点是每拍的全局收敛点——in_pack（1107b）从 L2/memAXI/cfgAXI
  各分区收集（依赖几乎所有生产者 MTask），o_out（1443b）下一拍到处被消费
  （几乎所有 MTask 依赖它）。每拍 MTask 图坍缩成
  `[全部生产者] → [DPI] → [全部消费者]` 串行波次，同步延迟主导墙钟；
- **对照**：RTL 8T 最热线程函数 pause 也占 98.2%——自旋是 Verilator MT 常态，
  区别在于 RTL 依赖图宽而浅、无全局汇聚点，同步损耗仅占墙钟 ~10%
  （wolvic 8T 为 ~62%、16T 为 ~73%）；
- **每拍 CPU 总量**：wolvic 16T 是 RTL 8T 的 5.1 倍（perf event count
  2.4e13 vs 4.7e12 cycles/10万拍）。

## 4. 优化方向（按预期收益排序）

1. **wolvicmod 框架：action 融合/粗粒度求值**（目标 ~2.5 µs/拍的大部）——
   按模块生成粗粒度求值函数、合并同类型逐信号 action、消除 std::function
   间接调用层；理论上可把 20.9% 压到小个位数，1T 有望从 11.65 → ~9 µs/拍；
2. **边界结构**（~2.4 µs/拍）：减少 BlackBox 端口暴露的无关字段（让 Verilator
   能死码消除供给逻辑）、评估 settle 波次是否可收窄；难度高，需 Verilator 侧实验；
3. **线程数**：当前 `WOLVIC=1` 默认 EMU_THREADS=16 已是最优点（1T 11.65 / 8T
   3.83 / 16T 2.67 µs/拍），无需调整；更多线程受同步主导预计收益递减；
4. 不建议动：`--threads-dpi none` 等 Verilator 分区旋钮，风险高收益不确定。

若 1+2 全做成，wolvic 1T 理论上 ≈ 7 µs/拍，与 RTL 1T 持平。

## 5. 数据产物与复现

**留存的二进制**（对比实验免重建，`make stash-emu NAME=<变体名>` 约定）：

```
proj-xiangshan-l3/build/emu-variants/emu-wolvic-1t   # wolvicmod L3, EMU_THREADS=1
proj-xiangshan-l3/build/emu-variants/emu-rtl-1t      # RTL ZhuJiang, EMU_THREADS=1
```

**perf 数据**（`proj-xiangshan-l3/build/`）：`perf-wolvic-1t.data`（1T 5万拍）、
`perf-rtl-1t.data`、`perf-wolvic.data`（16T 10万拍）、`perf-rtl.data`（8T 10万拍）。

注意：perf.data 按绝对路径解析符号，二进制被覆盖后须用
`perf report --symfs=<dir>` 指向留存副本（先把留存二进制按记录的绝对路径
铺进 symfs 目录树）。

**复现命令**：

```bash
# 构建某变体（以 wolvic 1T 为例；emu.mk 默认变量见 proj Makefile）
cd proj-xiangshan-l3/XiangShan && export NOOP_HOME=$PWD
make emu CONFIG=DefaultConfig LLC=ZhuJiang EMU_THREADS=1 WOLVIC_ZJ=1 \
  RTL_INCLUDE=$PWD/../dpi/sv \
  USER_LDFLAGS="$PWD/../build/libwolviczj_dpi.a $PWD/../build/libzjmodel.a \
                $PWD/../build/wolvicmod/third_party/libfst/libwolvicmod_fst.a -lz" -j32

# 测速（5 万拍窗口）
./build/emu -b 0 -e 0 -C 50000 -i ./ready-to-run/coremark-2-iteration.bin \
  --diff ./ready-to-run/riscv64-nemu-interpreter-so

# 采样
perf record -F 999 -o build/perf-<变体>.data ./build/emu -b 0 -e 0 -C 50000 ...

# 留存
make -C proj-xiangshan-l3 stash-emu NAME=<变体名>
```
