# proj-xiangshan-l3/verify/ —— 项目侧预制菜 vs XiangShan 生态 RTL 的 Verilator 对拍

用**真实 Chisel 源码**（xs-utils / dongjiang，chisel 7.13.0 钉死 XiangShan
kunminghu-v3 `build.mill` 版本）生成 SystemVerilog、Verilator 5.047 编译，与
`zj::prefab` 预制菜在相同激励下逐拍比对，证明行为等价。chisel3 标准库元件
（Queue/Pipe/FixedArb/RRArb）的对拍在 wolvicmod 仓（`../../wolvicmod/verify/`），
本侧只覆盖 XiangShan 生态元件。不接入项目侧 CMake/ctest，纯 `run.sh` 驱动。

## 怎么跑

```bash
# 推荐入口：项目根目录的 Makefile
make -C proj-xiangshan-l3 cosim            # 全矩阵
make -C proj-xiangshan-l3 cosim M=fastq    # 单模块（fastq|viparb|qosarb|alloc|spsram|dpsram）

# 或直接调脚本（支持更多开关）：
proj-xiangshan-l3/verify/run.sh               # 全矩阵
proj-xiangshan-l3/verify/run.sh fastq         # 单模块
proj-xiangshan-l3/verify/run.sh --skip-refgen # SV 已生成时跳过 Chisel 阶段
proj-xiangshan-l3/verify/run.sh --skip-build  # 只重跑对拍
```

全部生成物在 `proj-xiangshan-l3/build/verify/`（`build/` 已入 .gitignore），本目录只含源码。

依赖：mill launcher（`~/wksp/mill/mill`，`refgen/.mill-version` 钉 0.12.17）、
Verilator 5.047、OpenJDK 21。firtool 由 firtool-resolver 自动解析到缓存的
1.149.0。参考 RTL 只引用不复制：xs-utils 裁剪子集（import 闭包只到
firrtl.annotations 与 rocket-chip cde）+ dongjiang utils（FastArb/Alloc）+
cde 源码。

## 目录结构

```
verify/
├── refgen/            # 独立 mill 项目：Chisel 参考顶层生成器
│   ├── build.mill     #   chisel 7.13.0 + scala 2.13.17 + xs-utils/dongjiang/cde 源子集
│   └── src/           #   正规 chisel 项目布局：RefGen.scala 只做驱动，
│       ├── RefGen.scala        # 配置汇总/命令行解析/SV 输出
│       └── refs/               # 每模块一个文件：wrapper 定义 + 该模块配置表
│           ├── FastQueueRef.scala  # s{2,4}_x{0,1} × 3 配置
│           ├── VipArbRef.scala     # n{2,4,8} × 3 配置
│           ├── QosArbRef.scala     # n4_rr / n4_fx
│           ├── AllocRef.scala      # n{4,16}
│           ├── SpSramRef.scala     # × 4 配置（Directory/BeatStorage/basic/shouldReset）
│           └── DpSramRef.scala     # × 3 配置（bypass/nobypass/ways=2）
├── cosim/             # C++ 对拍 harness（verilated 参考模型 + zj::prefab）
│   ├── common.h       #   拍协议/失配报告（前 16 拍激励回放）/密度激励
│   └── harness_{fastq,viparb,qosarb,alloc,spsram,dpsram}.cpp
├── run.sh
└── （无生成物——全部产物在 ../build/verify/：sv/<cfg>/、obj/<cfg>/、bin/、mill/、日志；
     refgen/out 是指向 build/verify/mill 的符号链接，mill 0.12 无 --out-dir）
```

## 对拍矩阵（元件 × 参考 × 配置）

| 预制菜 | 参考（真实源码） | 配置 |
|---|---|---|
| `FastQueue` | xs-utils `queue/FastQueue.scala` | N=2、N=4（NoX=false）、N=2 NoX=true |
| `VipArb` | xs-utils `arb/VipArbiter.scala` | N=2、4、8 |
| `QosRRArb`/`QosFixedArb` | dongjiang `utils/FastArb.scala`（fastQosRRArb/fastQosArb） | N=4，bits=Bundle(qos 4b, data 32b) |
| `Alloc` | dongjiang `utils/Alloc.scala` | N=4、16 |
| `SpSram` | xs-utils `SinglePortSramTemplate` | (1,2,+outReg,ways=2) Directory、(2,2,+outReg,ways=1) BeatStorage、(1,1,无 outReg,ways=2)、(1,1,+outReg,sets=4,+shouldReset) 横扫回压 |
| `DpSram` | xs-utils `DualPortSramTemplate` | (bypass,1,1,+outReg,ways=1) replArray、(nobypass)、(bypass,ways=2) |

