# proj-xiangshan-l3 一键入口
#
#   make test                 项目侧单元测试（ctest，逐文件独立条目）
#   make emu                  构建 XiangShan emu（默认 DefaultConfig + ZhuJiang L3）
#           [TRACE=fst]       带波形（EMU_TRACE=fst；fst 之外取值见 difftest/verilator.mk）
#                             trace 标志变化不被 XiangShan make 跟踪，本 Makefile 按
#                             印记检测：仅 trace 变化时删 build/verilator-compile 重 verilate
#   make emu WOLVIC=1         构建 wolvicmod L3 模型版 emu（P5）：强制 LLC=ZhuJiang +
#                             WOLVIC_ZJ=1——chisel BlackBox 替换 ZhuJiang RTL，SV DPI
#                             薄壳（dpi/sv，经 RTL_INCLUDE 注入）调 libwolviczj_dpi.a
#                             （先自动构建；difftest USER_LDFLAGS 链接注入）
#   make coremark             用现有 emu 运行 coremark 仿真
#   make replay [N=20000]     coremark 前端 N 拍 trace 重放到 CC 边界：
#                             emu dump FST（需 emu 带 TRACE=fst 构建）→ fst2vcd
#                             → verify/trace/extract_cc_trace.py → ctest -R trace_replay
#   make replay-top           coremark 全程 trace 重放到 WolvicZjTop 三边界（P4b）：
#                             emu 全程 dump FST → verify/trace/extract_top_trace.cpp
#                             （C++ libfst 直读）→ ctest -R wolvic_top_replay
#   make emu-clean            手动清除 XiangShan 构建产物
#
# 本仓构建产物全部在 build/ 下（已入 .gitignore）；XiangShan 子模块的产物在
# XiangShan/build/ 下（子模块自身 .gitignore 覆盖）。
#
# emu 配置防呆：XiangShan 的 Makefile 不把 --llc 当构建依赖，切换配置不 clean 会
# 静默复用旧 Verilog。本 Makefile 用 XiangShan/build/.llc-config 印记跟踪当前配置
# （内容 "<LLC> trace=<TRACE>"）：LLC 不匹配时自动 emu-clean（重 elaborate）；
# 仅 trace 设置变化时只删 build/verilator-compile 强制重 verilate，不碰 RTL。

M ?=
TRACE ?=

SHELL := /bin/bash

.PHONY: test emu coremark emu-clean replay

test:
	cmake -S . -B build
	cmake --build build -j
	cd build && ctest --output-on-failure

# --- XiangShan emu ---
LLC    ?= ZhuJiang
WOLVIC ?=
ifeq ($(WOLVIC),1)
override LLC := ZhuJiang
endif
# RTL ZhuJiang 下 16 线程触发 UNOPTTHREADS；wolvic 版虽不受限，默认同 8 保持两侧可比
ET      = $(if $(filter ZhuJiang,$(LLC)),8,16)
JOBS   ?= 32
STAMP  := XiangShan/build/.llc-config
WANT   := $(if $(WOLVIC),ZhuJiang-wolvic,$(LLC)) trace=$(strip $(TRACE))
export NOOP_HOME := $(abspath XiangShan)

emu:
	@cur=$$(cat $(STAMP) 2>/dev/null || echo unknown); \
	if [[ -f XiangShan/build/emu && "$$cur" != "$(WANT)" ]]; then \
	  if [[ "$${cur%% *}" != "$(firstword $(WANT))" ]]; then \
	    echo "==> LLC 不匹配（$$cur → $(WANT)），自动 emu-clean"; \
	    $(MAKE) -C XiangShan clean; \
	  else \
	    echo "==> trace 设置变化（$$cur → $(WANT)），删 build/verilator-compile 强制重 verilate"; \
	    rm -rf XiangShan/build/verilator-compile; \
	  fi; \
	fi
ifeq ($(WOLVIC),1)
	cmake -S . -B build
	cmake --build build -j --target wolviczj_dpi
endif
	$(MAKE) -C XiangShan emu CONFIG=DefaultConfig LLC=$(LLC) EMU_THREADS=$(ET) \
	  $(if $(WOLVIC),WOLVIC_ZJ=1 RTL_INCLUDE=$(abspath dpi/sv) \
	    USER_LDFLAGS="$(abspath build/libwolviczj_dpi.a) $(abspath build/libzjmodel.a) $(abspath build/wolvicmod/third_party/libfst/libwolvicmod_fst.a) -lz") \
	  $(if $(strip $(TRACE)),EMU_TRACE=$(strip $(TRACE))) -j$(JOBS)
	@echo "$(WANT)" > $(STAMP)

