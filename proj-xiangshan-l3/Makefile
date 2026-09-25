# proj-xiangshan-l3 一键入口
#
#   make test                 项目侧单元测试（ctest，逐文件独立条目）
#   make cosim                项目侧预制菜 RTL 对拍全矩阵
#   make cosim M=fastq        单模块对拍（fastq|viparb|qosarb|alloc|spsram|dpsram）
#   make emu [LLC=ZhuJiang]   构建 XiangShan emu（默认 DefaultConfig + OpenLLC）
#   make coremark             用现有 emu 运行 coremark 仿真
#   make emu-clean            手动清除 XiangShan 构建产物
#
# 本仓构建产物全部在 build/ 下（已入 .gitignore）；XiangShan 子模块的产物在
# XiangShan/build/ 下（子模块自身 .gitignore 覆盖）。
#
# emu 配置防呆：XiangShan 的 Makefile 不把 --llc 当构建依赖，切换配置不 clean 会
# 静默复用旧 Verilog。本 Makefile 用 XiangShan/build/.llc-config 印记跟踪当前配置，
# 发现与请求的 LLC 不一致（或未知）时自动先 emu-clean。

M ?=

SHELL := /bin/bash

.PHONY: test cosim emu coremark emu-clean

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
export NOOP_HOME := $(abspath XiangShan)

emu:
	@cur=$$(cat $(STAMP) 2>/dev/null || echo unknown); \
	if [[ -f XiangShan/build/emu && "$$cur" != "$(LLC)" ]]; then \
	  echo "==> emu 配置未知或不匹配（$$cur → $(LLC)），自动 emu-clean"; \
	  $(MAKE) -C XiangShan clean; \
	fi
	$(MAKE) -C XiangShan emu CONFIG=DefaultConfig LLC=$(LLC) EMU_THREADS=$(ET) -j$(JOBS)
	@echo "$(LLC)" > $(STAMP)

coremark:
	@[[ -f XiangShan/build/emu ]] || { echo "emu 不存在，先 make emu（可选 LLC=ZhuJiang）"; exit 1; }
	@echo "==> 当前 emu 配置：$$(cat $(STAMP) 2>/dev/null || echo unknown)"
	cd XiangShan && ./build/emu -b 0 -e 0 -i ./ready-to-run/coremark-2-iteration.bin \
	  --diff ./ready-to-run/riscv64-nemu-interpreter-so

emu-clean:
	$(MAKE) -C XiangShan clean