规模：17 配置 × 3 seed × 每 run 10 万拍。

## 比对协议

- 两侧同一 PRNG（mt19937，固定 seed）逐拍生成激励，完全同步驱动。
- 拍协议：驱动本拍输入 → `clk=0 eval`（组合稳态）→ 采样比对全部输出端口 →
  `clk=1 eval`（提交）。
- 参考侧先复位 4 拍（输入清零），撤复位后第 0 拍开始比对；预制菜侧初始态
  即复位后态（FastQueue 的 `enq_rdy` 对应 `RegInit(true.B)` 初值为 true、
  VipArb 的 vip 对应 `RegInit(1.U)` 初值指向 0 路——两侧约定一致）。
  不加任何 RANDOMIZE define：Verilator 两态零初始化。
- 激励：定向相位（灌满→排空振荡、满时 valid 顶着试探 ready 恢复拍、全 valid
  连发与反压、qos=0xf 与低 qos 混合、SRAM 同址同拍读写冲突、写后跟读、掩码
  扫掠、复位横扫窗口）+ 随机密度分段（100%/50%/10%）。
- 每拍比对所有输出端口；bits 仅在 valid 时比对（见下）。
- 失配报告：元件、配置、seed、拍号、端口（含 lane 号）、两侧值、前 16 拍激励
  回放；每 run 最多报 5 条，计数不停。

## 已知语义收窄点

1. **无效时的 bits 不比对**：valid=0 时数据通路取值属 don't-care。
2. **DpSram 无 bypass 时同址同拍读写**：chisel `SyncReadMem` 此处语义为
   Undefined RDW；firtool/Verilator 的下件是"读地址打拍、下一拍组合取数"——
   写在 fire 拍末已提交，读侧见**新值**。预制菜对齐 Verilator golden（读新值），
   与 ASIC 宏的"读旧值"直觉相反；ZhuJiang 双口用法（replArray bypass=true 前递
   或 Directory 的 d1/d2 前递网络）不依赖该角落。
3. **Alloc 全忙时 freeId=N-1**：chisel `PriorityEncoder` 空输入归末位
   （PriorityMux 默认分支），此时 `out[N-1].valid` 随 in.valid 拉高但不会 fire
   （in_rdy=0）。
4. **FastQueue NoX=false 时 deq.bits 陈旧值**：两侧 array 轨迹一致故实测相同，
   协议上仍按 valid 门控。

## 对拍中抓到并修复的预制菜 bug

| bug | 根因 | 修法 |
|---|---|---|
| `SpSram/DpSram` ShouldReset 横扫窗口长度 | 初实现按 `Sets*kIsc` 拍，未含 SramResetGen 的 4 拍 resetHold 与 interval>1 的 RegNext 尾巴 | 改为 `4 + Sets*kIsc + (kIsc>1)`，与 `SramResetGen.scala` 逐拍推导一致（rst_s4w1 配置逐拍验证 req_rdy） |
| `Alloc` 全忙 freeId | 误取 0；chisel PriorityEncoder 空输入归末位 | free_id 归 N-1 |
| `VipArb` 指针让位方向 | 误为"除 vip 外最低索引"；源码是 highValidMask（vip 之上）优先、绕回之下 | 正向轮转语义修正 |
| `DpSram` nobypass 同址同拍读旧值 | firtool 对 SyncReadMem 的下件是读新值（读地址打拍、下拍组合取） | 读采样从 fire 拍移到 fire+1 拍（对齐 Verilator golden） |

每个修复都同步更新了对应单测并保证两侧 ctest 全绿（wolvicmod 13 个测试
条目、proj-xiangshan-l3 4 个测试条目）。

## refgen 注意事项

- xs-utils 的 `SramProto.defMap` 是 JVM 全局缓存，跨 elaboration 复用
  Definition 会失败——run.sh 每配置独立 refgen 进程 + 独立 SV 目录规避。
- firtool 对内层模块按名去重会跨配置覆盖同名文件——每配置独立 SV 子目录
  同样规避这一点。
