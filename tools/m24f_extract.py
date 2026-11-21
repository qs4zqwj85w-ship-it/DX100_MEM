#!/usr/bin/env python3
"""M24-F: extraction, pair validation and the generalization tables.

Reads
  build/wm/m24f/<id>__bulk.txt / __simt.txt      the 31-trace matrix (66 cells)
  build/wm/m24f/traces.tsv                       workload manifest
  build/wm/m24f/structure.tsv                    offline structure (m24f_stats)
  build/wm/m24d/m24d_{bulk,simt}_<wl>.txt        the four M24-D primaries (reused)
  build/wm/m24d/m24d_{bulk,simt}_<wl>_map3_s*.txt  the map-M3 cells (reused)

Writes
  build/wm/m24f/summary.csv    one row per (trace, arm)
  build/wm/m24f/pairs.csv      one row per (trace): properties + both arms
  build/wm/m24f/report.txt     the tables quoted in docs/m24-f-workload-generalization.md
"""
import csv
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
F = os.path.join(ROOT, "build", "wm", "m24f")
D = os.path.join(ROOT, "build", "wm", "m24d")
GUARD = 500000000
PRIMARY = [("orkut", "Orkut BFS S1"), ("livejournal", "LiveJournal"),
           ("ogbn", "ogbn-products"), ("criteo", "Criteo 16k")]
out_lines, bad = [], []


def out(s=""):
    out_lines.append(s)
    print(s)


def chk(ok, msg):
    if not ok:
        bad.append(msg)


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


def groups():
    """id -> (group, trace path) from the manifest"""
    g = {}
    with open(os.path.join(F, "traces.tsv")) as f:
        next(f)
        for line in f:
            c = line.rstrip("\n").split("\t")
            g[c[0]] = (c[1], c[2])
    return g


def structure():
    s = {}
    p = os.path.join(F, "structure.tsv")
    if not os.path.exists(p):
        return s
    with open(p) as f:
        r = csv.DictReader(f, delimiter="\t")
        for row in r:
            s[row["trace"]] = {k: (float(v) if k not in ("trace",) else v)
                               for k, v in row.items()}
    return s


G = groups()
S = structure()

# ---------------------------------------------------------------- load cells --
cells = {}
for tid in G:
    for arm in ("bulk", "simt"):
        p = os.path.join(F, f"{tid}__{arm}.txt")
        if not os.path.exists(p):
            bad.append(f"missing cell {tid}/{arm}")
            continue
        d = load(p)
        if "cycles" not in d:
            bad.append(f"incomplete cell {tid}/{arm}")
            continue
        cells[(tid, arm)] = d
for wl, _ in PRIMARY:
    for arm, f in (("bulk", f"m24d_bulk_{wl}.txt"), ("simt", f"m24d_simt_{wl}.txt")):
        p = os.path.join(D, f)
        if os.path.exists(p):
            cells[("primary:" + wl, arm)] = load(p)

maps = {}
for wl, _ in PRIMARY:
    for arm in ("bulk", "simt"):
        for s in (1, 2, 3):
            p = os.path.join(D, f"m24d_{arm}_{wl}_map3_s{s}.txt")
            if os.path.exists(p):
                maps[(wl, arm, s)] = load(p)

# ------------------------------------------------------------- pair validation --
for tid in G:
    b, s = cells.get((tid, "bulk")), cells.get((tid, "simt"))
    if not b or not s:
        continue
    chk(b["cycles"] < GUARD and s["cycles"] < GUARD, f"{tid}: guard")
    chk(b.get("conservation_ok") == 1, f"{tid}: bulk conservation")
    chk(s.get("conservation_ok") == 1, f"{tid}: simt conservation")
    # both arms must have run the same trace
    chk(b["trace_accesses"] == s["trace_accesses"], f"{tid}: access count differs")
    chk(b["accesses"] == s["accesses"], f"{tid}: element count differs")
    chk(b.get("dispatch") == 1 and s.get("dispatch") == 0, f"{tid}: dispatch fields")
    if b.get("bulk_elems_total"):
        chk(b["bulk_elems_done"] == b["bulk_elems_total"], f"{tid}: elements not all served")
    chk(b["dram_row_hits"] + b["dram_row_conflicts"] + 2 == b["dram_reads"], f"{tid}: row sum")


def pct(x, y):
    return 100.0 * x / y if y else 0.0


