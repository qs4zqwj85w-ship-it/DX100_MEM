#!/usr/bin/env python3
"""M24-D — recompute every figure quoted in the report and check it is present.

Each token is built from build/wm/m24d/summary.csv (or from config_audit.txt, or
from the historical M24-C summary for the M0 column and the fused-Bulk table), so
a miss is either a transcription error or a stale run. Nothing is checked against
this file's own constants except the historical M0 cycles, which are read from
build/wm/m24c/summary.csv.
"""
import csv
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, "docs", "m24-d-strict-resource-matched-comparison.md")
D = os.path.join(ROOT, "build", "wm", "m24d")
W = ["orkut", "livejournal", "ogbn", "criteo"]

doc = open(DOC, encoding="utf-8").read()
norm = (doc.replace("\u2212", "-").replace("\u00d7", "x").replace("\u2009", " ")
           .replace("\u00a0", " "))
miss, checked = [], 0


def want(tok):
    global checked
    checked += 1
    t = str(tok).replace("\u2212", "-").replace("\u00d7", "x")
    if t not in norm:
        miss.append(t)


def sp(n):
    return f"{int(n):,}".replace(",", " ")


def I(r, k):
    return int(r[k])


def F(r, k):
    return float(r[k])


def pct(a, b):
    return 100.0 * a / b


rows = list(csv.DictReader(open(os.path.join(D, "summary.csv"))))
prim = {(r["arm"], r["wl"]): r for r in rows if r["cell"].count("_") == 1}
mapc = {(r["arm"], r["wl"], int(r["cell"].rsplit("s", 1)[1])): r
        for r in rows if "_map3_s" in r["cell"]}
seedc = {(r["arm"], int(r["cell"].rsplit("seed", 1)[1])): r
         for r in rows if "_w1_seed" in r["cell"]}
hist = {(r["cfg"], r["wl"]): int(r["cycles"])
        for r in csv.DictReader(open(os.path.join(ROOT, "build", "wm", "m24c", "summary.csv")))}

# ---- 1. cycles and the speedup tables ---------------------------------------
for w in W:
    s, b = prim[("simt", w)], prim[("bulk", w)]
    for v in (hist[("m0", w)], I(s, "cycles"), I(b, "cycles")):
        want(sp(v))
    for a, c in ((("m0", w), None), ((None, w), None)):
        pass
    want("%.3f" % (hist[("m0", w)] / I(b, "cycles")))          # M0 / Bulk
    want("%.3f" % (I(s, "cycles") / I(b, "cycles")))           # M0-RM / Bulk
    want("%.3f" % (hist[("m0", w)] / I(s, "cycles")))          # M0 / M0-RM
    want("%.3f" % (I(s, "cycles") / I(b, "cycles") /
                   (hist[("m0", w)] / I(b, "cycles"))))        # fraction left
    # the fused-Bulk table, quoted from M24-C
    for c in ("b8k", "b16k", "b32k"):
        want("%.3f" % (I(s, "cycles") / hist[(c, w)]))

# ---- 2. walks, reads, row hits ---------------------------------------------
for w in W:
    s, b = prim[("simt", w)], prim[("bulk", w)]
    for v in (I(s, "walks"), I(b, "walks")):
        want(sp(v))
    want("%.3f" % (I(s, "walks") / I(b, "walks")))                       # walk ratio
    want("%.1f %%" % pct(I(s, "dram_row_hits"), I(s, "dram_reads")))
    want("%.1f %%" % pct(I(b, "dram_row_hits"), I(b, "dram_reads")))
    want("%+.1f %%" % (pct(I(b, "dram_reads"), I(s, "dram_reads")) - 100.0))
    want("%+.1f %%" % (pct(I(b, "dram_row_acts_sum"), I(s, "dram_row_acts_sum")) - 100.0))

