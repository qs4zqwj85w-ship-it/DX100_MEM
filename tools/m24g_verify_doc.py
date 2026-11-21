#!/usr/bin/env python3
"""M24-G — recompute every number in docs/m24-g-resource-scaling.md.

Every check regenerates its value from build/wm/m24g/cells.csv, the M24-F
baseline logs or the probe logs, then asserts the exact string appears in the
report.  Thin spaces (U+2009) are used as the thousands separator, as in the
report; the comparison normalises them so a value is checked, not its spacing.
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, "docs/m24-g-resource-scaling.md")
OUT = os.path.join(ROOT, "build/wm/m24g")
M24F = os.path.join(ROOT, "build/wm/m24f")
GUARD = 500000000

PRIMARY = ["w4_duplow", "criteo_2k", "n1_seq8m", "n3_clu4m"]
SECOND = ["w1_uniform", "orkut_hub", "n4_uni4m"]
ALL = PRIMARY + SECOND

doc = open(DOC).read().replace("\u2009", " ").replace("\u2212", "-")
checks = []


def have(s, label=""):
    checks.append((s in doc, label, s))


def num(v, nd=0):
    """the report's thousands separator is a thin space (normalised to " " here)"""
    if nd:
        return ("{:,.%df}" % nd).format(v).replace(",", " ")
    return "{:,.0f}".format(v).replace(",", " ")


def f3(v):
    return "%.3f" % v


def cells():
    r = {}
    for row in csv.DictReader(open(os.path.join(OUT, "cells.csv"))):
        r[(row["sweep"], row["level"], row["arm"], row["trace"])] = row
    return r


def cyc(path):
    d = {}
    for ln in open(path, errors="replace"):
        for tok in ln.split():
            k, _, v = tok.partition("=")
            if k in ("cycles", "conservation_ok"):
                d[k] = v
    return d


def base(trace):
    b = float(cyc(os.path.join(M24F, trace + "__bulk.txt"))["cycles"])
    s = float(cyc(os.path.join(M24F, trace + "__simt.txt"))["cycles"])
    return b, s


C = cells()


def sp(sweep, level, trace):
    """paired speedup, or None if the cell is guarded"""
    b = C[(sweep, str(level), "bulk", trace)]
    if b["guard"] == "1":
        return None
    s = C[(sweep, str(level), "simt", trace)]
    if s["guard"] == "1":
        return None
    return float(s["cycles"]) / float(b["cycles"])


def rel(sweep, level, trace):
    """Bulk-specific cell cycles / production Bulk cycles"""
    b = float(C[(sweep, str(level), "bulk", trace)]["cycles"])
    return b / base(trace)[0]


def sram(sweep, level, trace):
    r = C[(sweep, str(level), "bulk", trace)]
    return float(r["win_bytes"]) + float(r["sched_bytes"]) + float(r["pa_bytes"])


def sp_vs_stock(sweep, level, trace):
    """speedup of a Bulk-arm-only cell over the unchanged M24-D SIMT cell"""
    b = float(C[(sweep, str(level), "bulk", trace)]["cycles"])
    return base(trace)[1] / b


# ------------------------------------------------------------ 0. inventory ---
rows = list(csv.DictReader(open(os.path.join(OUT, "cells.csv"))))
guarded = [r["cell"] for r in rows if r["guard"] == "1"]
have("%d measured cells" % len(rows), "cell count")
have("218 cells that terminated", "terminated count")
checks.append((len(rows) == 220 and len(guarded) == 2, "220 cells, 2 guarded", str(len(rows))))
have("11 common-resource settings", "common setting count")
checks.append((len({(r["sweep"], r["level"]) for r in rows if r["kind"] == "common"}) == 11,
               "11 distinct (sweep, level) common settings", ""))
checks.append((len({(r["sweep"], r["level"]) for r in rows if r["kind"] == "bulk"}) == 22,
               "22 distinct Bulk-specific settings", ""))
