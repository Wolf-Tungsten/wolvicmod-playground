#!/usr/bin/env python3
# extract_cc_trace.py：从 emu FST 提取 CC 边界（L2 CHI 缝 + CC socket 环侧）
# 的逐拍 trace，供 tests/test_trace_replay.cpp 重放对拍。
#
# 用法：
#   fst2vcd wave.fst | extract_cc_trace.py --start N --end M -o trace.txt
#   extract_cc_trace.py wave.vcd --start N --end M -o trace.txt   # 直接读 VCD
#
# 提取的信号（两前缀下、匹配通道模式的全部叶子信号）：
#   TOP.SimTop.cpu.l_soc.core_with_l2.io_decoupledCHI_{tx_req,tx_rsp,tx_dat,
#     rx_rsp,rx_dat,rx_snp}_{valid,ready,bits_*}        → 列名 l2.<chan>.<field>
#   TOP.SimTop.cpu.l_soc.zhujiang_opt.ccn_0_0x8.io_dev_{tx_req,tx_resp,tx_data,
#     rx_req,rx_resp,rx_data,rx_snoop}_{valid,ready,bits_*} → 列名 ring.<chan>.<field>
#
# 输出：首行列名（空格分隔），随后每拍一行：cyc 后接各列十六进制值。
# FST 时间戳 = 拍号（--dump-wave 每拍 clk=1 dump 一次）。复位未撤除的拍被
# 跳过（stderr 告警一次）；窗口前若有通道 valid 流量同样告警（模型初态无法对齐）。
# 结束时向 stderr 打印每通道 fire（valid&&ready）统计，供验收报告。

import argparse
import re
import sys

L2_PREFIX = "TOP.SimTop.cpu.l_soc.core_with_l2.io_decoupledCHI_"
RING_PREFIX = "TOP.SimTop.cpu.l_soc.zhujiang_opt.ccn_0_0x8.io_dev_"

L2_CHANS = ("tx_req", "tx_rsp", "tx_dat", "rx_rsp", "rx_dat", "rx_snp")
RING_CHANS = ("tx_req", "tx_resp", "tx_data", "rx_req", "rx_resp", "rx_data", "rx_snoop")

