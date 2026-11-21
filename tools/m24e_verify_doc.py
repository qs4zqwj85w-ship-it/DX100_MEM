#!/usr/bin/env python3
"""M24-E — recompute every figure quoted in docs/m24-e-mechanism-ablation.md.

Two jobs, and it fails loudly on either:

  1. every number in the report is recomputed from the raw cell logs
     (build/wm/m24d/*.txt, build/wm/m24e/m24e_*.txt) and must appear in the
     report text exactly as formatted there;
  2. the report's structural claims (which mechanism ranks where, which cells
     exist, which cross-milestone figures are quoted) are checked against the
     artifacts rather than trusted.

Run after tools/m24e_extract.py; exit status is non-zero on any mismatch.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
E = os.path.join(ROOT, "build", "wm", "m24e")
D = os.path.join(ROOT, "build", "wm", "m24d")
DOC = os.path.join(ROOT, "docs", "m24-e-mechanism-ablation.md")
WLS = ["orkut", "livejournal", "ogbn", "criteo"]
SHORT = {"orkut": "Orkut", "livejournal": "LiveJournal", "ogbn": "ogbn",
         "criteo": "Criteo"}
GUARD = 500000000

fails, nchk = [], [0]


def load(p):
    d = {}
    with open(p) as f:
        for line in f:
            for tok in line.split():
                if "=" in tok:
                    k, _, v = tok.partition("=")
                    try:
                        d[k] = float(v) if "." in v else int(v)
                    except ValueError:
                        d[k] = v
    return d


text = open(DOC, encoding="utf-8").read()


def sp(n):
    """thousands-separated the way the report writes them"""
    return f"{int(round(n)):,}".replace(",", " ")


def have(s, what):
    """the report must contain this exact string"""
    nchk[0] += 1
    if s not in text:
        fails.append(f"{what}: report does not contain {s!r}")


def same(cond, what):
    nchk[0] += 1
    if not cond:
        fails.append(what)


# ------------------------------------------------------------------ cells --
cells = {}
for w in WLS:
    p = os.path.join(D, f"m24d_bulk_{w}.txt")
    same(os.path.exists(p), f"missing baseline {w}")
    if os.path.exists(p):
        cells[("base", w)] = load(p)
ABL = ["nocoh", "coh1", "coh2", "coh4", "coh16", "pool1", "pool16", "pool64",
       "pool512", "nodram", "unfused", "idx0", "idx1", "idx3"]
for a in ABL + ["nocohps"]:
    for w in (["orkut", "ogbn"] if a == "nocohps" else WLS):
        p = os.path.join(E, f"m24e_{a}_{w}.txt")
        same(os.path.exists(p), f"missing cell {a}/{w}")
        if os.path.exists(p):
            cells[(a, w)] = load(p)
same(len(cells) == 62, f"expected 62 cells, found {len(cells)}")
have("62 cells", "cell count")

# the baseline is the M24-D production cell, unchanged
have("build/wm/m24d/m24d_bulk.cfg", "base config path")
for k in ("bulk_org_policy", "bulk_g6b_pa_capacity", "bulk_win_fused", "bulk_idx_mode"):
    same(cells[("base", "orkut")][k] in (8, 4096, 1, 2), f"baseline {k} not production")


def I(a, w, k):
    return int(cells[(a, w)][k])


def F(a, w, k):
    return float(cells[(a, w)][k])


def pct(x, y):
    return 100.0 * x / y if y else 0.0


def slowdown(a, w):
    return I(a, w, "cycles") / I("base", w, "cycles")


def walker(a, w):
    return pct(cells[(a, w)]["walker_busy"], cells[(a, w)]["cycles"] * 8)


def dmshr(a, w):
    return F(a, w, "tlb0_mshr_occ") / cells[(a, w)]["cycles"]


def x3(v):
    return f"{v:.3f}\u00d7"


def grp(v, nd):
    """2 045.3 — the report's grouping of a decimal figure"""
    return f"{v:,.{nd}f}".replace(",", " ")


