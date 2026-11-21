#!/usr/bin/env python3
"""M24-H — plan the storage-ladder cells.

The M24-H audit prices the production Bulk engine structure by structure.  One
question can only be answered by measurement: what does the M23-G6D stack
(physical order + PA buffer + streaming ready pool + cohort admission) buy over
the fused window it was built on?  M24-E ablated the whole bundle
(`win_fused = 0 bulk_org = 0`, the M6-M9 window, a *different* representation).
This milestone needs the same representation with the G6D structures removed one
step at a time, so the ladder is:

  org 0   fused window + P0 cohort scheduler          (no `ford`, no PA buffer)
  org 6   + physical order + PA buffer (G6A)          (+ ford, fpa, ftr)
  org 7   + streaming ready pool (G6B)                (+ fpos, ready)
  org 8   + cohort admission (G6C/G6D) = production   (+ g6c counters)

Every cell is `build/wm/m24d/m24d_bulk.cfg` plus one `--set bulk_org=` and the
trace's own dataset arguments.  Nothing else moves, so each step's cycle delta is
the arithmetic difference of two runs of the same binary on the same trace, and
each step's storage delta is read from `sim/src/{sim.hpp,bulk.cpp}` (§2 of the
report), not estimated.

usage: python3 tools/m24h_plan.py [--check]
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m24h")
CFG = "../build/wm/m24d/m24d_bulk.cfg"
BIN = "./bin/dx100sim"

# The four M24-F primary traces, with the exact dataset arguments build/wm/m24f
# used.  `w4`/`n1`/`n3` are simulator-generated frames (index base and descriptor
# line pinned by the generator); `criteo_2k` is a file trace with 4 B elements.
TRACES = {
    "w4_duplow": ["dataset=../build/wm/m21_w4_duplow_s1_full.bin", "elem_bytes=8",
                  "bulk_index_base=8421440", "bulk_desc_line=131584",
                  "bulk_max_elems=409600"],
    "criteo_2k": ["dataset=../build/wm/m23_criteo_2k.bin", "elem_bytes=4"],
    "n1_seq8m": ["dataset=../build/wm/m24f/n1_seq8m.bin", "elem_bytes=8",
                 "bulk_index_base=8421440", "bulk_desc_line=131584",
                 "bulk_max_elems=409600"],
    "n3_clu4m": ["dataset=../build/wm/m24f/n3_clu4m.bin", "elem_bytes=8",
                 "bulk_index_base=4227136", "bulk_desc_line=66048",
                 "bulk_max_elems=409600"],
}
ORDER = ["w4_duplow", "criteo_2k", "n1_seq8m", "n3_clu4m"]

# M23's own panel: the four traces the G6A-G6D stack was designed and tuned on.
# Only org 0 is measured here (the storage-ladder step that is not yet on this
# panel); production org 8 is build/wm/m24e's `base` cell, same config.
M23_TRACES = {
    "orkut": ["dataset=../build/wm/m23_orkut_bfs.bin"],
    "livejournal": ["dataset=../build/wm/m23_livejournal_bfs.bin"],
    "ogbn": ["dataset=../build/wm/m23_ogbn.bin"],
}
M23_ORDER = ["orkut", "livejournal", "ogbn"]

# org 8 is the production cell and is not re-run: build/wm/m24f/<trace>__bulk.txt
# is byte-identical in configuration and is the denominator of every step.
LADDER = ["0", "6", "7"]


def cell_id(org, tr):
    return "org%s__%s__bulk" % (org, tr)


def main(argv):
    os.makedirs(os.path.join(OUT, "log"), exist_ok=True)
    rows, cmds = [], []
    for org in LADDER:
        for tr in ORDER:
            sets = ["--set " + a for a in TRACES[tr]] + ["--set bulk_org=" + org]
            cmd = "%s --config %s %s --all > ../build/wm/m24h/log/%s.txt" % (
                BIN, CFG, " ".join(sets), cell_id(org, tr))
            rows.append([cell_id(org, tr), "ladder", "org" + org, "bulk", tr])
            cmds.append([cell_id(org, tr), cmd])

    # M23 panel: one cell (org 0) per trace.  These are the traces G6D was built
    # for, so leaving them out would let the M24 panel speak for the design.
    for tr in M23_ORDER:
        sets = ["--set " + a for a in M23_TRACES[tr]] + ["--set bulk_org=0"]
        cmd = "%s --config %s %s --all > ../build/wm/m24h/log/%s.txt" % (
            BIN, CFG, " ".join(sets), "org0__%s__bulk" % tr)
        rows.append(["org0__%s__bulk" % tr, "m23panel", "org0", "bulk", tr])
        cmds.append(["org0__%s__bulk" % tr, cmd])

    # Production (org 8) on the same three traces: the same configuration M24-E
    # measured as its `base` cell, re-run so the audit's comparison does not
    # depend on a quoted number.  Reproducing M24-E to the cycle is the
    # determinism cross-check build/wm/m24h/checks.txt asserts.
    for tr in M23_ORDER:
        sets = ["--set " + a for a in M23_TRACES[tr]]
        cmd = "%s --config %s %s --all > ../build/wm/m24h/log/%s.txt" % (
            BIN, CFG, " ".join(sets), "prod__%s__bulk" % tr)
        rows.append(["prod__%s__bulk" % tr, "m23panel", "org8", "bulk", tr])
        cmds.append(["prod__%s__bulk" % tr, cmd])

    with open(os.path.join(OUT, "cells.tsv"), "w", newline="") as f:
        w = csv.writer(f, delimiter="\t")
        w.writerow(["cell", "kind", "sweep", "arm", "trace"])
        w.writerows(rows)
    with open(os.path.join(OUT, "cmds.txt"), "w") as f:
        for cid, cmd in cmds:
            f.write("%s\t%s\n" % (cid, cmd))

    print("M24-H plan: %d ladder cells (org %s), production org 8 reused from build/wm/m24f"
          % (len(rows), "/".join(LADDER)))
    if "--check" in argv:
        if not os.path.exists(os.path.join(ROOT, "build/wm/m24f/w4_duplow__bulk.txt")):
            print("MISSING production reference log", file=sys.stderr)
            return 1
        print("production reference logs present: %s" % ", ".join(ORDER))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
