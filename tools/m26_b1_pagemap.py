#!/usr/bin/env python3
"""M26-B1: enumerate the accessed pages of the M24-A workload and decode them
exactly as sim/src/memory.cpp does, for the identity map (M0) and the M24-A
physical-page permutation (M3), so the M24-A / M25 disagreement can be settled
on the implementation rather than on either report.

Source of every constant (sim/src/memory.cpp):
  PA_OFFSET_BYTES = 0x4000000            -> ppn = 16384 + frame
  pa_line_of      = (ppn << line_bits) | (off & mask)
  dram_bank_row   : bank = (line/il) ^ >>6 ^ >>12  % banks
                    row  = ((line/(il*banks))/groups) % rows,
                    groups = (row_bytes/64)/il
  m24_permute_page: four-round Feistel over two 10-bit halves, seeded.
Usage: m26_b1_pagemap.py <trace.bin> [--row-bytes N] [--index-base HEX --index-count N]
"""
import struct
import sys

LINE = 64
PA_OFFSET_PAGES = 0x4000000 >> 12  # 16384


def m24_round(x, k):
    x = (x ^ k) & 0xFFFFFFFF
    x = (x * 0x9E3779B1) & 0xFFFFFFFF
    x ^= x >> 15
    x = (x * 0x85EBCA6B) & 0xFFFFFFFF
    x ^= x >> 13
    return x & 1023


def m24_permute_page(p, seed):
    a = (p >> 10) & 1023
    b = p & 1023
    for r in range(4):
        k = (seed * 0x9E3779B9 + r * 0x7F4A7C15) & 0xFFFFFFFF
        na = b
        b = a ^ m24_round(b, k)
        a = na
    return ((a & 1023) << 10) | (b & 1023)


def frame_of(vpn, m24_map, seed):
    """pa_page_of_vpn: the physical page number, before PA_OFFSET is added."""
    if m24_map == 3:
        return m24_permute_page(vpn & ((1 << 20) - 1), seed)
    return vpn


def page_cells(frame, banks, rows, row_bytes, il=1):
    """The set of (bank,row) cells the 64 lines of one page occupy."""
    lines_per_row = max(1, row_bytes // LINE)
    groups = max(1, lines_per_row // il)
    out = set()
    base = (PA_OFFSET_PAGES + frame) * (4096 // LINE)
    for off in range(4096 // LINE):
        line = base + off
        bidx = line // il
        bidx ^= (bidx >> 6) ^ (bidx >> 12)
        bank = bidx % banks
        row = ((line // (il * banks)) // groups) % rows
        out.add((bank, row))
    return out


def read_trace(path):
    with open(path, "rb") as f:
        magic = f.read(8)
        if magic[:7] != b"M18TRC1":
            raise SystemExit(f"{path}: not an M18TRC1 trace")
        lanes, elem = struct.unpack("<II", f.read(8))
        n_instr, n_acc = struct.unpack("<QQ", f.read(16))
        f.read(n_instr)
        blob = f.read(n_acc * 16)
    pages = set()
    for i in range(n_acc):
        addr = struct.unpack_from("<Q", blob, i * 16)[0]
        pages.add(addr >> 12)
    return pages, n_acc, lanes, elem


def analyse(name, pages, banks=2, rows=4096, row_bytes=1024):
    """Row-sharing structure of an accessed page set under one geometry."""
    row_pages = {}   # (bank,row) -> set of pages
    for p in pages:
        for c in page_cells(p, banks, rows, row_bytes):
            row_pages.setdefault(c, set()).add(p)
    shared = {c: s for c, s in row_pages.items() if len(s) > 1}
    pairs = sum(len(s) * (len(s) - 1) // 2 for s in row_pages.values())
    maxshare = max((len(s) for s in row_pages.values()), default=0)
    print(f"  {name:26s} pages={len(pages):6d} cells={len(row_pages):6d} "
          f"shared_cells={len(shared):5d} page_pairs={pairs:5d} max_pages_per_cell={maxshare}")
    return len(row_pages), len(shared), pairs


def main():
    path = sys.argv[1]
    row_bytes = 1024
    index_base = None
    index_count = 0
    i = 2
    while i < len(sys.argv):
        if sys.argv[i] == "--row-bytes":
            row_bytes = int(sys.argv[i + 1]); i += 2
        elif sys.argv[i] == "--index-base":
            index_base = int(sys.argv[i + 1], 0); i += 2
        elif sys.argv[i] == "--index-count":
            index_count = int(sys.argv[i + 1]); i += 2
        else:
            raise SystemExit("bad argument " + sys.argv[i])

    pages, n_acc, lanes, elem = read_trace(path)
    print(f"trace {path}: accesses={n_acc} lanes={lanes} elem_bytes={elem} "
          f"distinct_pages={len(pages)}")
    lo, hi = min(pages), max(pages)
    print(f"  page range [{lo}, {hi}], span {hi - lo + 1} pages "
          f"({(hi - lo + 1) * 4096 / 2**20:.1f} MiB)")

    # Geometry, stated exactly as the decoder uses it.
    lines_per_row = row_bytes // LINE
    print(f"geometry row_bytes={row_bytes} banks=2 rows=4096 il=1 -> "
          f"lines_per_row={lines_per_row} address span per row index="
          f"{lines_per_row * 2 * LINE} B, page={4096 // LINE} lines "
          f"= {4096 // (lines_per_row * 2 * LINE)} row index(es) per page, "
          f"frame slots before wrap = 2^20 frames mod {2 * (4096)} -> "
          f"{4096 * 2 * row_bytes // 4096} pages of model capacity")

    variants = [("trace", set(pages))]
    if index_base is not None and index_count:
        ip = set(pages)
        first = index_base >> 12
        last = (index_base + index_count * 4) >> 12
        ip.update(range(first, last + 1))
        variants.append(("trace+engine index", ip))

    for vname, vpages in variants:
        print(f"\n== {vname}: {len(vpages)} pages, row_bytes={row_bytes}")
        for m24_map, seed in ((0, 1), (3, 1), (3, 2), (3, 3)):
            frames = {frame_of(p, m24_map, seed) for p in vpages}
            print(f" -- m24_map={m24_map} seed={seed}: distinct frames={len(frames)} "
                  f"(dup frames={len(vpages) - len(frames)})")
            rows_by_page = {p: page_cells(frame_of(p, m24_map, seed), 2, 4096, row_bytes)
                            for p in vpages}
            row_pages = {}
            for p, cs in rows_by_page.items():
                for c in cs:
                    row_pages.setdefault(c, set()).add(p)
            shared = {c: s for c, s in row_pages.items() if len(s) > 1}
            pairs = sum(len(s) * (len(s) - 1) // 2 for s in row_pages.values())
            maxshare = max((len(s) for s in row_pages.values()), default=0)
            print(f"    cells={len(row_pages):6d} shared_cells={len(shared):5d} "
                  f"page_pairs={pairs:5d} max_pages_per_cell={maxshare}")

    # The specific question M24-A asked: can two 4 KiB pages share one row?
    print("\n== adjacency question (row_bytes=1024): does page p share a cell "
          "with page p+1?")
    for p in sorted(pages)[:200]:
        a = page_cells(frame_of(p, 0, 1), 2, 4096, row_bytes)
        b = page_cells(frame_of(p + 1, 0, 1), 2, 4096, row_bytes)
        if a & b:
            print(f"    pages {p} and {p+1} share {a & b}")
            break
    else:
        print("    no adjacent pair among the first 200 accessed pages shares a cell")


if __name__ == "__main__":
    main()