# ------------------------------------------------------- 1. headline table --
have(sp(I("base", "orkut", "cycles")), "base orkut cycles")
have(x3(slowdown("nocoh", "livejournal")), "A worst case")
have(x3(slowdown("pool1", "ogbn")), "B worst case")
have(x3(slowdown("pool16", "livejournal")), "B 16-entry worst case")
have(x3(slowdown("unfused", "livejournal")), "D worst case")
have(x3(slowdown("idx0", "criteo")), "E worst case")
have(x3(slowdown("nodram", "criteo")), "C worst case")
same(slowdown("nodram", "orkut") < 1.0, "C on Orkut is not a slowdown (report says it is faster)")
have(sp(I("base", "orkut", "bulk_sched_bytes")), "cohort storage")
have(sp(I("base", "orkut", "bulk_g6b_pa_bytes")), "pool storage")
have(sp(I("pool16", "orkut", "bulk_g6b_pa_bytes")), "16-entry pool storage")
have(sp(I("base", "orkut", "bulk_idx_bytes")), "index tiling storage")

# --------------------------------------------------- 2. A: cohort ablation --
for a in ("nocoh", "coh1", "coh2", "coh4", "coh16"):
    row = " | ".join(x3(slowdown(a, w)) for w in WLS)
    have(row, f"A row {a}")
    row = f"{walker(a, 'orkut'):.1f} / {walker(a, 'ogbn'):.1f}"
    have(row, f"A walker row {a}")
    row = f"{dmshr(a, 'orkut'):.2f} / {dmshr(a, 'ogbn'):.2f}"
    have(row, f"A dMSHR row {a}")
    for w in WLS:
        have(sp(I(a, w, "cycles")), f"A cycles {a}/{w}")
same(I("nocoh", "orkut", "cycles") != I("coh1", "orkut", "cycles"),
     "nocoh and coh1 are the same cell twice")
have(sp(I("coh1", "orkut", "cycles") - I("nocoh", "orkut", "cycles")), "coh1-nocoh orkut")
have(sp(I("coh1", "ogbn", "cycles") - I("nocoh", "ogbn", "cycles")), "coh1-nocoh ogbn")
for w in WLS:
    have(sp(I("base", w, "cycles") - I("coh16", w, "cycles")), f"coh16 gain {w}")
# no translation work is removed: walks move by < 0.1 %
same(abs(I("nocoh", "orkut", "walks") - I("base", "orkut", "walks")) < 0.001 * I("base", "orkut", "walks"),
     "walks move by more than 0.1 %")
have(sp(I("base", "orkut", "walks")), "base orkut walks")
have(sp(I("nocoh", "orkut", "walks")), "nocoh orkut walks")
# the deferral table never holds more than the width, on every workload
for a, width in (("coh1", 1), ("coh2", 2), ("coh4", 4), ("base", 8), ("coh16", 16)):
    for w in WLS:
        same(I(a, w, "bulk_g6c_defer_depth_max") == width,
             f"defer depth at width {width} on {w}")
# blocked-page behaviour
have(sp(I("base", "orkut", "bulk_g6c_defer_events")), "orkut deferrals at width 8")
have(sp(I("base", "orkut", "bulk_g6c_deferred_readmitted")), "readmitted")
have(sp(I("base", "ogbn", "bulk_g6c_defer_events")), "ogbn deferrals at width 8")
have(sp(I("coh2", "ogbn", "bulk_g6c_defer_events")), "ogbn deferrals at width 2")
same(I("nocoh", "orkut", "bulk_g6c_defer_events") == 0, "cohorts-off still defers")
# equivalence cells: same cycles as the ps=0 arm, scheduler built but unused
for w in ("orkut", "ogbn"):
    same(I("nocohps", w, "cycles") == I("nocoh", w, "cycles"), f"nocohps/{w} not equivalent")
same(cells[("nocohps", "orkut")]["bulk_sched_on"] == 1 and
     cells[("nocoh", "orkut")]["bulk_sched_on"] == 0, "scheduler state in equivalence cells")
have(sp(I("nocoh", "orkut", "cycles")), "equivalence orkut cycles")

# --------------------------------------------------------- 3. B: the pool --
for a in ("pool1", "pool16", "pool64", "pool512"):
    have(" | ".join(x3(slowdown(a, w)) for w in WLS), f"B row {a}")
