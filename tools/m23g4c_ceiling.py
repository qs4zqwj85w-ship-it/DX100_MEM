#!/usr/bin/env python3
"""M23-G4C — DRAM mapping audit: address distribution and window ceilings.

For each mapping this prints

  * the address distribution over the trace: distinct lines, footprint span in
    MiB and in 8 MiB periods, lines and rows per bank, lines per touched row,
    the busiest row's share, and the per-bank line imbalance;
  * the window-local row ceiling (L - C) / L for each G3 window, where L is the
    distinct lines a window's elements touch and C the distinct (bank,row) cells
    they map to. That is the best row-hit rate *any* ordering of the window can
    reach under this mapping -- pure analysis, computed from the trace.

The mapping is replicated exactly from MemorySystem::dram_bank_row; with
dram_il_lines = 1 it reproduces M23-G3's tool to the digit (Orkut whole-trace
91.84 %, ogbn 98.85 %), which is what validates the replication.

Writes build/wm/m23g4c/ceilings.csv and prints a Markdown-ready table.

Usage: python3 tools/m23g4c_ceiling.py
"""
import csv
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g4c")
LINE_BYTES = 64
PPN_OFFSET = 0x00004000
BANK_HASH = True

# name, row_bytes, rows, il  -- all 2 banks x 8 MiB
MAPS = [("m0", 1024, 4096, 1), ("m1", 512, 8192, 1),
        ("m2", 2048, 2048, 1), ("m3", 1024, 4096, 4)]
LABEL = {"m0": "M0 production 16 lines/row, il 1",
         "m1": "M1 fine rows 8 lines/row, il 1",
         "m2": "M2 coarse rows 32 lines/row, il 1",
         "m3": "M3 16 lines/row, 4-line bank interleave"}
WORKLOADS = [("orkut", "Orkut BFS", "build/wm/m23_orkut_bfs.bin"),
             ("ogbn_uniform", "ogbn-products uniform", "build/wm/m23_ogbn_uniform.bin")]
WINDOWS = [8192, 16384, 32768]
BANKS = 2


def bank_row(pa_line, row_bytes, rows, il):
    lpr = row_bytes // LINE_BYTES
    bidx = pa_line // il
    if BANK_HASH:
        bidx ^= (bidx >> 6) ^ (bidx >> 12)
    bank = bidx % BANKS
    row = ((pa_line // (il * BANKS)) // (lpr // il)) % rows
    return bank, row


def pa_lines(path):
    with open(path, "rb") as f:
        magic, lanes, elem_bytes, n_instr, n_acc = struct.unpack("<8sIIQQ", f.read(32))
        assert magic[:7] == b"M18TRC1", magic
        cls = f.read(n_instr)
        out = []
        for _ in range(n_acc):
            addr, instr, lane = struct.unpack("<QII", f.read(16))
            if cls[instr] != 1:
                continue
            out.append((addr >> 6) + PPN_OFFSET * (4096 // LINE_BYTES))
        return out


def distribution(lines, row_bytes, rows, il):
    uniq = set(lines)
    banks = [0, 0]
    rowcnt = {}
    for l in uniq:
        b, r = bank_row(l, row_bytes, rows, il)
        banks[b] += 1
        rowcnt[(b, r)] = rowcnt.get((b, r), 0) + 1
    span_lines = max(uniq) - min(uniq) + 1
    cap_lines = BANKS * rows * (row_bytes // LINE_BYTES)
    return {
        "distinct_lines": len(uniq),
        "span_mib": round(span_lines * LINE_BYTES / (1 << 20), 2),
        "periods": round(span_lines / cap_lines, 2),
        "bank0_lines": banks[0],
        "bank1_lines": banks[1],
        "bank_imbalance_pct": round(100.0 * abs(banks[0] - banks[1]) / max(1, len(uniq)), 2),
        "rows_touched": len(rowcnt),
        "rows_total": rows * BANKS,
        "row_coverage_pct": round(100.0 * len(rowcnt) / (rows * BANKS), 2),
        "lines_per_row": round(len(uniq) / max(1, len(rowcnt)), 3),
        "max_lines_in_row": max(rowcnt.values()) if rowcnt else 0,
    }


def ceiling(lines, chunk, row_bytes, rows, il):
    tl = tc = 0
    for i in range(0, len(lines), chunk):
        part = set(lines[i:i + chunk])
        tl += len(part)
        tc += len({bank_row(l, row_bytes, rows, il) for l in part})
    return tl, tc


def main():
    os.makedirs(OUT, exist_ok=True)
    rows_csv = []
    print(f"{'map':4s} {'workload':13s} {'W':>7s} {'lines':>10s} {'cells':>9s} {'ceiling':>8s}")
    for m, rb, rr, il in MAPS:
        for wl, desc, path in WORKLOADS:
            p = os.path.join(ROOT, path)
            lines = pa_lines(p)
            for W in WINDOWS:
                tl, tc = ceiling(lines, W, rb, rr, il)
                ceil = 100.0 * (tl - tc) / tl
                rows_csv.append({"mapping": m, "label": LABEL[m], "workload": wl,
                                 "window": W, "lines": tl, "cells": tc,
                                 "ceiling_pct": round(ceil, 2)})
                print(f"{m:4s} {wl:13s} {W:7d} {tl:10,d} {tc:9,d} {ceil:7.2f}%")

    print()
    print("### Address distribution over the whole trace\n")
    print("| Map | Workload | Distinct lines | Span | Periods | Bank 0 / Bank 1 | Imbalance | "
          "Rows touched | Coverage | Lines/row | Busiest row |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for m, rb, rr, il in MAPS:
        for wl, desc, path in WORKLOADS:
            d = distribution(pa_lines(os.path.join(ROOT, path)), rb, rr, il)
            print("| {} | {} | {:,} | {:.2f} MiB | {:.2f} | {:,} / {:,} | {:.2f}% | {:,} of {:,} "
                  "({:.1f}%) | {:.3f} | {} |".format(
                      m, desc, d["distinct_lines"], d["span_mib"], d["periods"],
                      d["bank0_lines"], d["bank1_lines"], d["bank_imbalance_pct"],
                      d["rows_touched"], d["rows_total"], d["row_coverage_pct"],
                      d["lines_per_row"], d["max_lines_in_row"]))
            # attach the distribution to every window row of this mapping/workload
            for r in rows_csv:
                if r["mapping"] == m and r["workload"] == wl:
                    r.update({k: v for k, v in d.items()})

    with open(os.path.join(OUT, "ceilings.csv"), "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(rows_csv[0].keys()))
        wr.writeheader()
        wr.writerows(rows_csv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
