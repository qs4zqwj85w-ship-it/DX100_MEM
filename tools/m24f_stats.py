#!/usr/bin/env python3
"""M24-F: structural properties of every trace in the matrix, from the trace itself.

For each trace it measures, offline and independent of any simulator run:

  target stream   accesses, 64 B lines, 4 KiB pages, working-set size,
                  duplicate rates at word and line granularity,
                  reuse within one 16K window (the fraction a bulk window can
                  exploit), sequential-run fraction, DRAM rows touched and the
                  mean run length before the row changes
  index stream    lines, requests at the trace's lane width, share of all lines

Then the derived pressures the report correlates against:

  tlb_pressure    pages / 512  (the L2 TLB reach in 4 KiB pages, m24d config)
  cache_pressure  target working set / 1024 KiB (L2)
  page_spread     lines per page: 1.0 = one line per page, 64 = dense pages
  dup_line        1 - lines/accesses on the target

Usage: m24f_stats.py [--out build/wm/m24f/structure.tsv]
"""
import argparse
import json
import os
import struct
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W = os.path.join(ROOT, "build", "wm")
LINE, PAGE, ROW_BYTES = 64, 4096, 1024
LINES_PER_ROW = ROW_BYTES // LINE
TLB_PAGES = 512          # l2tlb_entries in build/wm/m24d/m24d_bulk.cfg
L2_KIB = 1024            # l2_lines(16384) x 64 B
WIN = 16384              # batch_window, the fused window


def classifier(meta):
    """A stream classifier for the three layout families the traces use.

    A  colidx_base present (M18 SpMV, M21 synth, M23 BFS graphs):
         target = [0, rowptr_base), index = [colidx_base, value|frontier_base)
    B  prop_base / data_base with no colidx (M23 ogbn uniform frontier):
         target = [0, index_base), index = [index_base, ...)
    C  sparse table region (M23 Criteo):
         target = the embedding table (plus the dense table), index = [index_base, ...)
    """
    lay = meta.get("layout") or {}
    ib = lay.get("index_base") or 0
    if "colidx_base" in lay:
        tgt_end = lay.get("rowptr_base") or lay.get("rowptr") or 0
        idx_beg = lay["colidx_base"]
        idx_end = min([v for k, v in lay.items()
                       if k in ("value_base", "frontier_base")
                       and isinstance(v, int) and v > idx_beg] or [1 << 62])

        def cl(addr):
            if addr < tgt_end:
                return "target"
            return "index" if idx_beg <= addr < idx_end else None
        return cl
    if "table_region_start" in lay:
        ts, te = lay["table_region_start"], lay.get("table_region_end", 1 << 62)
        dense = lay.get("dense_bytes", 0)

        def cl(addr):
            if addr >= ib:
                return "index" if (addr >> 6) * 64 < ib + 64 * 64 else None
            if addr < dense or ts <= addr < te:
                return "target"
            return None
        return cl

    def cl(addr):
        return "target" if addr < ib else ("index" if addr < (ib << 1) else None)
    return cl


