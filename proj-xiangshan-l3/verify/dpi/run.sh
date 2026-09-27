#!/bin/bash
# verify/dpi/run.sh：WolvicZjBB SV 壳 + DPI glue 冒烟测试（不经 emu）。
# verilate 壳 + tb_wolvic_zjbb.cpp + 静态库 → 运行。用法：verify/dpi/run.sh
set -e
cd "$(dirname "$0")/../.."   # proj 根

VERILATOR=${VERILATOR:-/home/gaoruihao/wksp/verilator/bin/verilator}
MDIR=build/dpi-smoke/obj
mkdir -p build/dpi-smoke

cmake --build build -j --target wolviczj_dpi > /dev/null

"$VERILATOR" --cc --exe \
  --no-timing -Wno-WIDTH -Wno-STMTDLY -Wno-UNSIGNED \
  --top-module WolvicZjBB --Mdir "$MDIR" \
  dpi/sv/WolvicZjBB.sv verify/dpi/tb_wolvic_zjbb.cpp \
  -CFLAGS "--std=c++20 -I$PWD -I$PWD/../wolvicmod/include" \
  "$PWD/build/libwolviczj_dpi.a" "$PWD/build/libzjmodel.a" \
  "$PWD/build/wolvicmod/third_party/libfst/libwolvicmod_fst.a" \
  -LDFLAGS "-lz" \
  -o tb_wolvic_zjbb

# 与 proj CMake 工具链一致（模型头使用 GCC 扩展，clang 严格模式拒编）：
# tb 翻译单元用 g++ 预编；verilated 支持文件沿用 verilated.mk 的 clang
# （本机 Verilator 按 clang 配置，其内置 flags 与 g++ 不兼容），clang 链接
# g++ 目标文件（同 libstdc++ ABI）。
g++ --std=c++20 -O1 \
  -I"$PWD" -I"$PWD/../wolvicmod/include" \
  -I/home/gaoruihao/wksp/verilator/include -I/home/gaoruihao/wksp/verilator/include/vltstd \
  -I"$MDIR" -DVERILATOR=1 \
  -c "$PWD/verify/dpi/tb_wolvic_zjbb.cpp" -o "$MDIR/tb_wolvic_zjbb.o"
make -C "$MDIR" -f VWolvicZjBB.mk -j 8 tb_wolvic_zjbb

rm -f build/dpi-smoke/glue_trace.txt
"$MDIR/tb_wolvic_zjbb"
