#!/usr/bin/env python3
"""M23-G3 — the row-locality ceiling available inside a finite window.

For a window that contains a set of distinct cache lines, the best possible
row-hit rate for *any* ordering of that window is (L - C) / L, where L is the
number of distinct lines and C the number of distinct (bank, row) cells they
touch: one activation per cell, every other line of that cell hits the open row.
That is the ceiling a perfect organizer with unbounded reorder space inside the
window could reach. It says nothing about windows that do not exist yet.

Computed from the trace alone (address map replicated exactly from
MemorySystem::dram_bank_row and pa_line_of_va), so it is an analysis bound, not a
simulated policy. The per-window distinct-line totals are also the simulator's
bulk_lines_issued, which is how the chunking is validated.

Usage: python3 tools/m23g3_ceiling.py
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BANKS, ROWS, ROW_BYTES = 2, 4096, 1024
LINE_BYTES = 64
PPN_OFFSET = 0x00004000
LINES_PER_ROW = ROW_BYTES // LINE_BYTES
BANK_HASH = 1

WORKLOADS = [("orkut", "build/wm/m23_orkut_bfs.bin"),
             ("ogbn_uniform", "build/wm/m23_ogbn_uniform.bin"),
             ("criteo", "build/wm/m23_criteo_16k.bin")]
WINDOWS = [8192, 16384, 32768]


def bank_row(pa_line):
    bidx = pa_line
    if BANK_HASH:
        bidx ^= (pa_line >> 6) ^ (pa_line >> 12)
    return bidx % BANKS, ((pa_line // BANKS) // LINES_PER_ROW) % ROWS


def pa_lines(path):
    """PA line number of every Bulk-class access, in trace order."""
    with open(path, "rb") as f:
        magic, lanes, elem_bytes, n_instr, n_acc = struct.unpack("<8sIIQQ", f.read(32))
        assert magic[:7] == b"M18TRC1", magic
        cls = f.read(n_instr)
        out = []
        for _ in range(n_acc):
            addr, instr, lane = struct.unpack("<QII", f.read(16))
            if cls[instr] != 1:      # 0 = SIMT, 1 = Bulk
                continue
            # pa_line_of_va: ppn = vpn + PPN_OFFSET, offset within the page kept
            out.append((addr >> 6) + PPN_OFFSET * (4096 // LINE_BYTES))
        return out


def ceiling(lines, chunk):
    """(sum distinct lines, sum distinct cells) over consecutive chunks."""
    tl = tc = 0
    for i in range(0, len(lines), chunk):
        part = lines[i:i + chunk]
        tl += len(set(part))
        tc += len({bank_row(l) for l in part})
    return tl, tc


def main():
    print(f"{'workload':13s} {'chunk':>7s} {'distinct lines':>15s} {'cells':>7s} "
          f"{'ceiling hit%':>13s} {'offline hit%':>13s}")
    for wl, path in WORKLOADS:
        p = os.path.join(ROOT, path)
        if not os.path.exists(p):
            print(f"{wl}: trace missing", file=sys.stderr)
            continue
        lines = pa_lines(p)
        tl, tc = ceiling(lines, len(lines))
        off = 100.0 * (tl - tc) / tl
        for W in WINDOWS:
            tl, tc = ceiling(lines, W)
            print(f"{wl:13s} {W:7d} {tl:15,d} {tc:7,d} "
                  f"{100.0 * (tl - tc) / tl:12.2f}% {off:12.2f}%")
        print(f"{wl:13s} {'whole':>7s} {len(set(lines)):15,d} "
              f"{len({bank_row(l) for l in lines}):7,d} {off:12.2f}% {off:12.2f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