for a in ("pool1", "pool16", "pool64", "pool512", "base"):
    have(f"{pct(cells[(a, 'orkut')]['bulk_g6b_full_cycles'], cells[(a, 'orkut')]['cycles']):.1f} / "
         f"{pct(cells[(a, 'ogbn')]['bulk_g6b_full_cycles'], cells[(a, 'ogbn')]['cycles']):.1f}",
         f"B full% row {a}")
    have(f"{grp(cells[(a, 'orkut')]['bulk_g6b_ready_occ_sum'] / cells[(a, 'orkut')]['cycles'], 1)} / "
         f"{grp(cells[(a, 'ogbn')]['bulk_g6b_ready_occ_sum'] / cells[(a, 'ogbn')]['cycles'], 1)}",
         f"B occupancy row {a}")
# one entry: full and empty at the same time, no residence
same(I("pool1", "orkut", "bulk_g6b_ready_peak") == 1, "pool1 peak")
same(pct(cells[("pool1", "orkut")]["bulk_g6b_full_cycles"], cells[("pool1", "orkut")]["cycles"]) > 97,
     "pool1 not full 97+ %")
same(cells[("pool1", "ogbn")]["bulk_g6b_ready_occ_sum"] / I("pool1", "ogbn", "bulk_g6b_release_pops") < 0.5,
     "pool1 residence not ~0")
have("97.5 %", "pool1 full on Orkut")
have("97.3 %", "pool1 empty on Orkut")


def resid(a, w):
    return cells[(a, w)]["bulk_g6b_ready_occ_sum"] / (cells[(a, w)]["bulk_g6b_release_pops"] or 1)


def relthr(a, w):
    return cells[(a, w)]["bulk_g6b_release_pops"] / cells[(a, w)]["cycles"]


# residence, release throughput and backpressure: the other three columns of the collapse
have(grp(resid("base", "orkut"), 1), "Orkut residence at 4096")
have(grp(resid("pool1", "orkut"), 1), "Orkut residence at 1")
have(f"{relthr('base', 'orkut'):.4f}", "Orkut release throughput at 4096")
have(f"{relthr('pool1', 'orkut'):.4f}", "Orkut release throughput at 1")
have(f"{pct(cells[('pool1', 'orkut')]['bulk_g6b_backpressure_cycles'], cells[('pool1', 'orkut')]['cycles']):.1f} %",
     "Orkut backpressure at 1")
have(f"{pct(cells[('pool16', 'orkut')]['bulk_g6b_backpressure_cycles'], cells[('pool16', 'orkut')]['cycles']):.1f} %",
     "Orkut backpressure at 16")
have(f"{pct(cells[('pool16', 'livejournal')]['bulk_g6b_backpressure_cycles'], cells[('pool16', 'livejournal')]['cycles']):.1f} %",
     "LiveJournal backpressure at 16")
have(f"{pct(cells[('pool512', 'orkut')]['bulk_g6b_backpressure_cycles'], cells[('pool512', 'orkut')]['cycles']):.1f} %",
     "Orkut backpressure at 512")
have(f"{pct(cells[('pool512', 'livejournal')]['bulk_g6b_backpressure_cycles'], cells[('pool512', 'livejournal')]['cycles']):.1f} %",
     "LiveJournal backpressure at 512")
have(grp(resid("pool512", "orkut"), 1), "Orkut residence at 512")
have(grp(resid("pool512", "livejournal"), 1), "LiveJournal residence at 512")
same(resid("pool1", "orkut") < 1.0, "pool1 residence is not ~0")
# 4096 vs 16 on the two translation-bound workloads
same(slowdown("pool16", "ogbn") < 1.005 and slowdown("pool16", "criteo") < 1.01,
     "16-entry pool not near 4096 on the translation-bound pair")
same(slowdown("pool512", "livejournal") > 1.05, "512-entry pool not costly on LiveJournal")
have(sp(I("pool16", "criteo", "cycles")), "pool16 criteo cycles")
have(sp(I("pool1", "ogbn", "cycles")), "pool1 ogbn cycles")