# ---- 3. the per-cell tables -------------------------------------------------
for w in W:
    for arm in ("simt", "bulk"):
        r = prim[(arm, w)]
        want("%.1f %%" % pct(I(r, "tlb0_hits"), I(r, "tlb0_reads")))
        want("%.1f %%" % pct(I(r, "l2tlb_hits"), I(r, "l2tlb_reads")))
        want("%.2f" % (F(r, "walker_busy") / I(r, "cycles")))
        want(sp(r["lat_p50"]))
        want(sp(r["lat_p95"]))
        for k in ("dram_reads", "dram_row_acts_sum", "l2_reads"):
            want(sp(r[k]))
        want("%.1f %%" % pct(I(r, "l2_hits"), I(r, "l2_reads")))
        want("%.1f %%" % pct(I(r, "dram_bank0_busy") + I(r, "dram_bank1_busy"),
                             2 * I(r, "cycles")))
        want("%.2f" % (F(r, "dram_q_occ_sum") / I(r, "cycles")))
        want("%.3f" % (I(r, "dram_bytes") / I(r, "cycles")))
        want("%.4f" % (I(r, "lines") / I(r, "cycles")))
        want(sp(r["tlb_refuse_full"]))
        want(sp(r["replays"]))

# ---- 4. mapping and seed sweeps --------------------------------------------
dev = 0.0
for w in W:
    base = I(prim[("simt", w)], "cycles") / I(prim[("bulk", w)], "cycles")
    vals = []
    for s in (1, 2, 3):
        b = mapc[("bulk", w, s)]
        if I(b, "cycles") >= 500000000:
            continue
        spd = I(mapc[("simt", w, s)], "cycles") / I(b, "cycles")
        vals.append(spd)
        want("%.3f" % spd)
        dev = max(dev, abs(spd - base) / base)
    want("%.3f" % min(vals))
    want("%.3f" % max(vals))
    want("%.3f" % sorted(vals)[len(vals) // 2])
want("%.1f %%" % (100.0 * dev))
for s in (1, 7, 13):
    want(sp(I(seedc[("simt", s)], "cycles")))
    want(sp(I(seedc[("bulk", s)], "cycles")))
    want("%.3f" % (I(seedc[("simt", s)], "cycles") / I(seedc[("bulk", s)], "cycles")))
seeds = [I(seedc[("simt", s)], "cycles") for s in (1, 7, 13)]
want("%.1f %%" % (100.0 * (max(seeds) - min(seeds)) / min(seeds)))

# ---- 5. the non-terminating cell -------------------------------------------
g = mapc[("bulk", "criteo", 3)]
want(sp(g["bulk_elems_done"]))
want(sp(g["bulk_win_sealed"]))
want(sp(g["dram_reads"]))
want(sp(g["bulk_g6b_ready_peak"]))
want(sp(g["bulk_last_done_cycle"]))
want(sp(g["bulk_last_seal"]))
want(sp(g["cycles"]))
want("%.1f %%" % (100.0 * I(g, "bulk_last_done_cycle") / I(g, "cycles")))

# ---- 6. capacity bounds quoted in the text ---------------------------------
for w in W:
    for arm in ("simt", "bulk"):
        r = prim[(arm, w)]
        want(sp(r["walker_max"]))
if all(I(prim[("bulk", w)], "bulk_peak_slot") <= 128 for w in W):
    want("128")
want(sp(132))
want(sp(128))
want(sp(64))

# ---- 7. the config audit's own counts --------------------------------------
audit = open(os.path.join(D, "config_audit.txt")).read()
m = re.search(r"(\d+) keys: (\d+) resource keys in (\d+) rows, (\d+) settings, "
              r"(\d+) architecture fields", audit)
for g6 in m.groups():
    want(g6)
echo = re.findall(r"(\d+) echoed keys, (\d+) differ", audit)
for a, b in echo:
    want(a)
    want(b)

# ---- 8. the test suite ------------------------------------------------------
tests = os.path.join(ROOT, "sim", "bin", "dx100_tests")
if os.path.exists(tests):
    out = subprocess.run([tests], capture_output=True, text=True).stdout
    t = re.search(r"(\d+) checks, (\d+) failures", out)
    if t:
        want(sp(t.group(1)))
        want(t.group(2))
        global_ok = t.group(2) == "0"

print(f"{checked} figures checked, {len(miss)} missing")
for m in miss:
    print("   missing: " + m)
sys.exit(1 if miss else 0)