# 复位信号（仅用于检查，不输出为列）
RESET_PATHS = (
    "TOP.SimTop.cpu.l_soc.core_with_l2.reset",
    "TOP.SimTop.cpu.l_soc.zhujiang_opt.ccn_0_0x8.reset",
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vcd", nargs="?", default="-", help="输入 VCD（缺省/`-` = stdin）")
    ap.add_argument("--start", type=int, default=0, help="起始拍（含）")
    ap.add_argument("--end", type=int, default=0, help="结束拍（不含）；0 = 到末尾")
    ap.add_argument("-o", "--output", default="-", help="输出文件（缺省 = stdout）")
    args = ap.parse_args()

    fin = sys.stdin if args.vcd in (None, "-") else open(args.vcd, "r", buffering=1024 * 1024)
    fout = sys.stdout if args.output == "-" else open(args.output, "w", buffering=1024 * 1024)

    id2col = {}             # vcd id → 列名列表（>64b 信号拆成多个 64b 块）
    id_seen = set()
    id_width = {}           # vcd id → 位宽
    col_names = []
    reset_ids = {}          # vcd id → reset 路径名
    scope_stack = []

    def register(path, vid, width):
        if path.startswith(L2_PREFIX):
            leaf, side, chans = path[len(L2_PREFIX):], "l2", L2_CHANS
        elif path.startswith(RING_PREFIX):
            leaf, side, chans = path[len(RING_PREFIX):], "ring", RING_CHANS
        else:
            return
        m = re.match(r"^(" + "|".join(chans) + r")_(valid|ready|bits_.+)$", leaf)
        if not m or vid in id_seen:
            return
        id_seen.add(vid)
        base = f"{side}.{m.group(1)}.{m.group(2)}"
        nchunk = (width + 63) // 64
        cols = [base] if nchunk == 1 else [f"{base}_{i}" for i in range(nchunk)]
        id2col[vid] = cols
        id_width[vid] = width
        col_names.extend(cols)

    # ---- 头部解析 ----
    for line in fin:
        if line.startswith("$enddefinitions"):
            break
        if line.startswith("$scope"):
            scope_stack.append(line.split()[2])
        elif line.startswith("$upscope"):
            scope_stack.pop()
        elif line.startswith("$var"):
            parts = line.split()
            width, vid, name = int(parts[2]), parts[3], parts[4]
            path = ".".join(scope_stack) + "." + name
            if path in RESET_PATHS:
                reset_ids[vid] = path
            register(path, vid, width)

    n_cols = len(col_names)
    col_idx = {c: i for i, c in enumerate(col_names)}
    print(f"# {n_cols} signal columns, resets watched: {len(reset_ids)}", file=sys.stderr)
    if n_cols == 0:
        sys.exit("no boundary signals found — hierarchy path mismatch?")
    for side, chans in (("l2", L2_CHANS), ("ring", RING_CHANS)):
        for ch in chans:
            for f in ("valid", "ready"):
                if f"{side}.{ch}.{f}" not in col_idx:
                    print(f"# warn: missing column {side}.{ch}.{f}", file=sys.stderr)

    fout.write("cyc")
    for c in col_names:
        fout.write(" " + c)
    fout.write("\n")

    vals = [0] * n_cols     # 当前各列值（int）
    resets = {v: 1 for v in reset_ids.values()}
    valid_i = {f"{s}.{c}": col_idx[f"{s}.{c}.valid"] for s, cs in (("l2", L2_CHANS), ("ring", RING_CHANS)) for c in cs if f"{s}.{c}.valid" in col_idx}
    ready_i = {f"{s}.{c}": col_idx[f"{s}.{c}.ready"] for s, cs in (("l2", L2_CHANS), ("ring", RING_CHANS)) for c in cs if f"{s}.{c}.ready" in col_idx}
    fire = {c: 0 for c in valid_i}
    first_valid_cyc = {}
    emitted = 0
    pending_t = None
    x_warn = 0
    reset_note_done = False

    def emit(cyc):
        nonlocal emitted
        fout.write(str(cyc))
        for v in vals:
            fout.write(f" {v:x}")
        fout.write("\n")
        emitted += 1
        for c, i in valid_i.items():
            if vals[i]:
                if c not in first_valid_cyc:
                    first_valid_cyc[c] = cyc
                if c in ready_i and vals[ready_i[c]]:
                    fire[c] += 1

    for line in fin:
        c0 = line[0]
        if c0 == "#":
            t = int(line[1:])
            if pending_t is not None:
                for cyc in range(max(pending_t, args.start), t):
                    if args.end and cyc >= args.end:
                        break
                    if any(resets.values()):
                        if not reset_note_done:
                            print(f"# note: rows skipped while reset active (first at cyc {cyc})",
                                  file=sys.stderr)
                            reset_note_done = True
                        continue
                    emit(cyc)
            pending_t = t
            if args.end and t >= args.end:
                break
            continue
        if c0 in "01xXzZ":
            vid = line[1:].strip()
            v = 1 if c0 == "1" else 0
            if c0 in "xXzZ":
                x_warn += 1
            if vid in id2col:
                vals[col_idx[id2col[vid][0]]] = v
            elif vid in reset_ids:
                resets[reset_ids[vid]] = v
            continue
        if c0 == "b":
            payload, vid = line[1:].split()
            if vid in id2col:
                if re.search(r"[xXzZ]", payload):
                    x_warn += 1
                    payload = re.sub(r"[xXzZ]", "0", payload)
                big = int(payload, 2)
                for i, col in enumerate(id2col[vid]):
                    vals[col_idx[col]] = (big >> (64 * i)) & 0xFFFFFFFFFFFFFFFF
            continue

    fout.close()
    print(f"# emitted {emitted} rows (start={args.start} end={args.end or 'EOF'})", file=sys.stderr)
    if x_warn:
        print(f"# warn: {x_warn} x/z values coerced to 0", file=sys.stderr)
    print("# channel fires (valid&&ready) / first valid cyc:", file=sys.stderr)
    for c in sorted(valid_i):
        print(f"#   {c:16s} fires={fire[c]:6d} first_valid={first_valid_cyc.get(c, '-')}",
              file=sys.stderr)


if __name__ == "__main__":
    main()
