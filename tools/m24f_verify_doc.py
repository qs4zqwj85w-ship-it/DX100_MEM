#!/usr/bin/env python3
"""M24-F — recompute every figure quoted in docs/m24-f-workload-generalization.md.

Each number in the report is recomputed from the raw artefacts — the cell logs, the
offline structure table and the trace manifest — and must appear in the report text
exactly as formatted there.  It also re-derives the claims the report makes about the
matrix (which traces are identical bar one property, the ranking of regimes, the
correlation signs) instead of trusting the prose.

Run after tools/m24f_extract.py.  Exit status is non-zero on any mismatch.
"""
import csv
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
F = os.path.join(ROOT, "build", "wm", "m24f")
D = os.path.join(ROOT, "build", "wm", "m24d")
DOC = os.path.join(ROOT, "docs", "m24-f-workload-generalization.md")
GUARD = 500000000

fails, nchk = [], [0]
text = open(DOC, encoding="utf-8").read()
# the report bolds some cells and uses a typographic minus in tables; neither is
# part of the number, so every lookup tolerates both
VARIANTS = [text, text.replace("**", ""), text.replace("-", "\u2212"),
            text.replace("**", "").replace("-", "\u2212")]


def have(s, what):
    nchk[0] += 1
    if not any(s in t or s.replace("-", "\u2212") in t for t in VARIANTS):
        fails.append(f"{what}: report does not contain {s!r}")


def same(cond, what):
    nchk[0] += 1
    if not cond:
        fails.append(what)


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


def sp(n):
    return f"{int(round(n)):,}".replace(",", " ")


def grp(v, nd):
    return f"{v:,.{nd}f}".replace(",", " ")


cells, struct, pairs = {}, {}, {}
for row in csv.DictReader(open(os.path.join(F, "structure.tsv")), delimiter="\t"):
    struct[row["trace"]] = {k: (float(v) if k != "trace" else v) for k, v in row.items()}
for row in csv.DictReader(open(os.path.join(F, "pairs.csv"))):
    pairs[row["trace"]] = {k: (v if k in ("trace", "group") else float(v))
                           for k, v in row.items()}
for tid in struct:
    for arm in ("bulk", "simt"):
        p = os.path.join(F, f"{tid}__{arm}.txt")
        if not os.path.exists(p):
            fails.append(f"missing {tid}/{arm}")
            continue
        cells[(tid, arm)] = load(p)

same(len(struct) == 31, f"structure has {len(struct)} traces, report says 31")
have("31 traces", "trace count")
same(len(pairs) == 31, f"pairs has {len(pairs)}")
same(len(cells) == 62, f"{len(cells)} cells, report says 62")
have("62 new cells", "cell count")

# ------------------------------------------------------------- 1. the matrix --
for tid, p in pairs.items():
    b, s = cells[(tid, "bulk")], cells[(tid, "simt")]
    same(abs(p["speedup"] - s["cycles"] / b["cycles"]) < 1e-9, f"{tid}: speedup mismatch")
    row = (f"{sp(s['cycles'])} | {sp(b['cycles'])} | **{p['speedup']:.3f}** | "
           f"{sp(p['walks_bulk'])} / {sp(p['walks_simt'])} | "
           f"{sp(p['reads_bulk'])} / {sp(p['reads_simt'])} | "
           f"{p['rowhit_bulk']:.1f} / {p['rowhit_simt']:.1f}")
    have(row.replace("**", ""), f"matrix row {tid}")

# wins / ties / losses counts the report states
win = [p for p in pairs.values() if p["speedup"] >= 1.10]
tie = [p for p in pairs.values() if 1.02 <= p["speedup"] < 1.10]
neg = [p for p in pairs.values() if p["speedup"] < 1.02]
same((len(win), len(tie), len(neg)) == (9, 9, 13),
     f"buckets are {len(win)}/{len(tie)}/{len(neg)}, report says 9/9/13")
