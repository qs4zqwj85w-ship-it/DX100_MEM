#!/usr/bin/env python3
"""M23-G4B — how much row locality a *page-major* order can reach at all.

M23-G3 measured two ceilings for a window of distinct lines L:
  unrestricted   (L - C)     / L,  C  = distinct (bank,row) cells over the window
  row hit rate for any ordering of the window.

P4 adds a constraint the brief asked for: the order must be page-major. That
removes the freedom to interleave two different pages, so the only cells an
ordering can consolidate are the ones *inside* one page. The ceiling then becomes

  page-restricted (L - sum_p C_p) / L,   C_p = distinct (bank,row) cells that
                                               page p's lines touch

and the gap between the two numbers is exactly the inter-page row locality that
P2 exploits and P4 cannot express.

Computed from the trace alone, with the address map replicated from
MemorySystem::dram_bank_row and pa_line_of_va. Analysis bound, not a policy.

Usage: python3 tools/m23g4b_page_ceiling.py
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
             ("ogbn_uniform", "build/wm/m23_ogbn_uniform.bin")]
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
            out.append((addr >> 6) + PPN_OFFSET * (4096 // LINE_BYTES))
        return out


def ceilings(lines, chunk):
    """Distinct lines, distinct cells, and the page-restricted cell count."""
    tl = call = cpage = tpages = 0
    for i in range(0, len(lines), chunk):
        part = set(lines[i:i + chunk])
        tl += len(part)
        call += len({bank_row(l) for l in part})
        by_page = {}
        for l in part:
            by_page.setdefault(l >> 6, set()).add(bank_row(l))
        cpage += sum(len(v) for v in by_page.values())
        tpages += len(by_page)
    return tl, call, cpage, tpages


def main():
    print(f"{'workload':13s} {'W':>7s} {'lines':>10s} {'cells':>8s} {'page cells':>11s} "
          f"{'ceil any':>9s} {'ceil page':>10s} {'gap':>7s} {'lines/page':>11s}")
    for wl, path in WORKLOADS:
        p = os.path.join(ROOT, path)
        if not os.path.exists(p):
            print(f"{wl}: trace missing", file=sys.stderr)
            continue
        lines = pa_lines(p)
        for W in WINDOWS:
            tl, call, cpage, tpages = ceilings(lines, W)
            anyc = 100.0 * (tl - call) / tl
            pagec = 100.0 * (tl - cpage) / tl
            print(f"{wl:13s} {W:7d} {tl:10,d} {call:8,d} {cpage:11,d} "
                  f"{anyc:8.2f}% {pagec:9.2f}% {anyc - pagec:6.2f}% "
                  f"{tl / max(1, tpages):11.2f}")
        tl, call, cpage, tpages = ceilings(lines, len(lines))
        anyc = 100.0 * (tl - call) / tl
        pagec = 100.0 * (tl - cpage) / tl
        print(f"{wl:13s} {'whole':>7s} {tl:10,d} {call:8,d} {cpage:11,d} "
              f"{anyc:8.2f}% {pagec:9.2f}% {anyc - pagec:6.2f}% "
              f"{tl / max(1, tpages):11.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