coremark:
	@[[ -f XiangShan/build/emu ]] || { echo "emu 不存在，先 make emu（可选 LLC=ZhuJiang）"; exit 1; }
	@echo "==> 当前 emu 配置：$$(cat $(STAMP) 2>/dev/null || echo unknown)"
	cd XiangShan && ./build/emu -b 0 -e 0 -i ./ready-to-run/coremark-2-iteration.bin \
	  --diff ./ready-to-run/riscv64-nemu-interpreter-so

emu-clean:
	$(MAKE) -C XiangShan clean

# 把当前 emu 二进制留存到 build/emu-variants/emu-<NAME>（对比实验用；
# 在本仓 build/ 下，XiangShan make clean 碰不到）。用法：make stash-emu NAME=wolvic-1t
stash-emu:
	@[[ -f XiangShan/build/emu ]] || { echo "emu 不存在"; exit 1; }
	@[[ -n "$(NAME)" ]] || { echo "用法：make stash-emu NAME=<变体名>"; exit 1; }
	@mkdir -p build/emu-variants
	cp -L XiangShan/build/emu build/emu-variants/emu-$(NAME)
	@echo "已留存 build/emu-variants/emu-$(NAME)"

# --- coremark 前端 trace 重放（P2 验收）---
N ?= 20000
replay:
	@[[ -f XiangShan/build/emu ]] || { echo "emu 不存在，先 make emu LLC=ZhuJiang TRACE=fst"; exit 1; }
	@grep -q 'trace=fst' $(STAMP) || { echo "当前 emu 未开 trace（$$(cat $(STAMP))），先 make emu LLC=ZhuJiang TRACE=fst"; exit 1; }
	@mkdir -p build/trace
	cd XiangShan && ./build/emu -b 0 -e $(N) -C $(N) --dump-wave \
	  --wave-path ../build/trace/cc_front.fst -i ./ready-to-run/coremark-2-iteration.bin \
	  --diff ./ready-to-run/riscv64-nemu-interpreter-so
	fst2vcd build/trace/cc_front.fst 2>/dev/null | \
	  python3 verify/trace/extract_cc_trace.py --start 0 --end $(N) -o build/trace/cc_front.txt
	cmake -S . -B build && cmake --build build -j --target test_trace_replay
	cd build && ctest -R test_trace_replay --output-on-failure

# --- coremark 全程 trace 重放到 WolvicZjTop 三边界（P4b 验收）---
# emu 全程 dump FST → C++ FST 直读提取器（L2 CHI 缝 + mem/cfg AXI）→ 重放对拍。
# coremark 于 ~31.7 万拍 HIT GOOD TRAP 自然结束；FULLN 仅为防死循环上限。
FULLN ?= 400000
replay-top:
	@[[ -f XiangShan/build/emu ]] || { echo "emu 不存在，先 make emu LLC=ZhuJiang TRACE=fst"; exit 1; }
	@grep -q 'trace=fst' $(STAMP) || { echo "当前 emu 未开 trace（$$(cat $(STAMP))），先 make emu LLC=ZhuJiang TRACE=fst"; exit 1; }
	@mkdir -p build/trace
	cd XiangShan && ./build/emu -b 0 -e $(FULLN) -C $(FULLN) --dump-wave \
	  --wave-path ../build/trace/cm_full.fst -i ./ready-to-run/coremark-2-iteration.bin \
	  --diff ./ready-to-run/riscv64-nemu-interpreter-so
	cmake -S . -B build && cmake --build build -j  # 确保 libwolvicmod_fst.a 存在
	g++ -O2 -std=c++17 -I ../wolvicmod/third_party/libfst/src \
	  verify/trace/extract_top_trace.cpp \
	  build/wolvicmod/third_party/libfst/libwolvicmod_fst.a -lz \
	  -o build/trace/extract_top_trace
	./build/trace/extract_top_trace build/trace/cm_full.fst -o build/trace/cm_full.txt
	cmake --build build -j --target test_wolvic_top_replay	cd build && ctest -R test_wolvic_top_replay --output-on-failure

# --- 共栖 A/B 回放（孤立 RTL vs wolvicmod 行为/性能对比）---
# ZhujiangReplayTop（XiangShan/src/test/scala/top/ZhujiangReplayTop.scala）=
# 与 zhujiang_opt 同配置的 Zhujiang + SocketDevSide/remap，端口即 trace 三边界。
# zj_ab_replay 同一二进制链接 verilated RTL 与 WolvicZjTop 裸模型（无 DPI 包装），
# --dut=rtl|wolvic|both 运行时选择激活侧；两侧均不开波形。solo 用于性能剖析，
# both 用于等价性交叉验证（同进程有缓存互扰）。
# 产物全部在 build/zjrtl/ 下，不碰 XiangShan/build（emu 构建不受影响）。
ZJRTL := build/zjrtl
ABBIN := $(ZJRTL)/zj_ab_replay
VERILATOR_ROOT ?= $(shell verilator --getenv VERILATOR_ROOT)