def stats(path):
    with open(path, "rb") as f:
        if f.read(8) != b"M18TRC1\x00":
            raise SystemExit(f"{path}: not an M18TRC1 trace")
        lanes, elem, ncls, nacc = struct.unpack("<IIQQ", f.read(24))
        cls = f.read(ncls)
        body = f.read()
    meta = json.load(open(path + ".json")) if os.path.exists(path + ".json") else {}
    stream = classifier(meta)
    tgt_end, idx_beg = 1 << 62, 0   # only used by the fast path below

    tgt_lines, tgt_pages, tgt_words = set(), set(), set()
    idx_lines, idx_reqs = set(), 0
    tgt_acc = idx_acc = instrs = 0
    hist = Counter()
    last = {}
    reuse_win = 0
    seq = 0
    rows = set()
    runs, run_len, prev_row = [], 0, None
    prev_line = None
    cur_instr, cur_lines = -1, set()

    for addr, instr, lane in struct.iter_unpack("<QII", body):
        ln = addr >> 6
        if instr != cur_instr:
            if cur_instr >= 0:
                idx_reqs += len(cur_lines)
                instrs += 1
            cur_instr, cur_lines = instr, set()
        s = stream(addr)
        if s == "index":
            cur_lines.add(ln)
            idx_acc += 1
            idx_lines.add(ln)
        if s == "target":
            tgt_acc += 1
            tgt_lines.add(ln)
            tgt_pages.add(addr >> 12)
            tgt_words.add(addr)
            row = ln // LINES_PER_ROW
            rows.add(row)
            if prev_row is None or row != prev_row:
                if run_len:
                    runs.append(run_len)
                run_len = 1
                prev_row = row
            else:
                run_len += 1
            if prev_line is not None and ln == prev_line + 1:
                seq += 1
            prev_line = ln
            prev = last.get(ln)
            if prev is None:
                hist["cold"] += 1
            else:
                d = tgt_acc - 1 - prev
                if d <= 16:
                    hist["1-16"] += 1
                elif d <= 128:
                    hist["17-128"] += 1
                elif d <= 1024:
                    hist["129-1K"] += 1
                elif d <= 8192:
                    hist["1K-8K"] += 1
                elif d <= 65536:
                    hist["8K-64K"] += 1
                else:
                    hist[">64K"] += 1
                if d <= WIN:
                    reuse_win += 1
            last[ln] = tgt_acc - 1
    if run_len:
        runs.append(run_len)
    if cur_instr >= 0:
        idx_reqs += len(cur_lines)
        instrs += 1

    access = nacc
    ws = len(tgt_lines) * LINE
    return dict(
        trace=os.path.basename(path)[:-4], lanes=lanes, elem_bytes=elem,
        accesses=access, instructions=instrs,
        tgt_acc=tgt_acc, tgt_lines=len(tgt_lines), tgt_pages=len(tgt_pages),
        ws_kib=round(ws / 1024, 1),
        dup_word=round(1 - len(tgt_words) / tgt_acc, 4) if tgt_acc else 0,
        dup_line=round(1 - len(tgt_lines) / tgt_acc, 4) if tgt_acc else 0,
        reuse16k=round(reuse_win / tgt_acc, 4) if tgt_acc else 0,
        seq_frac=round(seq / (tgt_acc - 1), 4) if tgt_acc > 1 else 0,
        rows=len(rows),
        row_run=round(sum(runs) / len(runs), 2) if runs else 0,
        acc_per_page=round(tgt_acc / len(tgt_pages), 2) if tgt_pages else 0,
        lines_per_page=round(len(tgt_lines) / len(tgt_pages), 2) if tgt_pages else 0,
        idx_lines=len(idx_lines), idx_reqs=idx_reqs,
        idx_share=round(len(idx_lines) / (len(idx_lines) + len(tgt_lines)), 4)
        if (idx_lines or tgt_lines) else 0,
        tlb_pressure=round(len(tgt_pages) / TLB_PAGES, 2),
        cache_pressure=round(ws / (L2_KIB * 1024), 2),
        reuse_hist=dict(hist),
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(W, "m24f", "structure.tsv"))
    ap.add_argument("--traces", nargs="*",
                    help="ids from build/wm/m24f/traces.tsv (default: all)")
    a = ap.parse_args()

    man = os.path.join(W, "m24f", "traces.tsv")
    rows = []
    with open(man) as f:
        head = next(f)
        for line in f:
            c = line.rstrip("\n").split("\t")
            rows.append(c)
    want = set(a.traces or [])
    cols = ["trace", "lanes", "elem_bytes", "accesses", "instructions", "tgt_acc",
            "tgt_lines", "tgt_pages", "ws_kib", "dup_word", "dup_line", "reuse16k",
            "seq_frac", "rows", "row_run", "acc_per_page", "lines_per_page",
            "idx_lines", "idx_reqs", "idx_share", "tlb_pressure", "cache_pressure"]
    with open(a.out, "w") as f:
        f.write("\t".join(cols) + "\n")
        for c in rows:
            tid, group, rel = c[0], c[1], c[2]
            if want and tid not in want:
                continue
            s = stats(os.path.join(W, rel))
            s["trace"] = tid      # the manifest id, not the file name
            s["group"] = group
            f.write("\t".join(str(s[k]) for k in cols) + "\n")
            print(f"{tid:22s} {group:9s} lines={s['tgt_lines']:>7d} pages={s['tgt_pages']:>6d} "
                  f"ws={s['ws_kib']:>8.1f} KiB dup={s['dup_line']:.3f} seq={s['seq_frac']:.3f} "
                  f"rows={s['rows']:>6d} tlbp={s['tlb_pressure']:>6.2f}", flush=True)
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
