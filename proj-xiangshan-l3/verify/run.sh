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
#   ./run.sh dir              # Directory 对拍（wolvicmod Directory vs refgenDj
#                             #   生成的 dongjiang Directory RTL，真实配置）
#   ./run.sh db               # DataBlock 对拍（同 refgenDj 路径）
#   ./run.sh ring             # 环级对拍（wolvicmod Ring vs RTL ZRING，免 refgen：
#                             #   直接用 XiangShan emu 构建产物 build/rtl 的
#                             #   ZRING2X1C1P1D1M1G32，与目标配置同源同参）
#   ./run.sh socket           # CC socket 对拍（wolvicmod CcSocket vs RTL
#                             #   SocketDevSide+SocketIcnSide 背对背，免 refgen，
#                             #   同样直接用 build/rtl）
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
SKIP_VERILATE=0
for arg in "$@"; do
  case "$arg" in
    --skip-refgen) SKIP_REFGEN=1 ;;
    --skip-build)  SKIP_BUILD=1 ;;
    --skip-verilate) SKIP_VERILATE=1 ;;  # 参考 RTL 已 verilate 过时跳过（harness 照构）
    -j*)           JOBS="${arg#-j}" ;;
    fastq|viparb|qosarb|alloc|spsram|dpsram|ring|socket|bridge|dir|db|backend|frontend|chixbar|all) MODULE="$arg" ;;
    *) echo "unknown arg: $arg（模块：fastq|viparb|qosarb|alloc|spsram|dpsram|ring|socket|bridge|dir|db|backend|frontend|chixbar|all）" >&2; exit 2 ;;
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
XS_RTL="$PROJ_DIR/XiangShan/build/rtl"   # emu 构建产物（ZhuJiang 配置）
ZRING_TOP=ZRING2X1C1P1D1M1G32
case "$MODULE" in
  fastq)  MODULES=(fastq);  ALL_CFGS=("${FASTQ_CFGS[@]}") ;;
  viparb) MODULES=(viparb); ALL_CFGS=("${VIPARB_CFGS[@]}") ;;
  qosarb) MODULES=(qosarb); ALL_CFGS=("${QOSARB_CFGS[@]}") ;;
  alloc)  MODULES=(alloc);  ALL_CFGS=("${ALLOC_CFGS[@]}") ;;
  spsram) MODULES=(spsram); ALL_CFGS=("${SPSRAM_CFGS[@]}") ;;
  dpsram) MODULES=(dpsram); ALL_CFGS=("${DPSRAM_CFGS[@]}") ;;
  ring)   MODULES=(ring) ;;   # 免 refgen：直接用 XS_RTL 的 ZRING
  socket) MODULES=(socket) ;; # 免 refgen：直接用 XS_RTL 的 Socket{Dev,Icn}Side
  bridge) MODULES=(bridge) ;; # 免 refgen：直接用 XS_RTL 的 AxiBridge/AxiLiteBridge
  dir)    MODULES=(dir) ;;    # refgenDj：dongjiang 真实源码生成 Directory RTL
  db)     MODULES=(db) ;;     # refgenDj：dongjiang 真实源码生成 DataBlock RTL
  backend) MODULES=(backend) ;; # refgenDj：dongjiang 真实源码生成 Backend RTL
  frontend) MODULES=(frontend) ;; # refgenDj：dongjiang 真实源码生成 Frontend RTL
  chixbar) MODULES=(chixbar) ;; # refgenDj：dongjiang 真实源码生成 ChiXbar RTL
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

# ---------- 1. refgen：逐配置生成 SV（ring/socket/bridge 免；dir 走 refgenDj）----------
if [[ "$MODULE" == "ring" || "$MODULE" == "socket" || "$MODULE" == "bridge" ]]; then
  echo "==> [1/4] refgen 不适用（$MODULE 直接用 $XS_RTL 的构建产物）"
elif [[ "$MODULE" == "dir" || "$MODULE" == "db" || "$MODULE" == "backend" || "$MODULE" == "frontend" || "$MODULE" == "chixbar" ]]; then
  if [[ "$SKIP_REFGEN" == 0 ]]; then
    DJ_TOP=$(case "$MODULE" in dir) echo Directory;; db) echo DataBlock;; backend) echo Backend;; frontend) echo Frontend;; chixbar) echo ChiXbar;; esac)
    echo "==> [1/4] refgenDj: 生成 dongjiang $DJ_TOP SystemVerilog"
    mkdir -p "$OUT/sv-dj/$DJ_TOP"
    (cd "$VERIFY_DIR/refgen" && "$MILL" -i refgenDj.run "$OUT/sv-dj/$DJ_TOP" "$DJ_TOP") \
      > "$OUT/sv-dj/$DJ_TOP.refgen.log" 2>&1 || {
        echo "refgenDj FAILED（日志 $OUT/sv-dj/$DJ_TOP.refgen.log）"; tail -5 "$OUT/sv-dj/$DJ_TOP.refgen.log"; exit 1; }
    [[ -f "$OUT/sv-dj/$DJ_TOP/$DJ_TOP.sv" ]] || { echo "refgenDj 未产出 $DJ_TOP.sv"; exit 1; }
  else
    echo "==> [1/4] refgenDj 跳过（--skip-refgen）"
  fi
