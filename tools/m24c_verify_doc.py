#!/usr/bin/env python3
"""M24-C — recompute every figure quoted in the report from summary.csv and check
that the document contains it. A miss is a transcription error or a stale run."""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, "docs", "m24-c-resource-matched-simt-baseline.md")
rows = list(csv.DictReader(open(os.path.join(ROOT, "build", "wm", "m24c", "summary.csv"))))
by = {(r["cfg"], r["wl"]): r for r in rows}
C = ["m0", "m0rm", "b8k", "b16k", "b32k", "g6ds1"]
W = ["orkut", "livejournal", "ogbn", "criteo"]

doc = open(DOC, encoding="utf-8").read()
norm = doc.replace("\u2212", "-").replace("\u00d7", "x").replace("\u2009", " ")
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


for w in W:
    for c in C:
        want(sp(by[(c, w)]["cycles"]))
    m0, rm = I(by[("m0", w)], "cycles"), I(by[("m0rm", w)], "cycles")
    b16, g6 = I(by[("b16k", w)], "cycles"), I(by[("g6ds1", w)], "cycles")
    want("%.3f" % (m0 / rm))
    want("%.3f" % (rm / b16))
    want("%.3f" % (rm / g6))
    want("%.3f" % (m0 / b16))
    want("%.3f" % (m0 / g6))

# translation table
for w in W:
    for c in ("m0", "m0rm", "b16k", "g6ds1"):
        r = by[(c, w)]
        want("%.1f %%" % (100.0 * I(r, "tlb0_hits") / (I(r, "tlb0_hits") + I(r, "tlb0_misses"))))
        want("%.1f %%" % (100.0 * I(r, "l2tlb_hits") / (I(r, "l2tlb_hits") + I(r, "l2tlb_misses"))))
        want("%.2f" % (F(r, "tlb0_mshr_occ") / I(r, "cycles")))
        want("%.2f" % (F(r, "l2tlb_occ") / I(r, "cycles")))
        want(sp(r["inflight_max"]))
        want(sp(r["lat_p50"]))
        want(sp(r["lat_p95"]))
        want(sp(r["dram_reads"]))
        want("%.1f %%" % (100.0 * I(r, "dram_row_hits") / I(r, "dram_reads")))
        want(sp(r["dram_row_acts_sum"]))
        want("%.1f %%" % (100.0 * (I(r, "dram_bank0_busy") + I(r, "dram_bank1_busy"))
                          / (2.0 * I(r, "cycles"))))
        want("%.2f" % (F(r, "dram_q_occ_sum") / I(r, "cycles")))
        want(sp(r["dram_wait_sum"]))
        want("%.2f" % (F(r, "l1_mshr_occ") / I(r, "cycles")))
        want("%.2f" % (F(r, "l2_mshr_occ") / I(r, "cycles")))

# frontend table
for w in W:
    for c in ("m0", "m0rm"):
        r = by[(c, w)]
        want("%.4f" % (I(r, "instr_issued") / I(r, "cycles")))
        want(sp(r["requests_generated"]))
        want(sp(r["simt_slot_full_cycles"]))
        want(sp(r["unissued_cycles"]))
        want(sp(r["tlb_refuse_full"]))
        want(sp(r["simt_peak_outstanding"]))

# narrative claims, recomputed
m0o, rmo = by[("m0", "orkut")], by[("m0rm", "orkut")]
want(sp(m0o["dram_reads"]))
want(sp(rmo["dram_reads"]))
want(sp(m0o["walks"]))
want(sp(by[("b16k", "orkut")]["walks"]))
want(sp(by[("g6ds1", "orkut")]["walks"]))
want("%.1f %%" % (100.0 * I(by[("b16k", "ogbn")], "dram_reads") / I(by[("m0", "ogbn")], "dram_reads") - 100.0))
want(sp(by[("b16k", "ogbn")]["dram_reads"]))
want(sp(by[("m0", "ogbn")]["dram_reads"]))
want("4 924 checks")
want("24/24")
# historical anchors
for v in (49067545, 67234692, 176184939, 28511774):
    want(sp(v))

print(f"{checked} figures checked, {len(miss)} missing")
for m in miss:
    print("   missing:", m)
sys.exit(1 if miss else 0)