def row(tid, b, s):
    return dict(
        trace=tid, group=G[tid][0],
        cycles_bulk=b["cycles"], cycles_simt=s["cycles"],
        speedup=s["cycles"] / b["cycles"],
        walks_bulk=b["walks"], walks_simt=s["walks"],
        tlbmiss_bulk=b["tlb0_misses"], tlbmiss_simt=s["tlb0_misses"],
        reads_bulk=b["dram_reads"], reads_simt=s["dram_reads"],
        rowhit_bulk=pct(b["dram_row_hits"], b["dram_reads"]),
        rowhit_simt=pct(s["dram_row_hits"], s["dram_reads"]),
        bank_bulk=pct(b["dram_bank0_busy"] + b["dram_bank1_busy"], 2 * b["cycles"]),
        bank_simt=pct(s["dram_bank0_busy"] + s["dram_bank1_busy"], 2 * s["cycles"]),
        l2_bulk=b["l2_reads"], l2_simt=s["l2_reads"],
        eng_share=pct(b.get("bulk_elems_total", 0), b["accesses"]),
        pool_occ=b.get("bulk_g6b_ready_occ_sum", 0) / max(1, b["cycles"]),
        pool_full=pct(b.get("bulk_g6b_full_cycles", 0), b["cycles"]),
        pool_bp=pct(b.get("bulk_g6b_backpressure_cycles", 0), b["cycles"]),
        walker=pct(b["walker_busy"], b["cycles"] * 8),
        ipc_bulk=b["accesses"] / b["cycles"], ipc_simt=s["accesses"] / s["cycles"],
    )


pairs = []
for tid in G:
    if (tid, "bulk") in cells and (tid, "simt") in cells:
        pairs.append(row(tid, cells[(tid, "bulk")], cells[(tid, "simt")]))

with open(os.path.join(F, "summary.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["trace", "group", "arm", "cycles", "walks", "tlb_misses", "dram_reads",
                "row_hits", "row_acts", "l2_reads", "bank_busy", "elements",
                "walkers", "walk_cache", "conservation"])
    for tid in sorted(G):
        for arm in ("bulk", "simt"):
            d = cells.get((tid, arm))
            if not d:
                continue
            w.writerow([tid, G[tid][0], arm, d["cycles"], d["walks"], d["tlb0_misses"],
                        d["dram_reads"], d["dram_row_hits"], d["dram_row_acts_sum"],
                        d["l2_reads"], d["dram_bank0_busy"] + d["dram_bank1_busy"],
                        d.get("bulk_elems_total", 0), d["walker_busy"],
                        d.get("walk_cache_hits", ""), d.get("conservation_ok")])

