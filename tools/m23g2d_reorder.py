#!/usr/bin/env python3
"""M23-G2D — build reordered copies of an M18TRC1 trace (request-order diagnostic).

This tool writes a *new* trace file. It never edits the input, never touches the
simulator, and never changes an address or a cache-line size.

Modes
-----
  orig      verbatim copy (byte-identical to the input; the control that proves
            the tool is a no-op)
  reformed  original request order, instructions re-formed into 4-access groups
            of distinct lines (control that isolates re-formation from
            reordering)
  page      requests grouped by virtual page number  (sort key: page, line,
            address) -- then re-formed
  row       requests grouped by modeled physical (bank, row)  (sort key: row,
            line, address) -- then re-formed

Why re-formation is needed, and why it is legal here
----------------------------------------------------
The trace groups accesses into memory instructions of at most `lanes` (4)
accesses, and the SIMT coalescer deduplicates addresses that fall in the same
64 B line *inside one instruction*.  In both M23 traces every access lands in
its own line (`requests_generated == accesses` in the M23-G2C logs: 1 200 003
lines from 1 200 003 accesses, i.e. no instruction contains two accesses to the
same line).  Re-forming the accesses into groups that also contain no two
accesses to the same line therefore preserves the coalesced request count
exactly -- it removes nothing, because there is nothing to remove.

The grouping below guarantees that property by construction: an access is only
appended to a group whose line set does not already contain its line.  The
predicted request count is therefore `n_acc`, and the runner/extractor check it
against the simulator's own `requests_generated`.

Address map (must match sim/src/memory.cpp l2_probe)
----------------------------------------------------
  bidx = line ^ (line >> 6) ^ (line >> 12)        (bank_hash = 1)
  bank = bidx % banks
  row  = ((line / banks) / (row_bytes / 64)) % rows
Under ideal_translation the DRAM line is `va_line + PPN_OFFSET * 64` (memory.cpp
pa_line_of, tlb_fill).  That constant is 0x4000 pages = 2^20 lines; it changes
neither bank (2^14 and 2^8 land on even bit positions of the XOR fold) nor row
(2^15 mod 4096 = 0).  `--check-pa` verifies this on the actual data instead of
assuming it.

Usage
  m23g2d_reorder.py --trace build/wm/m23_orkut_bfs.bin --mode row \
      --out build/wm/m23g2d/traces/orkut_row.bin
"""
import argparse
import hashlib
import json
import os
import struct
import sys

import numpy as np

MAGIC = b"M18TRC1\x00"
LINE = 64
PAGE_LINES = 64            # 4 KiB page / 64 B line
PPN_OFFSET = 0x00004000    # memory.cpp:19