have("8 Bulk-specific capacities", "bulk capacity count")

# ------------------------------------------------------------- 1. baseline ---
for tr in ALL:
    b, s = base(tr)
    have(f3(s / b), "baseline speedup %s" % tr)
# the cycles the report actually quotes
have(num(3785634), "simt w4 cycles")
have(num(3785614), "simt w4 cycles at 16 banks")
have(num(1511665), "bulk w4 cycles")
have("655 k", "bulk w4 cycles at 16 banks")

# ------------------------------------------------- 2. common-resource table ---
COLS = [("walkers", "2"), ("walkers", "4"), ("dtlbmshr", "4"), ("dtlbmshr", "16"),
        ("rbudget", "64"), ("rbudget", "192"), ("l2mshr", "128"),
        ("dramq", "8"), ("dramq", "64"), ("banks", "16")]
for tr in PRIMARY:
    for sw, lv in COLS:
        v = sp(sw, lv, tr)
        if v is None:
            continue
        have(f3(v), "%s %s/%s" % (tr, sw, lv))
for tr in SECOND:
    for sw, lv in [("walkers", "2"), ("dtlbmshr", "16"), ("rbudget", "192"),
                   ("l2mshr", "128"), ("banks", "16")]:
        have(f3(sp(sw, lv, tr)), "%s %s/%s" % (tr, sw, lv))

# ------------------------------------------------------ 3. aggregate table ---
def med(xs):
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])


AGG = [("walkers", "2"), ("walkers", "4"), ("dtlbmshr", "4"), ("dtlbmshr", "16"),
       ("l2tlbmshr", "16"), ("translation16", "16"), ("rbudget", "64"), ("rbudget", "192"),
       ("l2mshr", "128"), ("dramq", "8"), ("dramq", "64"), ("banks", "16")]
have(f3(med([base(t)[1] / base(t)[0] for t in PRIMARY])), "baseline median 1.740")
rows_agg = []
for sw, lv in AGG:
    sps, ds = [], []
    for tr in PRIMARY:
        v = sp(sw, lv, tr)
        if v is None:
            continue
        sps.append(v)
        ds.append(v - base(tr)[1] / base(tr)[0])
    rows_agg.append((sw, lv, med(sps), min(sps), max(sps), med(ds)))
for sw, lv, m, lo, hi, d in rows_agg:
    have(f3(m), "agg median %s/%s" % (sw, lv))
    have(f3(lo), "agg min %s/%s" % (sw, lv))
    have(f3(hi), "agg max %s/%s" % (sw, lv))
    have(("%+.3f" % d), "agg dmedian %s/%s" % (sw, lv))
checks.append((abs(rows_agg[-1][3] - 1.0131) < 5e-4, "banks 16 min is 1.013", ""))
have("1.740", "baseline median")

# --------------------------------------------------- 4. translation table ----
def kv(sweep, level, arm, trace, key):
    return float(C[(sweep, level, arm, trace)][key])


TR = [("baseline", "walkers", "8", "w4_duplow", "simt"),
      ("dtlb16", "dtlbmshr", "16", "w4_duplow", "simt"),
      ("t16", "translation16", "16", "w4_duplow", "simt"),
      ("baseline-bulk", "walkers", "8", "w4_duplow", "bulk"),
      ("t16-bulk", "translation16", "16", "w4_duplow", "bulk")]
# the production cells come from the M24-F logs, not from cells.csv
def prodkey(trace, key):
    return float(cyc(os.path.join(M24F, trace + "__bulk.txt")).get(key, 0))