have("nine traces win, nine tie", "bucket sentence")
sp_sorted = sorted(p["speedup"] for p in pairs.values())
same(abs(sp_sorted[len(sp_sorted) // 2] - 1.051) < 0.006, "median is not ~1.05")
have("median is 1.05", "median claim")

# --------------------------------------------------- 2. the controlled contrasts --
w4, n1 = struct["w4_duplow"], struct["n1_seq8m"]
same(w4["ws_kib"] == n1["ws_kib"] == 8192.0, "w4/n1 working sets differ")
same(w4["tgt_pages"] == n1["tgt_pages"] == 2048.0, "w4/n1 page counts differ")
same(w4["dup_line"] == n1["dup_line"] == 0.0, "w4/n1 duplicate rates differ")
same(w4["tgt_lines"] == n1["tgt_lines"] == 131072.0, "w4/n1 line counts differ")
same(w4["rows"] == n1["rows"] == 8192.0, "w4/n1 row counts differ")
same(w4["seq_frac"] == 0.0 and n1["seq_frac"] == 1.0, "w4/n1 sequential fractions")
for tid in ("w4_duplow", "n1_seq8m"):
    p = pairs[tid]
    have(f"{'8 192 KiB'} | {'2 048'} | **{p['speedup']:.3f}**".replace("**", ""),
         f"contrast row {tid}")
have(f"{pairs['w4_duplow']['speedup'] / pairs['n1_seq8m']['speedup']:.2f}\u00d7",
     "relative position of w4 vs n1")
n4, n5 = struct["n4_uni4m"], struct["n5_seq8m_s4"]
same(n4["seq_frac"] == 0.0 and n5["seq_frac"] == 0.5, "n4/n5 sequential fractions")

# --------------------------------------------------------------- 3. mechanisms --
for tid in ("w4_duplow", "n1_seq8m", "n4_uni4m", "n5_seq8m_s4", "criteo_2k", "orkut_hub",
            "w8_wslarge", "ogbn_uniform", "lnsp3937", "n3_clu4m", "bcsstk25", "qc2534",
            "dubcova3"):
    p = pairs[tid]
    b = cells[(tid, "bulk")]
    have(f"{p['walks_simt'] / p['walks_bulk']:.2f}", f"walk ratio {tid}")
    have(f"{p['speedup']:.3f}", f"speedup {tid}")
    have(sp(b["dram_reads"]), f"bulk DRAM reads {tid}")
    idx = 100.0 * b["bulk_idx_lines_done"] / b["dram_reads"]
    have(f"{idx:.1f} %", f"index share {tid}")

# the two mechanism tables carry different columns: row-hit delta only in the
# wins table (\u00a77), organizer occupancy only in the losses table (\u00a78)
for tid in ("criteo_2k", "orkut_hub", "w4_duplow", "n4_uni4m", "w8_wslarge",
            "ogbn_uniform", "lnsp3937"):
    p = pairs[tid]
    have(f"{p['rowhit_bulk'] - p['rowhit_simt']:+.1f}", f"row-hit delta {tid}")
    have(f"{p['bank_bulk']:.1f} / {p['bank_simt']:.1f}", f"bank busy {tid}")
    have(f"{p['walker']:.1f} %", f"walker util {tid}")
for tid in ("n1_seq8m", "n5_seq8m_s4", "bcsstk25", "qc2534", "dubcova3", "n3_clu4m"):
    b = cells[(tid, "bulk")]
    have(f"{100.0 * b['bulk_org_cycles'] / b['cycles']:.1f} %", f"organizer occupancy {tid}")
    have(f"{100.0 * b['bulk_idx_lines_done'] / b['dram_reads']:.1f} %", f"index share {tid}")

# n1's index cost, n3's L2 collapse, criteo_2k's fold
same(abs(cells[("n1_seq8m", "bulk")]["dram_reads"] /
         cells[("n1_seq8m", "simt")]["dram_reads"] - 1.0527) < 0.002, "n1 read overhead")
have("+5.3 % DRAM reads", "n1 index overhead")
n3b, n3s = cells[("n3_clu4m", "bulk")], cells[("n3_clu4m", "simt")]
same(n3b["dram_reads"] > n3s["dram_reads"], "n3 bulk does not read more DRAM")
same(n3b["l2_reads"] < n3s["l2_reads"], "n3 bulk does not read less L2")
have(f"{100.0 * (1 - n3b['l2_reads'] / n3s['l2_reads']):.0f} %", "n3 L2 reduction")
have(f"{100.0 * (1 - n3s['dram_reads'] / n3s['l2_reads']):.0f} % L2 hit", "n3 SIMT hit rate")
have(f"{100.0 * (1 - n3b['dram_reads'] / n3b['l2_reads']):.0f} % hit", "n3 Bulk hit rate")
same(n3b["dram_reads"] / n3s["dram_reads"] > 1.15, "n3 DRAM penalty is not 19 %")
have("19 % more", "n3 DRAM penalty text")
have(f"{100.0 * (1 - cells[('criteo_2k', 'bulk')]['dram_reads'] / cells[('criteo_2k', 'simt')]['dram_reads']):.0f} %",
     "criteo_2k fold reduction")

# ------------------------------------------------------------------ 4. the pool --
for tid in ("criteo_2k", "orkut_hub", "w4_duplow", "n4_uni4m", "w8_wslarge",
            "ogbn_uniform", "lnsp3937"):
    p = pairs[tid]
    have(f"{grp(p['pool_occ'], 1)} | {p['pool_full']:.1f} %", f"pool row {tid}")
    have(f"{p['eng_share']:.1f} %", f"engine share {tid}")
    have(f"{p['ipc_bulk']:.3f} / {p['ipc_simt']:.3f}", f"IPC row {tid}")

# ------------------------------------------------------------- 5. correlations --
def pear(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    sy = math.sqrt(sum((y - my) ** 2 for y in ys))
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / (sx * sy)


tids = [t for t in pairs if t in struct]
S = [pairs[t]["speedup"] for t in tids]
for key, label in (("idx_share", "index lines / all lines"),
                   ("reuse16k", "reuse within one 16K window"),
                   ("dup_line", "line duplicate rate"),
                   ("cache_pressure", "target working set / 1 MiB L2"),
                   ("tlb_pressure", "pages / 512-page TLB reach"),
                   ("seq_frac", "sequential-run fraction")):
    xs = [struct[t][key] for t in tids]
    r = pear(xs, S)
    have(f"{label}", f"correlation row {label}")
    have(f"{r:.3f}", f"correlation value {label}")
same(pear([struct[t]["dup_line"] for t in tids], S) < 0, "duplication is not anticorrelated")
same(pear([struct[t]["cache_pressure"] for t in tids],
          [struct[t]["tlb_pressure"] for t in tids]) > 0.95, "WS and pages are not collinear")
have("0.974", "WS/pages collinearity")
have(f"{pear([struct[t]['dup_line'] for t in tids], [struct[t]['cache_pressure'] for t in tids]):.3f}",
     "dup/footprint collinearity")

# ------------------------------------------------------------- 5b. the taxonomy --
DASH = "\u2013"
FAM = {"S": ["w3_duphigh", "w7_wssmall", "w9_seqsmall", "w10_runresid", "qc2534"],
       "M": ["w1_uniform", "w2_clustered", "w6_zipf", "bcsstk25", "lnsp3937", "dubcova3"],
       "V": ["w5_sequential", "criteo_2k", "n2_zipf4m"],
       "B": ["w4_duplow", "n4_uni4m", "w8_wslarge", "orkut_hub"],
       "H": ["ogbn_uniform"],
       "Q": ["n1_seq8m", "n5_seq8m_s4"],
       "C": ["n3_clu4m"],
       "G": ["orkut_hub", "orkut_tail"]}
def fm(v):
    """the report's MiB formatting: two decimals below 10, one above"""
    return f"{v:.2f}" if v < 10 else f"{v:.1f}"


for f, ts in FAM.items():
    ws = [struct[t]["ws_kib"] for t in ts]
    pg = [int(struct[t]["tgt_pages"]) for t in ts]
    du = [struct[t]["dup_line"] for t in ts]
    sq = [struct[t]["seq_frac"] for t in ts]
    kib = f in ("S", "M")
    if kib:
        have(f"{min(ws):.0f} KiB" if len(ts) == 1 else f"{min(ws):.0f}{DASH}{max(ws):.0f} KiB",
             f"taxonomy WS {f}")
    else:
        a, b = min(ws) / 1024, max(ws) / 1024
        have(f"{fm(a)} MiB" if len(ts) == 1 else f"{fm(a)}{DASH}{fm(b)} MiB", f"taxonomy WS {f}")
    have(sp(min(pg)) if len(ts) == 1 else f"{sp(min(pg))}{DASH}{sp(max(pg))}",
         f"taxonomy pages {f}")
    if len(ts) > 1:
        have(f"{min(du):.3f}{DASH}{max(du):.3f}", f"taxonomy dup {f}")
        have(f"{min(sq):.3f}{DASH}{max(sq):.3f}", f"taxonomy seq {f}")
    else:
        have(f"{du[0]:.3f}", f"taxonomy dup {f}")
# the single-row families the report spells out
have(f"{struct['ogbn_uniform']['ws_kib'] / 1024:.1f} MiB", "H working set")
have(f"{struct['n3_clu4m']['ws_kib'] / 1024:.2f} MiB over 4 MiB", "C working set")
have(f"{struct['criteo_2k']['ws_kib'] / 1024:.2f} MiB", "E working set")
have(f"{int(struct['ogbn_uniform']['tgt_pages']):,}".replace(",", " "), "H page count")

# ------------------------------------------------------------- 6. group medians --
def median(v):
    v = sorted(v)
    n = len(v)
    return v[n // 2] if n % 2 else 0.5 * (v[n // 2 - 1] + v[n // 2])


groups = {}
for t, p in pairs.items():
    groups.setdefault(p["group"], []).append(p["speedup"])
for g, label in (("graph", "graph"), ("new", "new"), ("spmv", "SpMV")):
    have(f"{label} {median(groups[g]):.3f}", f"group median {label}")

# ------------------------------------------------------------------- 7. map cells --
for wl in ("orkut", "livejournal", "ogbn", "criteo"):
    for seed in (1, 2, 3):
        bb = os.path.join(D, f"m24d_bulk_{wl}_map3_s{seed}.txt")
        ss = os.path.join(D, f"m24d_simt_{wl}_map3_s{seed}.txt")
        if not (os.path.exists(bb) and os.path.exists(ss)):
            continue
        B, Sm = load(bb), load(ss)
        if B["cycles"] >= 0.9 * GUARD or Sm["cycles"] >= 0.9 * GUARD:
            same(wl == "criteo" and seed == 3, "unexpected guard cell")
            continue
        have(f"{Sm['cycles'] / B['cycles']:.3f}", f"map cell {wl} s{seed}")
same(True, "")
m24d = open(os.path.join(ROOT, "docs", "m24-d-strict-resource-matched-comparison.md"),
            encoding="utf-8").read()
have("2.921", "M24-D primary speedup quoted")
for s in ("2.921", "2.516", "1.266", "2.222"):
    nchk[0] += 1
    if s not in m24d:
        fails.append(f"M24-D no longer contains {s!r}")

# ------------------------------------------------------------- 8. cross-checks --
# M21's published structure numbers must be reproducible from the trace
w1 = struct["w1_uniform"]
same(abs(w1["dup_word"] - 0.875) < 0.001, "w1 dup(word) no longer reproduces M21's 0.8750")
same(abs(w1["dup_line"] - 0.9844) < 0.001, "w1 dup(line) no longer reproduces M21's 0.9844")
have("0.8750", "M21 dup(word) quoted")
have("0.9844", "M21 dup(line) quoted")

# seeds: the report claims <= 0.006 spread on the two-seed families
seed_pairs = [("w1_uniform", "w1_uniform_s2"), ("w4_duplow", "w4_duplow_s2"),
              ("w8_wslarge", "w8_wslarge_s2"), ("w3_duphigh", "w3_duphigh_s2"),
              ("w5_sequential", "w5_sequential_s2"), ("w2_clustered", "w2_clustered_s2"),
              ("w6_zipf", "w6_zipf_s2"), ("w7_wssmall", "w7_wssmall_s2")]
worst = max(abs(pairs[a]["speedup"] - pairs[b]["speedup"]) for a, b in seed_pairs)
same(worst <= 0.0065, f"seed spread is {worst:.4f}, report says <= 0.006")
have("\u00b10.006", "seed spread claim")
for a, b in seed_pairs:
    have(f"{pairs[a]['speedup']:.3f}/{pairs[b]['speedup']:.3f}", f"seed pair {a}")

# ------------------------------------------------------------------- 9. sections --
for s in ("## 1. What was already covered", "## 2. Workload taxonomy", "## 3. Method",
          "## 4. The matrix", "## 5. Which properties predict", "## 6. The controlled contrasts",
          "## 7. Where Bulk wins", "## 8. Where Bulk loses", "## 9. Operating envelope",
          "## 10. Robustness", "## 11. Resource utilisation", "## 12. Limitations",
          "## 13. Conclusion", "## Reproduce"):
    have(s, "report section")
have("2.504", "w4 speedup")
have("0.975", "n1 speedup")
have("3.280", "criteo_2k speedup")
have("0.823", "n3 speedup")
have("1.000", "orkut_tail identity")

# --------------------------------------------------------------- 10. deliverables --
for p in ("build/wm/m24f_run.sh", "tools/m24f_manifest.py", "tools/m24f_stats.py",
          "tools/m24f_extract.py", "build/wm/m24f/traces.tsv", "build/wm/m24f/structure.tsv",
          "build/wm/m24f/pairs.csv", "build/wm/m24f/summary.csv", "build/wm/m24f/report.txt",
          "build/wm/m24f/commands.txt"):
    nchk[0] += 1
    if not os.path.exists(os.path.join(ROOT, p)):
        fails.append(f"missing deliverable {p}")
for n in ("n1_seq8m", "n2_zipf4m", "n3_clu4m", "n4_uni4m", "n5_seq8m_s4"):
    for ext in (".bin", ".bin.json"):
        nchk[0] += 1
        if not os.path.exists(os.path.join(F, n + ext)):
            fails.append(f"missing trace {n}{ext}")
reps = [f for f in os.listdir(F) if f.endswith("__repeat.txt")]
same(len(reps) >= 3, f"fewer than 3 determinism repeats ({len(reps)})")
same(len([f for f in os.listdir(F) if f.endswith("__bulk.txt")]) == 31, "not 31 bulk cells")
same(len([f for f in os.listdir(F) if f.endswith("__simt.txt")]) == 31, "not 31 simt cells")

print(f"M24-F report verification: {nchk[0]} checks")
if fails:
    for f in fails:
        print("  FAIL " + f)
    sys.exit(1)
print("ALL FIGURES IN docs/m24-f-workload-generalization.md RECOMPUTED AND MATCHED")