# --------------------------------------------------------------------------- #
# trace I/O
# --------------------------------------------------------------------------- #
def read_trace(path):
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:8] != MAGIC:
        raise SystemExit(f"{path}: not an M18TRC1 trace")
    lanes, elem, n_instr, n_acc = struct.unpack_from("<IIQQ", blob, 8)
    off = 32          # 8 (magic) + 4 + 4 + 8 + 8, as build_trace_file reads it
    cls = np.frombuffer(blob, dtype=np.uint8, count=n_instr, offset=off)
    off += n_instr
    rec = np.frombuffer(blob, dtype=np.dtype([("addr", "<u8"),
                                              ("instr", "<u4"),
                                              ("lane", "<u4")]),
                        count=n_acc, offset=off)
    if off + n_acc * 16 != len(blob):
        raise SystemExit(f"{path}: trailing bytes ({len(blob)} != {off + n_acc * 16})")
    return lanes, elem, n_instr, n_acc, cls, rec


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# --------------------------------------------------------------------------- #
# address map
# --------------------------------------------------------------------------- #
def bank_row(line, banks, rows, lines_per_row, bank_hash, pa=False):
    x = line + (PPN_OFFSET * PAGE_LINES if pa else 0)
    bidx = x
    if bank_hash:
        bidx = x ^ (x >> 6) ^ (x >> 12)
    b = bidx % banks
    r = ((x // banks) // lines_per_row) % rows
    return b, r


# --------------------------------------------------------------------------- #
# grouping: <=lanes accesses per instruction, all on distinct lines
# --------------------------------------------------------------------------- #
def assign_groups(order_line, lanes):
    """Return (gid, lane) arrays: instruction id and lane index per position.

    Greedy, in stream order: an access joins the earliest open instruction that
    has room and does not already hold its line.  Because the input is sorted,
    that instruction is (almost always) the current one, so the grouping keeps
    the sorted order intact.
    """
    n = order_line.size
    gid = np.empty(n, dtype=np.uint32)
    lane = np.empty(n, dtype=np.uint8)
    active = {}          # gid -> list of lines, append order = lane index
    gptr = 0
    N = int
    for pos in range(n):
        ln = N(order_line[pos])
        j = gptr
        gl = active.get(j)
        if gl is None:
            gl = active[j] = []
        while len(gl) >= lanes or ln in gl:
            j += 1
            gl = active.get(j)
            if gl is None:
                gl = active[j] = []
        gid[pos] = j
        lane[pos] = len(gl)
        gl.append(ln)
        while True:
            g2 = active.get(gptr)
            if g2 is not None and len(g2) >= lanes:
                del active[gptr]
                gptr += 1
            else:
                break
    return gid, lane


# --------------------------------------------------------------------------- #
# stream-order row predictor (same idea as M23-G0's predicted_row_hit_rate)
# --------------------------------------------------------------------------- #
def predict_hits(bank, row, banks):
    """Per-bank last-row replay: assume the stream is served in order."""
    last = [-1] * banks
    hit = 0
    for i in range(bank.size):
        b = int(bank[i])
        if last[b] == int(row[i]):
            hit += 1
        last[b] = int(row[i])
    return hit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", required=True)
    ap.add_argument("--mode", required=True,
                    choices=["orig", "reformed", "page", "row"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--lanes", type=int, default=4)
    ap.add_argument("--banks", type=int, default=2)
    ap.add_argument("--rows", type=int, default=4096)
    ap.add_argument("--row-bytes", type=int, default=1024)
    ap.add_argument("--bank-hash", type=int, default=1)
    ap.add_argument("--check-pa", action="store_true",
                    help="assert the PA offset leaves bank and row unchanged")
    a = ap.parse_args()

    lanes, elem, n_instr, n_acc, cls, rec = read_trace(a.trace)
    lines_per_row = max(1, a.row_bytes // LINE)

    addr = np.ascontiguousarray(rec["addr"])
    line = addr // LINE
    orig_instr = np.ascontiguousarray(rec["instr"])
    orig_lane = np.ascontiguousarray(rec["lane"])

    # ---- address-multiset fingerprint (identical in every mode) -------------
    addr_sorted_sha = hashlib.sha256(np.sort(addr).tobytes()).hexdigest()
    line_sorted_sha = hashlib.sha256(np.sort(line).tobytes()).hexdigest()

    # ---- map ---------------------------------------------------------------
    x = line
    bidx = x ^ (x >> 6) ^ (x >> 12) if a.bank_hash else x
    bank = (bidx % a.banks).astype(np.uint32)
    row = (((x // a.banks) // lines_per_row) % a.rows).astype(np.uint32)
    if a.check_pa:
        y = line + PPN_OFFSET * PAGE_LINES
        bidx2 = y ^ (y >> 6) ^ (y >> 12) if a.bank_hash else y
        bank2 = bidx2 % a.banks
        row2 = ((y // a.banks) // lines_per_row) % a.rows
        if not (np.array_equal(bank, bank2) and np.array_equal(row, row2)):
            raise SystemExit("PA offset changes bank or row: offline map is wrong")

    # intra-instruction line duplication in the *original* trace: if any
    # instruction holds two accesses to one line, re-formation could dedup and
    # the request count would move.  Measure it rather than assume it.
    key = orig_instr.astype(np.int64) * (1 << 32) + line.astype(np.int64)
    uniq = np.unique(key).size
    orig_dedup = int(n_acc - uniq)

    # ---- order -------------------------------------------------------------
    page = (line >> 6).astype(np.uint64)
    idx = np.arange(n_acc, dtype=np.uint64)
    if a.mode == "page":
        order = np.lexsort((idx, addr, line, page))
    elif a.mode == "row":
        order = np.lexsort((idx, addr, line, row))
    else:
        order = idx

    if a.mode == "orig":
        out = a.out
        os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
        with open(a.trace, "rb") as f, open(out, "wb") as g:
            g.write(f.read())
        info = {"mode": "orig", "note": "verbatim copy"}
    else:
        gid, lane_idx = assign_groups(np.ascontiguousarray(line[order]), a.lanes)
        # emission order: instruction id, then lane (append order)
        emit = np.lexsort((lane_idx, gid))
        e_addr = addr[order][emit]
        e_gid = gid[emit].astype(np.uint32)
        e_lane = lane_idx[emit].astype(np.uint32)
        n_out = int(e_gid[-1]) + 1 if e_gid.size else 0
        # per-instruction distinct-line count == per-instruction access count
        # by construction; assert it so the count claim is checked, not argued.
        cnt = np.bincount(e_gid, minlength=n_out)
        if int(cnt.max()) > a.lanes:
            raise SystemExit("group larger than lanes")
        if int(cnt.sum()) != n_acc:
            raise SystemExit("grouping lost accesses")

        out = a.out
        os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
        cls_out = bytes(n_out)          # every instruction SIMT: dispatch = 0 in
                                        # the frozen config, so the class is inert
        with open(out, "wb") as f:
            f.write(MAGIC)
            f.write(struct.pack("<IIQQ", lanes, elem, n_out, n_acc))
            f.write(cls_out)
            buf = np.empty(n_acc, dtype=np.dtype([("addr", "<u8"),
                                                  ("instr", "<u4"),
                                                  ("lane", "<u4")]))
            buf["addr"] = e_addr
            buf["instr"] = e_gid
            buf["lane"] = e_lane
            f.write(buf.tobytes())
        info = {"mode": a.mode, "instructions": n_out,
                "instr_per_access": round(n_out / n_acc, 6)}

    # ---- verification record ----------------------------------------------
    ord_lines = line[order]
    ob = bidx[order] if False else None
    o_bank = (ord_lines ^ (ord_lines >> 6) ^ (ord_lines >> 12)) % a.banks
    o_row = (((ord_lines // a.banks) // lines_per_row) % a.rows)
    pred_hits = predict_hits(o_bank.astype(np.int64), o_row.astype(np.int64), a.banks)
    same_cell_adj = int((np.diff(o_bank * a.rows + o_row) == 0).sum()) if n_acc > 1 else 0

    info.update({
        "trace_in": os.path.abspath(a.trace),
        "trace_out": os.path.abspath(out),
        "trace_in_sha256": sha256_file(a.trace),
        "trace_out_sha256": sha256_file(out),
        "lanes": lanes, "elem_bytes": elem,
        "accesses": int(n_acc),
        "instr_in": int(n_instr),
        "unique_lines": int(np.unique(line).size),
        "unique_pages": int(np.unique(page).size),
        "unique_rows": int(np.unique(row).size),
        "unique_cells": int(np.unique(bank.astype(np.int64) * a.rows + row).size),
        "address_multi_sha256": addr_sorted_sha,
        "line_multi_sha256": line_sorted_sha,
        "orig_intra_instr_dedup": orig_dedup,
        "orig_requests_generated": int(n_acc - orig_dedup),
        "predicted_requests_generated": int(n_acc),
        "stream_row_hit_pred": int(pred_hits),
        "stream_row_hit_pred_rate": round(pred_hits / n_acc, 4),
        "same_cell_adjacent_rate": round(same_cell_adj / max(1, n_acc - 1), 4),
        "map": {"banks": a.banks, "rows": a.rows, "row_bytes": a.row_bytes,
                "bank_hash": a.bank_hash, "lines_per_bank_row": lines_per_row},
    })
    with open(out + ".json", "w") as f:
        json.dump(info, f, indent=2, sort_keys=True)

    print(f"{a.mode:9s} acc={n_acc} instr={info.get('instructions', n_instr)} "
          f"uniq_lines={info['unique_lines']} cells={info['unique_cells']} "
          f"pred_hit={info['stream_row_hit_pred_rate']:.4f} "
          f"adj_same_cell={info['same_cell_adjacent_rate']:.4f} "
          f"orig_dedup={orig_dedup}")
    print(f"          out={out} sha={info['trace_out_sha256'][:16]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