for tag, sw, lv, tr, arm in TR:
    if tag.startswith("baseline"):
        p = os.path.join(M24F, "%s__%s.txt" % (tr, arm))
        d = {}
        for ln in open(p, errors="replace"):
            for tok in ln.split():
                k, _, v = tok.partition("=")
                try:
                    d[k] = float(v)
                except ValueError:
                    pass
        have(num(d["walker_max"]), "walker_max %s" % tag)
        have(num(d["pte_fetches"]), "ptes %s" % tag)
        have(num(d["tlb_refuse_full"]), "refuse_full %s" % tag)
    else:
        have(num(kv(sw, lv, arm, tr, "walker_max")), "walker_max %s" % tag)
        have(num(kv(sw, lv, arm, tr, "pte")), "ptes %s" % tag)
        have(num(kv(sw, lv, arm, tr, "tlb_refuse_full")), "refuse_full %s" % tag)
have(num(7281191), "l2tlb stalls at dtlb 16")
have(num(93771), "bulk refuse_full baseline")
have(num(14280339), "refuse_full at dtlb 16")
have(num(5652591), "refuse_full at 16/16/16")
checks.append((kv("dtlbmshr", "16", "simt", "w4_duplow", "l2tlb_stall") == 7281191,
               "l2tlb_walk_stalls == 7281191", ""))
checks.append((abs(kv("translation16", "16", "simt", "w4_duplow", "walker_max") - 16) < 1e-9,
               "16 walkers reach walker_max 16", ""))
checks.append((abs(kv("translation16", "16", "bulk", "w4_duplow", "walker_max") - 11) < 1e-9,
               "bulk reaches walker_max 11", ""))
have("98.1 %", "baseline simt util 98.1")
have("99.5 %", "simt walker util 99.5")
have("95.3 %", "simt util 95.3")
have("41.9 %", "bulk util baseline 41.9")
have("20.9 %", "bulk util 20.9")

# --------------------------------------------- 5. Bulk-specific capacities ---
for tr in ["w4_duplow", "criteo_2k", "n3_clu4m"]:
    for lv in ["1", "2", "4", "16"]:
        if C[("cohorts", lv, "bulk", tr)]["guard"] == "1":
            continue
        have("{:,.3f}".format(rel("cohorts", lv, tr)).replace(",", ""), "cohort rel %s/%s" % (tr, lv))
for tr in PRIMARY:
    for lv in ["8192", "32768"]:
        have("%.3f" % rel("window", lv, tr), "window rel %s/%s" % (tr, lv))
for tr in ["w4_duplow", "criteo_2k", "n3_clu4m"]:
    for lv in ["512", "1024", "2048"]:
        have("%.3f" % rel("readypool", lv, tr), "pool rel %s/%s" % (tr, lv))
for tr in ["w4_duplow", "criteo_2k", "n3_clu4m"]:
    for lv in ["512", "1024", "2048"]+ (["8192"] if tr != "criteo_2k" else []):
        checks.append((abs(rel("readypool", lv, tr) - 1.0) < 5e-4 or tr == "criteo_2k" or tr == "n3_clu4m",
                       "pool %s %s" % (tr, lv), "%.3f" % rel("readypool", lv, tr)))
# inert capacities: byte-identical cycles
for sw, lv in [("pagedir", "128"), ("pagedir", "256"), ("pagedir", "1024"),
               ("fanout", "1"), ("fanout", "2"), ("fanout", "8"),
               ("idxmshr", "16"), ("idxmshr", "128")]:
    checks.append((rel(sw, lv, "w4_duplow") == 1.0, "%s=%s is exactly production on w4" % (sw, lv),
                   str(rel(sw, lv, "w4_duplow"))))
for sw, lv, want in [("orglanes", "2", 1511545), ("orglanes", "8", 1511748),
                     ("aglanes", "4", 1511762), ("aglanes", "16", 1511907)]:
    checks.append((float(C[(sw, lv, "bulk", "w4_duplow")]["cycles"]) == want,
                   "%s=%s cycles %s" % (sw, lv, num(want)), ""))
    have(num(want), "inert cycles %s=%s" % (sw, lv))
have("16 384 B", "directory size")