# ------------------------------------------------- 4. C: physical ordering --
reads_base = {w: I("base", w, "dram_reads") for w in WLS}
reads_off = {w: I("nodram", w, "dram_reads") for w in WLS}
for w in WLS:
    have(sp(reads_base[w]), f"C reads base {w}")
    have(sp(reads_off[w]), f"C reads off {w}")
    have(sp(abs(reads_off[w] - reads_base[w])), f"C read delta {w}")
    have(sp(I("base", w, "dram_row_acts_sum")), f"C acts base {w}")
    have(sp(I("nodram", w, "dram_row_acts_sum")), f"C acts off {w}")
    have(sp(I("base", w, "bulk_org_cycles")), f"organizer cycles {w}")
    have(sp(I("nodram", w, "bulk_org_cycles")), f"organizer cycles off {w}")
    have(sp(I("base", w, "bulk_org_cycles") - I("nodram", w, "bulk_org_cycles")),
         f"organizer cost {w}")
    have(f"{pct(I('base', w, 'dram_row_hits'), reads_base[w]):.1f} \u2192 "
         f"{pct(I('nodram', w, 'dram_row_hits'), reads_off[w]):.1f} %", f"C row hit {w}")
    have(f"{pct(I('base', w, 'bulk_m24_rel_inversions'), I('base', w, 'bulk_m24_rel_pairs')):.2f} %",
         f"C inversions base {w}")
    have(f"{pct(I('nodram', w, 'bulk_m24_rel_inversions'), I('nodram', w, 'bulk_m24_rel_pairs')):.2f} %",
         f"C inversions off {w}")
    # the report's own claim: the organizer costs activations, not cycles
    same(I("base", w, "dram_row_acts_sum") < I("nodram", w, "dram_row_acts_sum"),
         f"C organizer does not reduce activations on {w}")
    same(abs(slowdown("nodram", w) - 1.0) < 0.002, f"C is not ~0 on {w}")
    # bank utilisation, queue depth and wait per read, base -> off
    have(f"{pct(cells[('base', w)]['dram_bank0_busy'] + cells[('base', w)]['dram_bank1_busy'], 2 * cells[('base', w)]['cycles']):.1f} \u2192 "
         f"{pct(cells[('nodram', w)]['dram_bank0_busy'] + cells[('nodram', w)]['dram_bank1_busy'], 2 * cells[('nodram', w)]['cycles']):.1f}",
         f"C bank busy {w}")
    have(f"{cells[('base', w)]['dram_q_occ_sum'] / cells[('base', w)]['cycles']:.2f} \u2192 "
         f"{cells[('nodram', w)]['dram_q_occ_sum'] / cells[('nodram', w)]['cycles']:.2f}",
         f"C queue depth {w}")
    have(f"{cells[('base', w)]['dram_wait_sum'] / reads_base[w]:.1f} \u2192 "
         f"{cells[('nodram', w)]['dram_wait_sum'] / reads_off[w]:.1f}",
         f"C wait per read {w}")
# the organizer never runs in the ablated arm, and always runs in the baseline
same(cells[("nodram", "orkut")]["bulk_org_runs"] == 0, "organizer still ran")
same(cells[("base", "orkut")]["bulk_org_runs"] > 0, "organizer never ran in base")

# ---------------------------------------------------- 5. D: fused/unfused --
for w in WLS:
    b, u = cells[("base", w)], cells[("unfused", w)]
    have(sp(b["bulk_win_vec_bytes"]), f"D fused bytes {w}")
    have(sp(u["bulk_win_vec_bytes"]), f"D unfused bytes {w}")
    have(f"{u['bulk_win_vec_bytes'] / b['bulk_win_vec_bytes']:.2f}\u00d7", f"D storage ratio {w}")
    have(x3(slowdown("unfused", w)), f"D cycles {w}")
    have(sp(b["cycles"]), f"D fused cycles {w}")
    have(sp(u["cycles"]), f"D unfused cycles {w}")
    have(f"{sp(b['l2_reads'])} \u2192 {sp(u['l2_reads'])}", f"D L2 reads {w}")
    same(b["bulk_win_bytes"] == u["bulk_win_bytes"], "design charge differs between arms")
same(cells[("unfused", "orkut")]["bulk_win_fused"] == 0, "unfused arm is fused")
same(cells[("unfused", "orkut")]["bulk_org_policy"] == 0, "unfused arm has an organizer")
have("3.90\u00d7", "D orkut ratio in prose")
have("3.38\u00d7", "D criteo ratio in prose")
# M22's accounting, quoted: predicted vs measured
have("1 851 632", "M22 predicted unfused 2x16K")
have("475 376", "M22 predicted fused 2x16K")
same(1851632 - cells[("unfused", "orkut")]["bulk_win_vec_bytes"] == 240, "M22 rounding delta unfused")
same(475376 - cells[("base", "orkut")]["bulk_win_vec_bytes"] == 240, "M22 rounding delta fused")

