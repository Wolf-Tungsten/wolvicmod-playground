# proj-xiangshan-l3 一键入口
#
#   make test                 项目侧单元测试（ctest，逐文件独立条目）
#   make cosim                项目侧预制菜 RTL 对拍全矩阵
#   make cosim M=fastq        单模块对拍（fastq|viparb|qosarb|alloc|spsram|dpsram|ring|socket|bridge）
#                             # M=ring/socket/bridge：环级/CC socket/P4a 两桥对拍（直接用
#                             # XiangShan/build/rtl 的产物，须先 emu）
#   make emu [LLC=ZhuJiang]   构建 XiangShan emu（默认 DefaultConfig + OpenLLC）
#           [TRACE=fst]       带波形（EMU_TRACE=fst；fst 之外取值见 difftest/verilator.mk）
#                             trace 标志变化不被 XiangShan make 跟踪，本 Makefile 按
#                             印记检测：仅 trace 变化时删 build/verilator-compile 重 verilate
#   make coremark             用现有 emu 运行 coremark 仿真
#   make replay [N=20000]     coremark 前端 N 拍 trace 重放到 CC 边界：
#                             emu dump FST（需 emu 带 TRACE=fst 构建）→ fst2vcd
#                             → verify/trace/extract_cc_trace.py → ctest -R trace_replay
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

.PHONY: test cosim emu coremark emu-clean replay

test:
	cmake -S . -B build
	cmake --build build -j
	cd build && ctest --output-on-failure

cosim:
	cd verify && ./run.sh $(if $(M),$(M),all)

# --- XiangShan emu ---
LLC    ?= OpenLLC
ET      = $(if $(filter ZhuJiang,$(LLC)),8,16)  # ZhuJiang 下 16 线程会触发 UNOPTTHREADS
JOBS   ?= 32
STAMP  := XiangShan/build/.llc-config
WANT   := $(LLC) trace=$(strip $(TRACE))
export NOOP_HOME := $(abspath XiangShan)

emu:
	@cur=$$(cat $(STAMP) 2>/dev/null || echo unknown); \
	if [[ -f XiangShan/build/emu && "$$cur" != "$(WANT)" ]]; then \
	  if [[ "$${cur%% *}" != "$(LLC)" ]]; then \
	    echo "==> LLC 不匹配（$$cur → $(WANT)），自动 emu-clean"; \
	    $(MAKE) -C XiangShan clean; \
	  else \
	    echo "==> trace 设置变化（$$cur → $(WANT)），删 build/verilator-compile 强制重 verilate"; \
	    rm -rf XiangShan/build/verilator-compile; \
	  fi; \
	fi
	$(MAKE) -C XiangShan emu CONFIG=DefaultConfig LLC=$(LLC) EMU_THREADS=$(ET) \
	  $(if $(strip $(TRACE)),EMU_TRACE=$(strip $(TRACE))) -j$(JOBS)
	@echo "$(WANT)" > $(STAMP)

coremark:
	@[[ -f XiangShan/build/emu ]] || { echo "emu 不存在，先 make emu（可选 LLC=ZhuJiang）"; exit 1; }
	@echo "==> 当前 emu 配置：$$(cat $(STAMP) 2>/dev/null || echo unknown)"
	cd XiangShan && ./build/emu -b 0 -e 0 -i ./ready-to-run/coremark-2-iteration.bin \
	  --diff ./ready-to-run/riscv64-nemu-interpreter-so

emu-clean:
	$(MAKE) -C XiangShan clean

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
	cmake --build build -j --target test_trace_replay
	cd build && ctest -R test_trace_replay --output-on-failure
