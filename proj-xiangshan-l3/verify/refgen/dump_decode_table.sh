#!/usr/bin/env bash
# 一键再生成 model/dj/dj_decode_table.inc：
# refgenDj 发射 DecodeDump（dongjiang.frontend.decode.Decode.parse 真实源码）
# → verilate 转储器 → 读出四级译码表常量写 .inc。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$(dirname "$HERE")")"
MILL=/home/gaoruihao/wksp/mill/mill
WORK="$PROJ/build/verify/decode-dump"
INC="$PROJ/model/dj/dj_decode_table.inc"

mkdir -p "$WORK/sv"
(cd "$HERE" && "$MILL" -i refgenDj.run "$WORK/sv" DecodeDump) > "$WORK/refgen.log" 2>&1 || {
    echo "refgenDj FAILED（日志 $WORK/refgen.log）"; exit 1; }
(cd "$WORK/sv" && verilator --cc --exe --build -j 32 -Wno-fatal -Wno-WIDTH \
    DecodeDump.sv "$HERE/dump_decode_table.cpp" --top-module DecodeDump \
    -o dump_decode_table > "$WORK/verilate.log" 2>&1) || {
    echo "verilate FAILED（日志 $WORK/verilate.log）"; exit 1; }
"$WORK/sv/obj_dir/dump_decode_table" "$INC"
echo "regenerated $INC"