zjrtl-rtl:
	cd XiangShan && mill -i xiangshan.test.runMain top.ZhujiangReplayTopMain \
	  --config DefaultConfig --issue E.b --num-cores 1 --llc ZhuJiang \
	  --full-stacktrace --target systemverilog --split-verilog \
	  --firtool-opt "-O=release --disable-annotation-unknown --lowering-options=explicitBitcast,disallowLocalVariables,disallowPortDeclSharing,locationInfoStyle=none" \
	  --target-dir $(abspath $(ZJRTL)/rtl)

zjrtl-replay: $(ABBIN)
	$(ABBIN) --dut=$(or $(DUT),both) $(if $(ROWS),--rows=$(ROWS)) \
	  $(if $(PREROLL),--preroll=$(PREROLL)) \
	  $(if $(TRACE_FILE),$(TRACE_FILE),build/trace/cm_full.txt)

# verilate 只产模型归档（--cc，无 --exe）；二进制由下方规则手工链接
$(ZJRTL)/obj/VZhujiangReplayTop__ALL.a: $(ZJRTL)/rtl/ZhujiangReplayTop.sv
	rm -rf $(ZJRTL)/obj
	verilator --cc -O3 --top-module ZhujiangReplayTop \
	  +define+VERILATOR=1 +define+PRINTF_COND=1 \
	  +define+SIM_TOP_MODULE_NAME=ZhujiangReplayTop \
	  -Wno-STMTDLY -Wno-WIDTH -Wno-UNSIGNED -Wno-fatal \
	  --max-num-width 150000 --x-assign unique --no-timing \
	  --output-split 30000 --output-split-cfuncs 30000 \
	  -I$(ZJRTL)/rtl $(ZJRTL)/rtl/*.sv \
	  -Mdir $(ZJRTL)/obj
	$(MAKE) -C $(ZJRTL)/obj -f VZhujiangReplayTop.mk -j$(JOBS) \
	  OPT_FAST="-O3" OPT_SLOW="-O3" VZhujiangReplayTop__ALL.a

# verilator 运行时（单线程无波形：verilated.cpp + verilated_threads.cpp——
# 后者虽只用空线程池，但 verilated.cpp 引用其符号，必须链入）
VRT_OBJS := $(ZJRTL)/obj/verilated.o $(ZJRTL)/obj/verilated_threads.o
$(ZJRTL)/obj/verilated%.o: $(ZJRTL)/obj/VZhujiangReplayTop__ALL.a
	c++ -O3 -I$(VERILATOR_ROOT)/include -I$(VERILATOR_ROOT)/include/vltstd \
	  -c $(VERILATOR_ROOT)/include/verilated$*.cpp -o $@

# 模型库由 cmake 管理：源码变动后本规则不会自动感知，先跑
# `cmake --build build -j --target zjmodel` 再链接（与 emu 流程一致）。
build/libzjmodel.a:
	cmake -S . -B build
	cmake --build build -j --target zjmodel --target wolvicmod_fst

# 链接 A/B 回放器。ab_replay.cpp 的编译 flags 与 zjmodel 库一致
# （AUDIT/FST 宏、-O3），避免框架 inline 代码跨 TU 的 ODR 分歧。
$(ABBIN): verify/zjrtl/ab_replay.cpp verify/zjrtl/trace_io.h \
          $(ZJRTL)/obj/VZhujiangReplayTop__ALL.a $(VRT_OBJS) \
          build/libzjmodel.a
	c++ -O3 -DNDEBUG -std=gnu++20 -DWOLVICMOD_AUDIT=1 -DWOLVICMOD_WITH_FST \
	  -I. -I../wolvicmod/include -I../wolvicmod/third_party \
	  -I../wolvicmod/third_party/libfst/src \
	  -I$(VERILATOR_ROOT)/include -I$(VERILATOR_ROOT)/include/vltstd -I$(ZJRTL)/obj \
	  verify/zjrtl/ab_replay.cpp \
	  $(ZJRTL)/obj/VZhujiangReplayTop__ALL.a $(VRT_OBJS) \
	  build/libzjmodel.a build/wolvicmod/third_party/libfst/libwolvicmod_fst.a \
	  -lz -lpthread -o $@

$(ZJRTL)/rtl/ZhujiangReplayTop.sv:
	$(MAKE) zjrtl-rtl
