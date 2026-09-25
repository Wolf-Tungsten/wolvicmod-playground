#!/usr/bin/env bash
# proj-xiangshan-l3/verify/run.sh —— 项目侧预制菜（XiangShan 生态元件）vs
# 真实 RTL（xs-utils / dongjiang 源码 → firtool → Verilator）对拍全流程。
# chisel3 标准库元件（Queue/Pipe/FixedArb/RRArb）的对拍在 wolvicmod 仓
# （../../wolvicmod/verify/），本侧只覆盖 XiangShan 生态元件。
#
# 步骤：refgen 逐配置生成 SV（每配置独立进程+独立目录，规避 xs-utils
# SramProto defMap 的跨 elaboration 缓存与 firtool 按名去重的文件覆盖）→
# verilator 编译每个配置为静态库 → 链接 harness → 跑对拍矩阵。
#
# 用法：
#   ./run.sh                  # 全矩阵
#   ./run.sh fastq            # 单模块（fastq|viparb|qosarb|alloc|spsram|dpsram）
#   ./run.sh --skip-refgen    # SV 已生成时跳过 Chisel 阶段
#   ./run.sh --skip-build     # 只重跑对拍
set -euo pipefail

VERIFY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_DIR="$(dirname "$VERIFY_DIR")"
WOLVIC_DIR="$PROJ_DIR/../wolvicmod"
OUT="$PROJ_DIR/build/verify"   # 全部生成物归一到 proj-xiangshan-l3/build/verify（build/ 已入 .gitignore）
MILL=/home/gaoruihao/wksp/mill/mill
VR_ROOT="$(dirname "$(dirname "$(command -v verilator)")")"
JOBS=32
MODULE="all"
SKIP_REFGEN=0
SKIP_BUILD=0
for arg in "$@"; do
  case "$arg" in
    --skip-refgen) SKIP_REFGEN=1 ;;
    --skip-build)  SKIP_BUILD=1 ;;
    -j*)           JOBS="${arg#-j}" ;;
    fastq|viparb|qosarb|alloc|spsram|dpsram|all) MODULE="$arg" ;;
    *) echo "unknown arg: $arg（模块：fastq|viparb|qosarb|alloc|spsram|dpsram|all）" >&2; exit 2 ;;
  esac
done

# 元件 → 配置名列表（配置名 = refgen wrapper 的 desiredName = SV 顶层模块名）
FASTQ_CFGS=(FastQueueRef_s2_x0 FastQueueRef_s4_x0 FastQueueRef_s2_x1)
VIPARB_CFGS=(VipArbRef_n2 VipArbRef_n4 VipArbRef_n8)
QOSARB_CFGS=(QosArbRef_n4_rr QosArbRef_n4_fx)
ALLOC_CFGS=(AllocRef_n4 AllocRef_n16)
SPSRAM_CFGS=(SpSramRef_s16w2_r0_su1_l2_o1 SpSramRef_s16w1_r0_su2_l2_o1 SpSramRef_s16w2_r0_su1_l1_o0 SpSramRef_s4w1_r1_su1_l1_o1)
DPSRAM_CFGS=(DpSramRef_s16w1_b1_su1_l1_o1 DpSramRef_s16w1_b0_su1_l1_o1 DpSramRef_s16w2_b1_su1_l1_o1)

MODULES=()
ALL_CFGS=()
case "$MODULE" in
  fastq)  MODULES=(fastq);  ALL_CFGS=("${FASTQ_CFGS[@]}") ;;
  viparb) MODULES=(viparb); ALL_CFGS=("${VIPARB_CFGS[@]}") ;;
  qosarb) MODULES=(qosarb); ALL_CFGS=("${QOSARB_CFGS[@]}") ;;
  alloc)  MODULES=(alloc);  ALL_CFGS=("${ALLOC_CFGS[@]}") ;;
  spsram) MODULES=(spsram); ALL_CFGS=("${SPSRAM_CFGS[@]}") ;;
  dpsram) MODULES=(dpsram); ALL_CFGS=("${DPSRAM_CFGS[@]}") ;;
  all)    MODULES=(fastq viparb qosarb alloc spsram dpsram)
          ALL_CFGS=("${FASTQ_CFGS[@]}" "${VIPARB_CFGS[@]}" "${QOSARB_CFGS[@]}"
                    "${ALLOC_CFGS[@]}" "${SPSRAM_CFGS[@]}" "${DPSRAM_CFGS[@]}") ;;
esac

# mill 的 out/ 归一到 build/verify/mill（mill 0.12 无 --out-dir 选项，用符号链接
# 保持 refgen/ 目录只含源码；refgen/out 若已是真实目录则先清除）
mkdir -p "$OUT/mill"
if [[ -e "$VERIFY_DIR/refgen/out" && ! -L "$VERIFY_DIR/refgen/out" ]]; then
  rm -rf "$VERIFY_DIR/refgen/out"
fi
ln -sfn "$OUT/mill" "$VERIFY_DIR/refgen/out"