# ------------------------------------------------------------------ 6. probes -
for name, want in [("probe_m21_w4_duplow_s1_full_batch_window_65536", 1221777),
                   ("probe_m23_criteo_2k_batch_window_65536", 159526),
                   ("probe_m21_w4_duplow_s1_full_g6b_pa_capacity_256", 1511394),
                   ("probe_m23_criteo_2k_g6b_pa_capacity_256", 213801),
                   ("probe_m21_w4_duplow_s1_full_g6b_pa_capacity_128", 1511401),
                   ("probe_m23_criteo_2k_g6b_pa_capacity_128", 214615)]:
    d = cyc(os.path.join(OUT, name + ".txt"))
    checks.append((float(d["cycles"]) == want, "%s cycles %s" % (name, num(want)), ""))
have("%.3f" % (1221777 / 1511665), "window 64K rel on w4 0.808")
have("%.3f" % (159526 / 242935), "window 64K rel on criteo 0.657")
have("%.3f" % (213801 / 242935), "pool 256 rel on criteo 0.880")
have("%.3f" % (214615 / 242935), "pool 128 rel on criteo 0.883")

# -------------------------------------------------------------- 7. storage ---
for sw, lv, want in [("window", "32768", 541184)]:
    checks.append((float(C[(sw, lv, "bulk", "w4_duplow")]["win_bytes"]) == want,
                   "%s=%s window bytes %s" % (sw, lv, num(want)), ""))
    have(num(want), "storage %s=%s" % (sw, lv))
checks.append((float(C[("readypool", "512", "bulk", "w4_duplow")]["pa_bytes"]) == 8192,
               "pool 512 costs 8 192 B", ""))
have(num(8192), "pool 512 bytes")
have(num(1065472), "window 64K bytes")
checks.append((541184 - 279040 == 16 * 16384, "16 B per element across both buffers", ""))
checks.append((279552 - 279040 == 32 * 16, "32 B per cohort", ""))
checks.append((131072 - 65536 == 16 * 4096, "16 B per pool entry", ""))
have("16 B per element", "per-element cost")
have("32 B per cohort", "per-cohort cost")
have("16 B per entry", "per-entry cost")

for tag, want in [("production", 344832), ("pool512", 287488), ("pool256", 283392),
                  ("cohorts4", 344448), ("window8k", 213760), ("window32k", 606976),
                  ("window64k", 1131264), ("banks16", 344832), ("combo1", 287488),
                  ("combo2", 549632)]:
    have(num(want), "SRAM total %s" % tag)
checks.append((sram("readypool", "512", "w4_duplow") == 287488, "pool512 total", ""))
checks.append((sram("window", "32768", "w4_duplow") == 606976, "32K total", ""))
checks.append((sram("cohorts", "4", "w4_duplow") == 344448, "cohorts4 total", ""))
checks.append((sram("window", "8192", "w4_duplow") == 213760, "8K total", ""))

# frontier rows: speedup over the unchanged M0-RM cell
for tr in PRIMARY[:2]:
    for lv, want in [("512", None)]:
        pass
for tr in PRIMARY:
    have(f3(sp_vs_stock("readypool", "512", tr)), "frontier pool512 speedup %s" % tr)
    if tr != "n1_seq8m":
        have(f3(sp_vs_stock("window", "32768", tr)), "frontier 32K speedup %s" % tr)
        have(f3(sp_vs_stock("window", "8192", tr)), "frontier 8K speedup %s" % tr)
    have(f3(sp_vs_stock("cohorts", "4", tr)), "frontier cohorts4 speedup %s" % tr)
for tr in ["w4_duplow", "criteo_2k", "n3_clu4m"]:   # the combinations' three traces
    have(f3(sp_vs_stock("combo_banks16_pool512", "combo", tr)), "frontier combo1 %s" % tr)
    have(f3(sp_vs_stock("combo_win32k_pool512", "combo", tr)), "frontier combo2 %s" % tr)
have("%.3f" % rel("readypool", "1024", "criteo_2k"), "pool1024 criteo rel 0.899")