with open(os.path.join(F, "pairs.csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(pairs[0].keys()))
    w.writeheader()
    for p in pairs:
        w.writerow(p)

# ---------------------------------------------------------------------- tables --
out("== 1. the matrix: Bulk G6D S1 vs resource-matched M0-RM ==")
out(f"{'trace':22s} {'group':9s} {'M0-RM':>10s} {'Bulk':>10s} {'speedup':>8s} "
    f"{'walks b/s':>14s} {'DRAM rd b/s':>16s} {'row% b/s':>14s} {'eng%':>6s}")
ordered = sorted(pairs, key=lambda p: -p["speedup"])
for p in ordered:
    out(f"{p['trace']:22s} {p['group']:9s} {p['cycles_simt']:10d} {p['cycles_bulk']:10d} "
        f"{p['speedup']:8.3f} {p['walks_bulk']:6d}/{p['walks_simt']:<7d} "
        f"{p['reads_bulk']:7d}/{p['reads_simt']:<8d} "
        f"{p['rowhit_bulk']:5.1f}/{p['rowhit_simt']:<7.1f} {p['eng_share']:6.1f}")

out("\n== 2. properties vs speedup (offline structure, sorted by speedup) ==")
out(f"{'trace':22s} {'ws KiB':>9s} {'pages':>7s} {'dup':>6s} {'seq':>6s} {'reuse16K':>8s} "
    f"{'rows':>7s} {'ws/L2':>6s} {'pg/TLB':>7s} {'speedup':>8s}")
for p in ordered:
    st = S.get(p["trace"])
    if not st:
        continue
    out(f"{p['trace']:22s} {st['ws_kib']:9.1f} {int(st['tgt_pages']):7d} {st['dup_line']:6.3f} "
        f"{st['seq_frac']:6.3f} {st['reuse16k']:8.3f} {int(st['rows']):7d} "
        f"{st['cache_pressure']:6.2f} {st['tlb_pressure']:7.2f} {p['speedup']:8.3f}")


def pearson(xs, ys):
    n = len(xs)
    if n < 3:
        return float("nan")
    mx, my = sum(xs) / n, sum(ys) / n
    sx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    sy = math.sqrt(sum((y - my) ** 2 for y in ys))
    if sx == 0 or sy == 0:
        return float("nan")
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / (sx * sy)


def spearman(xs, ys):
    def rank(v):
        order = sorted(range(len(v)), key=lambda i: v[i])
        r = [0.0] * len(v)
        i = 0
        while i < len(order):
            j = i
            while j + 1 < len(order) and v[order[j + 1]] == v[order[i]]:
                j += 1
            avg = (i + j) / 2.0 + 1
            for k in range(i, j + 1):
                r[order[k]] = avg
            i = j + 1
        return r
    return pearson(rank(xs), rank(ys))


PROPS = [("ws_kib", "target working set (KiB)"), ("tgt_pages", "target pages"),
         ("dup_line", "line duplicate rate"), ("seq_frac", "sequential-run fraction"),
         ("reuse16k", "reuse within one 16K window"), ("rows", "DRAM rows touched"),
         ("row_run", "mean accesses per row run"),
         ("cache_pressure", "working set / 1 MiB L2"),
         ("tlb_pressure", "pages / 512-page L2 TLB reach"),
         ("acc_per_page", "target accesses per page"),
         ("lines_per_page", "lines per page"),
         ("idx_share", "index lines / all lines")]
joined = [(p, S[p["trace"]]) for p in pairs if p["trace"] in S]
out(f"\n== 3. correlation with speedup (n = {len(joined)} traces) ==")
out(f"{'property':38s} {'pearson r':>10s} {'spearman':>9s} {'r on log2(S)':>13s}")
lg = [math.log2(p["speedup"]) for p, _ in joined]
rows_corr = []
for k, label in PROPS:
    xs = [st[k] for _, st in joined]
    r = pearson(xs, [p["speedup"] for p, _ in joined])
    rs = spearman(xs, [p["speedup"] for p, _ in joined])
    rl = pearson(xs, lg)
    rows_corr.append((label, r, rs, rl, k))
    out(f"{label:38s} {r:10.3f} {rs:9.3f} {rl:13.3f}")

out("\n== 4. where Bulk wins, ties and loses ==")
win = [p for p in pairs if p["speedup"] >= 1.10]
tie = [p for p in pairs if 1.02 <= p["speedup"] < 1.10]
neg = [p for p in pairs if p["speedup"] < 1.02]
for label, lst in (("wins  (>= 1.10x)", win), ("marginal (1.02-1.10x)", tie),
                   ("no benefit (< 1.02x)", neg)):
    out(f"{label}: {len(lst)} traces")
    for p in sorted(lst, key=lambda p: -p["speedup"]):
        st = S.get(p["trace"], {})
        out(f"    {p['trace']:22s} {p['speedup']:6.3f}  ws={st.get('ws_kib', 0):7.1f} KiB "
            f"dup={st.get('dup_line', 0):.3f} seq={st.get('seq_frac', 0):.3f} "
            f"pages={int(st.get('tgt_pages', 0)):6d}")

out("\n== 5. per-group summary ==")
out(f"{'group':10s} {'n':>3s} {'min':>6s} {'median':>7s} {'max':>6s}")
gs = {}
for p in pairs:
    gs.setdefault(p["group"], []).append(p["speedup"])
for g in sorted(gs):
    v = sorted(gs[g])
    med = v[len(v) // 2] if len(v) % 2 else 0.5 * (v[len(v) // 2 - 1] + v[len(v) // 2])
    out(f"{g:10s} {len(v):3d} {v[0]:6.3f} {med:7.3f} {v[-1]:6.3f}")

out("\n== 6. the four M24-D primaries (reused) and their map-M3 cells (reused) ==")
out(f"{'workload':16s} {'map0 speedup':>13s} {'map3 s1':>8s} {'map3 s2':>8s} {'map3 s3':>8s} "
    f"{'spread':>7s}")
for wl, name in PRIMARY:
    b, s = cells.get(("primary:" + wl, "bulk")), cells.get(("primary:" + wl, "simt"))
    if not b or not s:
        continue
    base = s["cycles"] / b["cycles"]
    ms = []
    guard = False
    for seed in (1, 2, 3):
        bb, ss = maps.get((wl, "bulk", seed)), maps.get((wl, "simt", seed))
        if bb and ss and (bb["cycles"] >= 0.9 * GUARD or ss["cycles"] >= 0.9 * GUARD):
            guard = True          # the M24-D non-terminating cell: not a ratio
            ms.append(float("nan"))
            continue
        ms.append(ss["cycles"] / bb["cycles"] if bb and ss else float("nan"))
    vals = [m for m in ms if m == m]
    spread = max(vals) / min(vals) - 1 if len(vals) > 1 else 0.0
    cells_s = [("guard" if m != m else f"{m:8.3f}") for m in ms]
    out(f"{name:16s} {base:13.3f} {cells_s[0]:>8s} {cells_s[1]:>8s} {cells_s[2]:>8s} "
        f"{spread*100:6.1f}%" + ("   (s3 hit the 500 M guard, excluded)" if guard else ""))

out("\n== 7. controlled contrasts: same footprint, different order ==")
PAIRS = [("w4_duplow", "n1_seq8m", "8 MiB, 2 048 pages, dup 0.000"),
         ("n4_uni4m", "n5_seq8m_s4", "~4 MiB, ~1 024 pages, dup ~0.5")]
for a, b, label in PAIRS:
    pa = next((p for p in pairs if p["trace"] == a), None)
    pb = next((p for p in pairs if p["trace"] == b), None)
    if not pa or not pb:
        continue
    out(f"{label}:")
    for p in (pa, pb):
        st = S.get(p["trace"], {})
        out(f"    {p['trace']:16s} seq={st.get('seq_frac', 0):.3f} dup={st.get('dup_line', 0):.3f} "
            f"ws={st.get('ws_kib', 0):7.1f} KiB  speedup={p['speedup']:6.3f}  "
            f"walks={p['walks_bulk']:6d}/{p['walks_simt']:<6d} "
            f"reads={p['reads_bulk']:6d}/{p['reads_simt']:<6d} "
            f"row={p['rowhit_bulk']:5.1f}/{p['rowhit_simt']:<5.1f}")

out("\n== 8. request-pool and dispatch behaviour in the winning cells ==")
out(f"{'trace':22s} {'pool occ':>8s} {'full%':>6s} {'bp%':>6s} {'eng%':>6s} "
    f"{'walker%':>8s} {'ipc b/s':>14s}")
for p in sorted(win, key=lambda p: -p["speedup"]):
    out(f"{p['trace']:22s} {p['pool_occ']:8.1f} {p['pool_full']:6.1f} {p['pool_bp']:6.1f} "
        f"{p['eng_share']:6.1f} {p['walker']:8.1f} "
        f"{p['ipc_bulk']:6.3f}/{p['ipc_simt']:<7.3f}")

# --------------------------------------------------------------- determinism --
for tid in ("w4_duplow", "criteo_2k", "n3_clu4m"):
    for arm in ("bulk", "simt"):
        p = os.path.join(F, f"{tid}__{arm}__repeat.txt")
        if not os.path.exists(p):
            continue
        r = load(p)
        chk(r["cycles"] == cells[(tid, arm)]["cycles"], f"repeat {tid}/{arm} differs")
        chk(r.get("completion_hash") == cells[(tid, arm)].get("completion_hash"),
            f"repeat {tid}/{arm} completion differs")

out("\n== 9. mechanism detail: what the engine changed ==")
out(f"{'trace':22s} {'speedup':>7s} {'walks S/B':>10s} {'reads S/B':>10s} {'row% B-S':>8s} "
    f"{'L2 S/B':>16s} {'idx%':>5s} {'org%':>5s} {'bank% b/s':>14s}")
for p in pairs:
    tid = p["trace"]
    b, s = cells[(tid, "bulk")], cells[(tid, "simt")]
    idx = pct(b.get("bulk_idx_lines_done", 0), b["dram_reads"])
    org = pct(b.get("bulk_org_cycles", 0), b["cycles"])
    st = S.get(tid, {})
    seq = st.get("seq_frac", 0)
    out(f"{tid:22s} {p['speedup']:7.3f} {p['walks_simt']/max(1,p['walks_bulk']):10.2f} "
        f"{p['reads_simt']/max(1,p['reads_bulk']):10.2f} {p['rowhit_bulk']-p['rowhit_simt']:+8.1f} "
        f"{p['l2_simt']:7d}/{p['l2_bulk']:<8d} {idx:5.1f} {org:5.1f} "
        f"{p['bank_bulk']:6.1f}/{p['bank_simt']:<6.1f}  seq={seq:.3f}")

out("\n== 10. validation ==")
out(f"pairs: {len(pairs)}  cells: {len(cells)}  map cells: {len(maps)}")
if bad:
    out(f"FAILURES {len(bad)}:")
    for m in bad[:40]:
        out("  " + m)
else:
    out("all pair checks passed: same trace, same element count, conservation, "
        "dispatch fields, row accounting, no guard hit")

with open(os.path.join(F, "report.txt"), "w") as f:
    f.write("\n".join(out_lines) + "\n")
sys.exit(1 if bad else 0)