# ---------- 1. refgen：逐配置生成 SV ----------
if [[ "$SKIP_REFGEN" == 0 ]]; then
  echo "==> [1/4] refgen: 逐配置生成 SystemVerilog（${#ALL_CFGS[@]} 个配置）"
  for cfg in "${ALL_CFGS[@]}"; do
    mkdir -p "$OUT/sv/$cfg"
    (cd "$VERIFY_DIR/refgen" && "$MILL" -i refgen.run "$OUT/sv/$cfg" "$cfg") \
      > "$OUT/sv/$cfg.refgen.log" 2>&1 || {
        echo "refgen FAILED for $cfg（日志 $OUT/sv/$cfg.refgen.log）"; tail -5 "$OUT/sv/$cfg.refgen.log"; exit 1; }
    [[ -f "$OUT/sv/$cfg/$cfg.sv" ]] || { echo "refgen 未产出 $cfg.sv"; exit 1; }
  done
else
  echo "==> [1/4] refgen 跳过（--skip-refgen）"
fi

# ---------- 2. verilate + 3. 构建 harness ----------
if [[ "$SKIP_BUILD" == 0 ]]; then
  echo "==> [2/4] verilator: 编译参考模型（${#ALL_CFGS[@]} 个配置）"
  mkdir -p "$OUT/obj"
  for cfg in "${ALL_CFGS[@]}"; do
    obj="$OUT/obj/$cfg"
    mkdir -p "$obj"
    verilator --cc --top-module "$cfg" -Mdir "$obj" --prefix "V$cfg" \
      -Wno-fatal -Wno-WIDTH -Wno-LATCH -Wno-MULTIDRIVEN \
      "$OUT/sv/$cfg"/*.sv > "$OUT/obj/$cfg.verilate.log" 2>&1 || {
        echo "verilate FAILED for $cfg"; tail -10 "$OUT/obj/$cfg.verilate.log"; exit 1; }
    make -C "$obj" -f "V$cfg.mk" -j"$JOBS" > "$OUT/obj/$cfg.make.log" 2>&1 || {
      echo "verilated make FAILED for $cfg"; tail -10 "$OUT/obj/$cfg.make.log"; exit 1; }
  done
  g++ -std=c++20 -O2 -I"$VR_ROOT/include" -c "$VR_ROOT/include/verilated.cpp" -o "$OUT/verilated.o"
  g++ -std=c++20 -O2 -I"$VR_ROOT/include" -c "$VR_ROOT/include/verilated_threads.cpp" -o "$OUT/verilated_threads.o"

  echo "==> [3/4] 构建对拍 harness（${MODULES[*]}）"
  mkdir -p "$OUT/bin"
  for comp in "${MODULES[@]}"; do
    case "$comp" in
      fastq)  cfgs=("${FASTQ_CFGS[@]}") ;;
      viparb) cfgs=("${VIPARB_CFGS[@]}") ;;
      qosarb) cfgs=("${QOSARB_CFGS[@]}") ;;
      alloc)  cfgs=("${ALLOC_CFGS[@]}") ;;
      spsram) cfgs=("${SPSRAM_CFGS[@]}") ;;
      dpsram) cfgs=("${DPSRAM_CFGS[@]}") ;;
    esac
    incs=() libs=()
    for cfg in "${cfgs[@]}"; do
      incs+=("-I$OUT/obj/$cfg")
      libs+=("$OUT/obj/$cfg/V$cfg"__ALL.a)
    done
    g++ -std=c++20 -O2 -Wall -Wextra -Wno-sign-compare \
      -I"$VR_ROOT/include" -I"$VR_ROOT/include/vltstd" \
      -I"$WOLVIC_DIR/include" -I"$PROJ_DIR" -I"$VERIFY_DIR/cosim" \
      "${incs[@]}" \
      "$VERIFY_DIR/cosim/harness_$comp.cpp" "${libs[@]}" "$OUT/verilated.o" "$OUT/verilated_threads.o" \
      -o "$OUT/bin/$comp" || { echo "harness 构建失败：$comp"; exit 1; }
  done
else
  echo "==> [2/4] verilate 跳过 / [3/4] harness 构建跳过（--skip-build）"
fi

# ---------- 4. 跑对拍矩阵 ----------
echo "==> [4/4] 对拍矩阵（${MODULES[*]}）"
fail=0
for comp in "${MODULES[@]}"; do
  "$OUT/bin/$comp" > "$OUT/bin/$comp.log" 2>&1 || fail=1
  cat "$OUT/bin/$comp.log"
done
echo "=============================================="
if [[ "$fail" == 0 ]]; then
  pass_count=0
  for m in "${MODULES[@]}"; do
    n=$(grep -c '^PASS' "$OUT/bin/$m.log")
    pass_count=$((pass_count + n))
  done
  echo "VERIFY ALL-PASS（$pass_count 个配置×seed 全过）"
else
  echo "VERIFY FAIL（见 $OUT/bin/*.log）"
  exit 1
fi