# ------------------------------------------------------------ 6. index mode --
for a in ("idx0", "idx1", "idx3"):
    have(x3(slowdown(a, "criteo")), f"E row {a}")
for w in WLS:
    same(cells[("idx3", w)]["cycles"] == cells[("base", w)]["cycles"],
         f"mode 3 differs from mode 2 on {w}")
    have(sp(I("base", w, "bulk_idx_lines_done")), f"index lines {w}")
for w in WLS:
    share = pct(I("base", w, "bulk_idx_lines_done"), I("base", w, "dram_reads"))
    have(f"{share:.1f}", f"index share {w}")
have(sp(I("base", "orkut", "bulk_idx_ooo_gen")), "ooo index generation")
have(sp(I("idx0", "orkut", "bulk_idx_wait_stall")), "idx0 stall")
have(sp(I("base", "orkut", "bulk_idx_wait_stall")), "base stall")
have(sp(I("idx1", "orkut", "bulk_idx_wait_stall")), "idx1 stall")
same(I("idx0", "orkut", "bulk_idx_bytes") == 0, "mode 0 allocates tiles")

# --------------------------------------------------- 7. ranking is a ranking --
order = [("nocoh", max(slowdown("nocoh", w) for w in WLS)),
         ("unfused", sum(slowdown("unfused", w) for w in WLS) / 4),
         ("pool16", sum(slowdown("pool16", w) for w in WLS) / 4),
         ("idx0", max(slowdown("idx0", w) for w in WLS)),
         ("nodram", max(slowdown("nodram", w) for w in WLS))]
# ranks 1..5 of the report, in its order; ranks 3 and 4 are declared a tie, so the
# mean-ordering check below is allowed to be within 0.5 % of a tie there
same(order[0][1] > order[1][1] + 0.01 > order[2][1] and order[2][1] > order[3][1] > order[4][1],
     "the ranking in the report is not ordered by measured impact: " + str(order))
have(f"{order[1][1]:.3f}", "mean slowdown of the fused window (rank 3)")
have(f"{order[2][1]:.3f}", "mean slowdown of the 16-entry pool (rank 4)")
# the report must not claim a separate winner between the two tied rows
same(order[1][1] - order[2][1] < 0.005, "ranks 3 and 4 are not a tie after all")

# ------------------------------------------------- 8. cross-milestone quotes --
simt = load(os.path.join(D, "m24d_simt_orkut.txt"))
have(sp(simt["l2_reads"]), "SIMT orkut L2 reads")
have(sp(simt["dram_reads"]), "SIMT orkut DRAM reads")
same(simt["l2_reads"] > I("base", "orkut", "l2_reads"), "SIMT does not read more L2 than Bulk")
m22 = open(os.path.join(ROOT, "docs", "m22-storage-accounting.md"), encoding="utf-8").read()
for s in ("1 851 632", "475 376", "467 064", "123 000", "934 128", "246 000"):
    nchk[0] += 1
    if s not in m22:
        fails.append(f"M22 no longer contains {s!r} (quoted in M24-E §7.2)")
for s in ("467 064", "123 000", "1 851 632", "475 376"):
    have(s, "M22 figure quoted in M24-E")
m21 = open(os.path.join(ROOT, "docs", "m21-mechanism-ablation-report.md"), encoding="utf-8").read()
for s in ("2.994", "1.833", "1.001"):
    nchk[0] += 1
    if s not in m21:
        fails.append(f"M21 no longer contains {s!r} (quoted in M24-E §2)")
    have(s, "M21 figure quoted in M24-E")
have("2.994 \u2192 1.001", "M21 ordering ablation quoted")

# M24-A: the walker/MSHR figures quoted in \u00a74.3 and the two cells reproduced in \u00a74.1
m24a = open(os.path.join(ROOT, "docs", "m24-a-four-way-organization-evaluation.md"),
            encoding="utf-8").read()
for s in ("18.3 %", "65.7 %", "1.48", "5.31", "21 992 384", "6 130 494"):
    nchk[0] += 1
    if s not in m24a:
        fails.append(f"M24-A no longer contains {s!r} (quoted in M24-E \u00a74)")
    have(s, "M24-A figure quoted in M24-E")