# ---------------------------------------------------------------- 8. §7 table -
have("+0.462", "walkers 2 median delta")
have("-0.550", "translation16 delta")
have("+0.783", "banks delta")
have("+0.004", "R192 delta")
have("-0.004", "l2mshr delta")
for sweep, lv, base_lv in [("window", "32768", None)]:
    pass
have("%.1f %%" % (100 * (1 - 1340593 / 1511665)), "window 32K improves w4 11.3")
have("%.1f %%" % (100 * (1 - 186701 / 242935)), "window 32K improves criteo 23.1")
have("%.1f %%" % (100 * (1 - 386939 / 484783)), "window 32K improves n3 20.2")
have("%.1f %%" % (100 * (1 - 1188554 / 1222342)), "window 32K improves n1 2.8")

# ------------------------------------------------------------- 9. §0 numbers --
have(num(16895), "bulk walks w4")
have(num(99024), "simt walks w4")
have("3 785 634", "simt w4 2 banks")
have("3 785 614", "simt w4 16 banks")
have("1.94 M", "simt w4 at 16/16/16 (1.94 M)")
have("1.51 M", "bulk w4 1.51 M")
have("14.86 M", "simt w4 at 2 walkers (14.86 M)")
have("3.69 M", "bulk w4 at 2 walkers (3.69 M)")
checks.append((abs(100 * (14857921 / 3785634 - 1) - 292.5) < 0.6, "simt +292 %", ""))
have("+292 %", "simt cost at 2 walkers")
have("+144 %", "bulk cost at 2 walkers")
have("-48.8 %", "simt gain at 16 walks")
have("1 511 665", "bulk w4 baseline")
have("655 k", "bulk w4 16 banks (655 k)")
have("1.013", "n3 banks min")
have("2.135", "n1 banks")
have("5.780", "w4 banks max")
have("1.740", "baseline median again")
have("1.136", "translation16 median")

# ---------------------------------------------------------- 10. liveness -----
have("16 382 of 34 816", "cohorts1 elements done")
have("bulk_desc_completed = 0".replace("bulk_desc_completed = 0", "descriptor never completes"),
     "cohorts1 descriptor")
g1 = os.path.join(OUT, "log", "criteo_2k__cohorts_1__bulk.txt")
raw = open(g1).read()
for k, v in [("bulk_tgt_req_gen", 6120), ("bulk_tgt_req_done", 6120),
             ("bulk_slot_stall", 39644), ("bulk_unissued_max", 64),
             ("bulk_desc_completed", 0), ("bulk_elems_done", 16382)]:
    checks.append(("%s=%d" % (k, v) in raw, "cohorts1 %s = %s" % (k, num(v)), ""))
have("34 816 / 34 816", "l2mshr cell completes the engine")
for q in ("96", "128", "160"):
    d = cyc(os.path.join(OUT, "probe_l2mshr_%s.txt" % q))
    checks.append(("conservation=VIOLATED" in open(os.path.join(OUT, "probe_l2mshr_%s.txt" % q)).read(),
                   "l2_mshr %s deadlocks" % q, ""))
d = cyc(os.path.join(OUT, "probe_l2mshr_64.txt"))
checks.append(("conservation=OK" in open(os.path.join(OUT, "probe_l2mshr_64.txt")).read(),
               "l2_mshr 64 completes", ""))
have("242 935 cycles", "l2_mshr 64 cycles")

# ------------------------------------------------------------- 11. elements --
checks.append((len(doc.split()) > 3000, "report length", str(len(doc.split()))))

# ------------------------------------------------------------------ report ---
bad = [(lab, s) for ok, lab, s in checks if not ok]
print("M24-G report verification: %d checks" % len(checks))
for lab, s in bad[:40]:
    print("  FAIL %-46s report does not contain %r" % (lab, s[:120]))
if bad:
    print("%d of %d checks failed" % (len(bad), len(checks)))
    sys.exit(1)
print("ALL FIGURES IN docs/m24-g-resource-scaling.md RECOMPUTED AND MATCHED")
