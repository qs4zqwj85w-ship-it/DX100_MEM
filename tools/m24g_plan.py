#!/usr/bin/env python3
"""M24-G — plan the resource and capacity sweeps.

Emits build/wm/m24g/cells.tsv (one row per cell) and build/wm/m24g/cmds.txt
(the exact shell command line of every cell).  Both arms come from the M24-D
pair, so a common-resource cell differs from its reference only in the one
knob, and the knob is passed to *both* arms identically.  Bulk-specific cells
run the Bulk arm only; their SIMT denominator is the unchanged M24-D SIMT cell
already measured in build/wm/m24f/, which no architecture-specific knob can
touch (every such key is inert while dispatch = 0).

usage: python3 tools/m24g_plan.py [--check]
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m24g")
M24F = os.path.join(ROOT, "build/wm/m24f")
BULK = "../build/wm/m24d/m24d_bulk.cfg"
SIMT = "../build/wm/m24d/m24d_simt.cfg"

# ---- the panel --------------------------------------------------------------
# Four primary traces: the four regimes M24-F identified (order-bound win,
# translation-bound win, sequential loss, reuse loss).  Every sweep runs here.
PRIMARY = ["w4_duplow", "criteo_2k", "n1_seq8m", "n3_clu4m"]
# Three secondary traces: an L2-resident tie, the graph win and a second
# order-bound win.  Only the knobs that move the primary set run here, because
# orkut_hub alone is 5.8x the primary set's total cycles.
SECOND = ["w1_uniform", "orkut_hub", "n4_uni4m"]

# ---- common resources -------------------------------------------------------
# (sweep, knob, levels).  The production value of each knob is the M24-F
# baseline cell and is not repeated here; it is read from build/wm/m24f/.
COMMON = [
    ("walkers", "walkers", ["2", "4"]),            # production 8
    ("dtlbmshr", "dtlb_mshr", ["4", "16"]),        # production 8
    ("l2tlbmshr", "l2tlb_mshr", ["16"]),           # production 8
    # production 128.  256 is not constructible: the fused window stores the slot
    # id in 8 bits, so the engine refuses R > 255 (bulk.cpp:329).  192 is the
    # largest supported step above production and is what the sweep uses.
    ("rbudget", "R", ["64", "192"]),
    ("l2mshr", "l2_mshr", ["128"]),                # production 64
    ("dramq", "dram_q", ["8", "64"]),              # production 0 = unbounded
    ("banks", "banks", ["16"]),                    # production 2
]
# Reduced grid for the secondary traces: the five knobs that moved the primary
# set at all, one level each.
COMMON_SECOND = [("walkers", "walkers", ["2"]),
                 ("dtlbmshr", "dtlb_mshr", ["16"]),
                 ("rbudget", "R", ["192"]),
                 ("l2mshr", "l2_mshr", ["128"]),
                 ("banks", "banks", ["16"])]

# Beyond the requested grid, kept separate in the report: the first point where
# the 8-entry L2-TLB MSHR file stops being the translation ceiling.  M23-F1
# measured this ceiling on the M23 configuration; this re-measures it on the
# M24-D pair, and it is the only point where l2tlb_mshr is not inert.
DIAG = ("translation16", {"walkers": "16", "dtlb_mshr": "16", "l2tlb_mshr": "16"},
        PRIMARY)

# Two combinations, added after the single-knob sweeps showed which knobs are
# free: does the smaller ready pool still hold when the DRAM is twice as wide,
# and do the pool and the larger window compose?  Bulk arm only.
COMBO = [("combo_banks16_pool512", {"banks": "16", "g6b_pa_capacity": "512"},
          ["w4_duplow", "criteo_2k", "n3_clu4m"]),
         ("combo_win32k_pool512", {"batch_window": "32768", "g6b_pa_capacity": "512"},
          ["w4_duplow", "criteo_2k", "n3_clu4m"])]

# ---- Bulk-specific capacities ----------------------------------------------
# (sweep, knob, levels).  Every one is a structure only the Bulk arm has; the
# SIMT arm has no counterpart and is not given one.
BULK_ONLY = [
    ("cohorts", "page_cohorts", ["1", "2", "4", "16"]),            # production 8
    ("window", "batch_window", ["8192", "32768"]),                 # production 16384
    ("readypool", "g6b_pa_capacity", ["512", "1024", "2048", "8192"]),  # 4096
    ("pagedir", "page_dir", ["128", "256", "1024"]),               # production 512
    ("orglanes", "org_lanes", ["2", "8"]),                         # production 4
    ("fanout", "fanout_lanes", ["1", "2", "8"]),                   # production 4
    ("idxmshr", "idx_mshr", ["16", "128"]),                        # production 64
    ("aglanes", "ag_lanes", ["4", "16"]),                          # production 8
]


def manifest():
    rows = {}
    with open(os.path.join(M24F, "traces.tsv")) as f:
        for r in csv.DictReader(f, delimiter="\t"):
            rows[r["id"]] = r
    return rows


def wargs(r):
    """The workload fields, identical for both arms (the M24-F manifest row)."""
    a = ["--set", "dataset=../build/wm/" + r["trace"],
         "--set", "elem_bytes=" + r["elem_bytes"]]
    if r["bulk_index_base"]:
        a += ["--set", "bulk_index_base=" + r["bulk_index_base"]]
    if r["bulk_desc_line"]:
        a += ["--set", "bulk_desc_line=" + r["bulk_desc_line"]]
    if r["bulk_max_elems"]:
        a += ["--set", "bulk_max_elems=" + r["bulk_max_elems"]]
    return a


def main():
    man = manifest()
    os.makedirs(OUT, exist_ok=True)
    cells, cmds = [], []

    def add(trace, sweep, level, knobs, arms, kind):
        r = man[trace]
        for arm in arms:
            cid = "%s__%s_%s__%s" % (trace, sweep, level, arm)
            cfg = BULK if arm == "bulk" else SIMT
            cmd = ["./bin/dx100sim", "--config", cfg] + wargs(r)
            for k, v in knobs.items():
                cmd += ["--set", "%s=%s" % (k, v)]
            cmd += ["--all"]
            cells.append((cid, kind, sweep, level, arm, trace,
                          ";".join("%s=%s" % kv for kv in knobs.items())))
            cmds.append((cid, " ".join(cmd)))

    # common resources: both arms move together
    for tr in PRIMARY:
        for sweep, knob, levels in COMMON:
            for lv in levels:
                add(tr, sweep, lv, {knob: lv}, ("bulk", "simt"), "common")
    for tr in SECOND:
        for sweep, knob, levels in COMMON_SECOND:
            for lv in levels:
                add(tr, sweep, lv, {knob: lv}, ("bulk", "simt"), "common")
    sweep, knobs, traces = DIAG
    for tr in traces:
        add(tr, sweep, "16", dict(knobs), ("bulk", "simt"), "diag")

    # Bulk-specific capacities: the Bulk arm only
    for tr in PRIMARY:
        for sweep, knob, levels in BULK_ONLY:
            for lv in levels:
                add(tr, sweep, lv, {knob: lv}, ("bulk",), "bulk")

    # two combinations
    for sweep, knobs, traces in COMBO:
        for tr in traces:
            add(tr, sweep, "combo", dict(knobs), ("bulk",), "combo")

    with open(os.path.join(OUT, "cells.tsv"), "w") as f:
        f.write("cell\tkind\tsweep\tlevel\tarm\ttrace\tknobs\n")
        for c in cells:
            f.write("\t".join(c) + "\n")
    with open(os.path.join(OUT, "cmds.txt"), "w") as f:
        for cid, cmd in cmds:
            f.write("%s\t%s > ../build/wm/m24g/log/%s.txt\n" % (cid, cmd, cid))

    n = {"common": 0, "bulk": 0, "diag": 0, "combo": 0}
    for c in cells:
        n[c[1]] += 1
    print("M24-G plan: %d cells (common %d, bulk-specific %d, diagnostic %d, combo %d)"
          % (len(cells), n["common"], n["bulk"], n["diag"], n["combo"]))
    print("  primary panel: %s" % " ".join(PRIMARY))
    print("  secondary panel: %s" % " ".join(SECOND))
    print("  -> build/wm/m24g/cells.tsv, cmds.txt")
    if "--check" in sys.argv:
        seen = set()
        for c in cells:
            assert c[0] not in seen, c[0]
            seen.add(c[0])
        print("  ids unique: %d" % len(seen))
    return 0


if __name__ == "__main__":
    sys.exit(main())