elif [[ "$SKIP_REFGEN" == 0 ]]; then
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
  if [[ "$SKIP_VERILATE" == 1 ]]; then
    echo "==> [2/4] verilate 跳过（--skip-verilate）；直接构建 harness"
  else
  echo "==> [2/4] verilator: 编译参考模型"
  mkdir -p "$OUT/obj"
  if [[ "$MODULE" == "ring" ]]; then
    obj="$OUT/obj/zring"
    mkdir -p "$obj"
    # 顶层文件显式传入；-y 从 emu 构建产物自动解析子模块闭包（文件名=模块名）；
    # xs_assert_shim 提供 xs_assert_v2 桩（emu 流程中它是 difftest DPI-C 导入）
    verilator --cc --top-module "$ZRING_TOP" -Mdir "$obj" --prefix VZRing \
      -Wno-fatal -Wno-WIDTH -Wno-LATCH -Wno-MULTIDRIVEN -Wno-UNOPTTHREADS \
      "$VERIFY_DIR/cosim/xs_assert_shim.sv" "$XS_RTL/$ZRING_TOP.sv" \
      -y "$XS_RTL" +libext+.sv > "$OUT/obj/zring.verilate.log" 2>&1 || {
        echo "verilate FAILED for zring"; tail -10 "$OUT/obj/zring.verilate.log"; exit 1; }
    make -C "$obj" -f VZRing.mk -j"$JOBS" > "$OUT/obj/zring.make.log" 2>&1 || {
      echo "verilated make FAILED for zring"; tail -10 "$OUT/obj/zring.make.log"; exit 1; }
  elif [[ "$MODULE" == "socket" ]]; then
    obj="$OUT/obj/ccsocket"
    mkdir -p "$obj"
    # 同 ring：wrapper 显式传入，子模块闭包（Socket*/ChiPdc*/PDC/Queue）由 -y 解析
    verilator --cc --top-module CcSocketRef -Mdir "$obj" --prefix VCcSocket \
      -Wno-fatal -Wno-WIDTH -Wno-LATCH -Wno-MULTIDRIVEN -Wno-UNOPTTHREADS \
      "$VERIFY_DIR/cosim/xs_assert_shim.sv" "$VERIFY_DIR/cosim/cc_socket_ref.sv" \
      -y "$XS_RTL" +libext+.sv > "$OUT/obj/ccsocket.verilate.log" 2>&1 || {
        echo "verilate FAILED for ccsocket"; tail -10 "$OUT/obj/ccsocket.verilate.log"; exit 1; }
    make -C "$obj" -f VCcSocket.mk -j"$JOBS" > "$OUT/obj/ccsocket.make.log" 2>&1 || {
        echo "verilated make FAILED for ccsocket"; tail -10 "$OUT/obj/ccsocket.make.log"; exit 1; }
  elif [[ "$MODULE" == "bridge" ]]; then
    # 两桥：build/rtl 的 AxiBridge/AxiLiteBridge 即边界顶层（端口与模型一致），
    # 子模块闭包（CtrlMachine/DataBuffer/Queue/SRAM 等）由 -y 解析
    for br in AxiBridge AxiLiteBridge; do
      obj="$OUT/obj/$(echo "$br" | tr 'A-Z' 'a-z')"
      mkdir -p "$obj"
      verilator --cc --top-module "$br" -Mdir "$obj" --prefix "V$br" \
        -Wno-fatal -Wno-WIDTH -Wno-LATCH -Wno-MULTIDRIVEN -Wno-UNOPTTHREADS \
        "$VERIFY_DIR/cosim/xs_assert_shim.sv" "$XS_RTL/$br.sv" \
        -y "$XS_RTL" +libext+.sv > "$obj.verilate.log" 2>&1 || {
          echo "verilate FAILED for $br"; tail -10 "$obj.verilate.log"; exit 1; }
      make -C "$obj" -f "V$br.mk" -j"$JOBS" > "$obj.make.log" 2>&1 || {
        echo "verilated make FAILED for $br"; tail -10 "$obj.make.log"; exit 1; }
    done
  elif [[ "$MODULE" == "dir" || "$MODULE" == "db" || "$MODULE" == "backend" || "$MODULE" == "frontend" || "$MODULE" == "chixbar" ]]; then
    DJ_TOP=$(case "$MODULE" in dir) echo Directory;; db) echo DataBlock;; backend) echo Backend;; frontend) echo Frontend;; chixbar) echo ChiXbar;; esac)
    obj="$OUT/obj/$(echo "$DJ_TOP" | tr 'A-Z' 'a-z')"
    mkdir -p "$obj"
    # 顶层 $DJ_TOP.sv 显式传入；子模块闭包由同目录 -y 解析（refgenDj 单目录取名=模块名）
    verilator --cc --top-module "$DJ_TOP" -Mdir "$obj" --prefix "V$DJ_TOP" \
      -Wno-fatal -Wno-WIDTH -Wno-LATCH -Wno-MULTIDRIVEN -Wno-UNOPTTHREADS \
      "$OUT/sv-dj/$DJ_TOP/$DJ_TOP.sv" \
      -y "$OUT/sv-dj/$DJ_TOP" +libext+.sv > "$obj.verilate.log" 2>&1 || {
        echo "verilate FAILED for $DJ_TOP"; tail -10 "$obj.verilate.log"; exit 1; }
    make -C "$obj" -f "V$DJ_TOP.mk" -j"$JOBS" > "$obj.make.log" 2>&1 || {
      echo "verilated make FAILED for $DJ_TOP"; tail -10 "$obj.make.log"; exit 1; }
  else
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
  fi
  fi  # SKIP_VERILATE
  g++ -std=c++20 -O2 -I"$VR_ROOT/include" -c "$VR_ROOT/include/verilated.cpp" -o "$OUT/verilated.o"
  g++ -std=c++20 -O2 -I"$VR_ROOT/include" -c "$VR_ROOT/include/verilated_threads.cpp" -o "$OUT/verilated_threads.o"

  # zjmodel：非模板模块实现（模板模块与数据结构为 header-only，随 harness 编译）
  mkdir -p "$OUT/zjmodel"
  zj_objs=()
  while IFS= read -r src; do
    obj="$OUT/zjmodel/$(echo "${src#$PROJ_DIR/}" | tr '/' '_').o"
    g++ -std=c++20 -O2 -Wall -Wextra -I"$WOLVIC_DIR/include" -I"$PROJ_DIR" -c "$src" -o "$obj" \
      || { echo "zjmodel 构建失败：$src"; exit 1; }
    zj_objs+=("$obj")
  done < <(find "$PROJ_DIR/model" -name '*.cpp' | sort)
  ar rcs "$OUT/zjmodel/libzjmodel.a" "${zj_objs[@]}"

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
      ring)   cfgs=(zring) ;;
      socket) cfgs=(ccsocket) ;;
      bridge) cfgs=(axibridge axilitebridge) ;;
      dir)    cfgs=(directory) ;;
      db)     cfgs=(datablock) ;;
      backend) cfgs=(backend) ;;
      chixbar) cfgs=(chixbar) ;;
      frontend) cfgs=(frontend) ;;
    esac
    incs=() libs=()
    for cfg in "${cfgs[@]}"; do
      incs+=("-I$OUT/obj/$cfg")
      if [[ "$cfg" == "zring" ]]; then
        libs+=("$OUT/obj/$cfg/VZRing__ALL.a")
      elif [[ "$cfg" == "ccsocket" ]]; then
        libs+=("$OUT/obj/$cfg/VCcSocket__ALL.a")
      elif [[ "$cfg" == "axibridge" ]]; then
        libs+=("$OUT/obj/$cfg/VAxiBridge__ALL.a")
      elif [[ "$cfg" == "axilitebridge" ]]; then
        libs+=("$OUT/obj/$cfg/VAxiLiteBridge__ALL.a")
      elif [[ "$cfg" == "directory" ]]; then
        libs+=("$OUT/obj/$cfg/VDirectory__ALL.a")
      elif [[ "$cfg" == "datablock" ]]; then
        libs+=("$OUT/obj/$cfg/VDataBlock__ALL.a")
      elif [[ "$cfg" == "chixbar" ]]; then
        libs+=("$OUT/obj/$cfg/VChiXbar__ALL.a")
      elif [[ "$cfg" == "backend" ]]; then
        libs+=("$OUT/obj/$cfg/VBackend__ALL.a")
      elif [[ "$cfg" == "frontend" ]]; then
        libs+=("$OUT/obj/$cfg/VFrontend__ALL.a")
      else
        libs+=("$OUT/obj/$cfg/V$cfg"__ALL.a)
      fi
    done
    g++ -std=c++20 -O2 -Wall -Wextra -Wno-sign-compare \
      -I"$VR_ROOT/include" -I"$VR_ROOT/include/vltstd" \
      -I"$WOLVIC_DIR/include" -I"$PROJ_DIR" -I"$VERIFY_DIR/cosim" \
      "${incs[@]}" \
      "$VERIFY_DIR/cosim/harness_$comp.cpp" "${libs[@]}" "$OUT/zjmodel/libzjmodel.a" "$OUT/verilated.o" "$OUT/verilated_threads.o" \
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