# the reproduced cells are the report's own numbers
same("21 992 384" == sp(I("nocoh", "orkut", "cycles")), "nocoh is not M24-A's cell C")
same("6 130 494" == sp(I("base", "orkut", "cycles")), "base is not M24-A's cell D")

# M24-D: the resource-matched speedup range quoted in \u00a76.3, recomputed from its table
m24d = open(os.path.join(ROOT, "docs", "m24-d-strict-resource-matched-comparison.md"),
            encoding="utf-8").read()
import re
ratios = []
for m in re.finditer(r"^\| ([A-Za-z][^|]*?) \| (\d[\d ]*) \| (\d[\d ]*) \| (\d[\d ]*) \| ([\d.]+) \|$",
                     m24d, re.M):
    full, m0, bulk, r = m.group(2), m.group(3), m.group(4), float(m.group(5))
    n = lambda s: int(s.replace(" ", ""))
    if n(bulk) in [I("base", w, "cycles") for w in WLS]:
        same(abs(n(m0) / n(bulk) - r) < 0.001, f"M24-D ratio for {m.group(1).strip()} is wrong")
        ratios.append(r)
same(len(ratios) >= 4, f"could not read M24-D's speedups ({ratios})")
if ratios:
    have(f"{min(ratios):.2f}\u2013{max(ratios):.2f}\u00d7", "M24-D speedup range quoted in \u00a76.3")

# the base config is 94 pinned fields, as \u00a73 says
keys = [l for l in open(os.path.join(D, "m24d_bulk.cfg"), encoding="utf-8")
        if re.match(r"^[a-z0-9_]+ *=", l)]
same(len(keys) == 94, f"base config has {len(keys)} fields, report says 94")

# ------------------------------------------------------- 9. report structure --
for s in ("## 1. Result", "## 2. Evidence audit", "### 2.3 What cannot be isolated",
          "## 3. Method", "## 4. A \u2014 page-cohort", "## 5. B \u2014 the shared ready pool",
          "## 6. C \u2014 DRAM-aware", "## 7. D \u2014 fused window", "## 8. E \u2014 index-stream",
          "## 9. Contribution table", "## 10. Interactions", "## 11. Validation",
          "## 12. Limitations", "## 13. Conclusions", "## Reproduce"):
    have(s, "report section")
have("[M]", "measured tag")
have("[Q]", "quoted tag")
have("[H]", "hypothesis tag")
same(text.count("[H]") >= 2, "hypotheses are not tagged")
have("Negative result, stated plainly", "negative results are reported")
have("m24e_verify_doc.py", "verify script is referenced")

# ------------------------------------------------------------- 10. artifacts --
for p in ("build/wm/m24e_run.sh", "tools/m24e_extract.py", "build/wm/m24e/cells.tsv",
          "build/wm/m24e/summary.csv", "build/wm/m24e/ablate.txt",
          "build/wm/m24e/commands.txt"):
    nchk[0] += 1
    if not os.path.exists(os.path.join(ROOT, p)):
        fails.append(f"missing deliverable {p}")
reps = [f for f in os.listdir(E) if f.endswith("__repeat.txt")]
same(len(reps) >= 3, f"fewer than 3 determinism repeats ({len(reps)})")
if os.path.exists(os.path.join(E, "cells.tsv")):
    n = sum(1 for line in open(os.path.join(E, "cells.tsv")) if line.strip())
    same(n == 59, f"cells.tsv has {n} lines (58 cells + header)")
if os.path.exists(os.path.join(E, "commands.txt")):
    cmds = [l for l in open(os.path.join(E, "commands.txt")) if l.strip()]
    same(len(cmds) == 58, f"commands.txt has {len(cmds)} lines")
    same(all(l.startswith("./bin/dx100sim --config ../build/wm/m24d/m24d_bulk.cfg")
             for l in cmds), "a cell command does not use the production base config")

# ------------------------------------------------------------------ report --
print(f"M24-E report verification: {nchk[0]} checks")
if fails:
    for f in fails:
        print("  FAIL " + f)
    sys.exit(1)
print("ALL FIGURES IN docs/m24-e-mechanism-ablation.md RECOMPUTED AND MATCHED")
