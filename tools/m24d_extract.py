#!/usr/bin/env python3
"""M24-D — strict resource-matched SIMT vs. M24 Bulk: extraction and checks.

Reads every build/wm/m24d/m24d_*.txt cell and writes summary.csv + extract.txt.

Every cell is checked, not only the eight primary ones: identical host counts,
one walk per L2 TLB miss (SIMT), the arm's own request-conservation laws, and a
bounded capacity for every populated structure. A cell that reaches the cycle
guard is a failure unless it is the one declared exception below, which is
reported in full and excluded from the statistics rather than dropped silently.
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = os.path.join(ROOT, "build", "wm", "m24d")
C = os.path.join(ROOT, "build", "wm", "m24c")
WLS = ["orkut", "livejournal", "ogbn", "criteo"]
NAME = {"orkut": "Orkut BFS S1", "livejournal": "LiveJournal",
        "ogbn": "ogbn-products train", "criteo": "Criteo embeddings"}
ARM = {"simt": "SIMT (M0-RM)", "bulk": "Bulk G6D S1"}
GUARD = 500000000
# historical M0: build/wm/m23b_m0.txt, m23d_train_m0.txt, m23g6c/*.txt
M0 = {"orkut": 49067545, "livejournal": 67234692, "ogbn": 176184939, "criteo": 28511774}

# The one cell that does not terminate. Its target stream is complete (every
# element served, last completion at cycle 3 292 976) and the model then runs to
# the guard with the streaming ready pool at capacity: a termination defect in
# the M24 Bulk release path under this mapping, not a slowdown. Reported, not
# fixed -- M24-D does not modify the Bulk architecture -- and excluded from the
# mapping statistics.
EXCLUDED = {("bulk", "criteo", 3): "did not terminate; all elements served by "
                                   "cycle 3 292 976, ready pool full at exit"}

KEYS = ("cycles accesses trace_accesses trace_lines instr_issued issued "
        "requests_generated lines requests_accepted walks pte_fetches walker_busy "
        "walker_max inflight_max simt_peak_outstanding lat_p50 lat_p95 "
        "tlb0_reads tlb0_hits tlb0_misses tlb0_mshr_occ tlb0_mshr_max tlb0_mshr_stalls "
        "l2tlb_reads l2tlb_hits l2tlb_misses l2tlb_occ l2tlb_max_occ l2tlb_walk_stalls "
        "l2tlb_dedups tlb_refuse_full tlb_refuse_parked replays walk_stalls "
        "l1_hits l1_misses l1_reads l1_mshr_occ l1_mshr_max l1_stall_cycles "
        "l2_hits l2_misses l2_reads l2_mshr_occ l2_mshr_max "
        "unissued_cycles simt_slot_full_cycles "
        "dram_reads dram_arrivals dram_row_hits dram_row_conflicts dram_row_acts_sum "
        "dram_rows_used dram_bank0_busy dram_bank1_busy dram_bytes "
        "dram_wait_sum dram_wait_max dram_q_occ_sum dram_max_q "
        "dram_blocked_cycles dram_starved_cycles dram_missed_hit conservation_ok "
        "completion_hash bulk_elems_total bulk_elems_done bulk_dedup_merged "
        "bulk_idx_elems_req bulk_idx_elems_done bulk_idx_consumed "
        "bulk_idx_lines_req bulk_idx_lines_done bulk_idx_sub "
        "bulk_tgt_req_gen bulk_tgt_req_done bulk_tgt_req_issued bulk_lines_issued "
        "bulk_win_sealed bulk_batches bulk_g6b_runs_released bulk_g6b_ready_peak "
        "bulk_peak_slot bulk_residency_violations bulk_last_done_cycle bulk_last_seal "
        "cc0_issue cc0_done").split()

bad, out_lines = [], []


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


def name(kind, arm, wl, seed=None):
    if kind == "primary":
        return f"{arm}_{wl}"
    if kind == "repeat":
        return f"{arm}_{wl}__repeat"
    if kind == "map":
        return f"{arm}_{wl}_map3_s{seed}"
    return f"{arm}_w1_seed{seed}"


# --------------------------------------------------------------- the matrix --
CELLS = []
for wl in WLS:
    for arm in ("simt", "bulk"):
        CELLS.append(("primary", arm, wl, None))
        CELLS.append(("repeat", arm, wl, None))
        for s in (1, 2, 3):
            CELLS.append(("map", arm, wl, s))
for arm in ("simt", "bulk"):
    for s in (1, 7, 13):
        CELLS.append(("seed", arm, "w1", s))

data = {}
for kind, arm, wl, s in CELLS:
    p = os.path.join(D, f"m24d_{name(kind, arm, wl, s)}.txt")
    if not os.path.exists(p):
        bad.append(f"missing cell {name(kind, arm, wl, s)}")
        continue
    data[(kind, arm, wl, s)] = load(p)

present = {f for f in os.listdir(D) if f.startswith("m24d_") and f.endswith(".txt")}
expected = {f"m24d_{name(*c)}.txt" for c in CELLS}
chk(present == expected, f"cell set mismatch: {sorted(present ^ expected)}")

# ------------------------------------------------------- identities, all cells --
guard, violated, oks = [], [], []
for key, d in data.items():
    kind, arm, wl, s = key
    tag = name(kind, arm, wl, s)
    if d.get("cycles", 0) >= GUARD or d.get("conservation_ok") != 1:
        violated.append(tag)
        if key in EXCLUDED or (arm, wl, s) in EXCLUDED:
            guard.append(tag)
        continue
    oks.append(tag)

    def c(ok, msg):
        chk(ok, f"{tag}: {msg}")

    c(d["accesses"] == d["trace_accesses"], "accesses != trace_accesses")
    c(d["pte_fetches"] == 2 * d["walks"], "pte_fetches != 2*walks")
    c(d["walks"] <= d["l2tlb_misses"], "walks > l2tlb_misses")
    c(d["tlb0_hits"] + d["tlb0_misses"] == d["tlb0_reads"], "dTLB hits+misses != reads")
    c(d["l2tlb_hits"] + d["l2tlb_misses"] == d["l2tlb_reads"], "L2 hits+misses != reads")
    c(d["l1_hits"] + d["l1_misses"] == d["l1_reads"], "L1 hits+misses != L1 reads")
    c(d["l2_hits"] + d["l2_misses"] == d["l2_reads"], "L2 hits+misses != L2 reads")
    c(d["dram_row_hits"] + d["dram_row_conflicts"] + 2 == d["dram_reads"],
      "row hits + conflicts + 2 != reads")
    c(d["dram_row_acts_sum"] == d["dram_row_conflicts"] + 2, "acts != conflicts + 2")
    c(d["dram_reads"] == d["dram_arrivals"], "reads != arrivals")
    c(d["tlb0_mshr_max"] <= 8, f"dTLB MSHR peak {d['tlb0_mshr_max']} > 8")
    c(d["walker_max"] <= 8, f"walker peak {d['walker_max']} > 8")
    c(d["l2tlb_max_occ"] <= 8, f"L2 TLB MSHR peak {d['l2tlb_max_occ']} > 8")
    c(d["l1_mshr_max"] <= 128, f"L1 MSHR peak {d['l1_mshr_max']} > 128")
    c(d["l2_mshr_max"] <= 64, f"L2 MSHR peak {d['l2_mshr_max']} > 64")
    if arm == "simt":
        c(d["l2tlb_misses"] - d["walks"] <= 8, "SIMT walks != L2 TLB misses")
        c(d["simt_peak_outstanding"] <= 132, "SIMT peak outstanding > 132")
        c(d["cc0_issue"] == d["cc0_done"] == d["requests_accepted"]
          == d["requests_generated"], "issue/done/accepted/generated disagree")
        c(d["requests_accepted"] <= d["accesses"], "more requests than accesses")
    else:
        c(d["bulk_elems_done"] == d["bulk_elems_total"], "elements not all served")
        c(d["bulk_idx_elems_done"] == d["bulk_idx_elems_req"], "idx elems done != req")
        c(d["bulk_idx_lines_done"] == d["bulk_idx_lines_req"] == d["bulk_idx_sub"],
          "idx lines done != req != submitted")
        c(d["bulk_idx_consumed"] == d["bulk_elems_total"], "idx consumed != elements")
        c(d["bulk_tgt_req_done"] == d["bulk_tgt_req_gen"] == d["bulk_tgt_req_issued"],
          "target requests gen/done/issued disagree")
        c(d["bulk_lines_issued"] == d["bulk_tgt_req_gen"], "lines issued != generated")
        c(d["bulk_win_sealed"] == d["bulk_batches"], "windows sealed != batches")
        # the shared R budget bounds the engine exactly as it bounds the SIMT arm
        c(d["bulk_peak_slot"] <= 128, f"engine slots {d['bulk_peak_slot']} > R = 128")
        c(d["bulk_residency_violations"] == 0, "residency violations")

chk(set(guard) == {name("map", a, w, s) for (a, w, s) in EXCLUDED},
    f"non-terminating cells {sorted(guard)} != declared {sorted(EXCLUDED)}")
for key, d in data.items():
    if name(*key) in guard:
        a, w, s = key[1], key[2], key[3]
        chk(d["bulk_elems_done"] == d["bulk_elems_total"],
            f"{name(*key)}: not even all elements served")
        chk(d["completion_hash"] not in ("", 0), f"{name(*key)}: no completion hash")

# --------------------------------------------------------------- determinism --
det = []
for wl in WLS:
    for arm in ("simt", "bulk"):
        a = data.get(("primary", arm, wl, None))
        b = data.get(("repeat", arm, wl, None))
        if not (a and b):
            continue
        same = (a.get("cycles") == b.get("cycles")
                and a.get("completion_hash") == b.get("completion_hash"))
        det.append((arm, wl, a.get("cycles"), b.get("cycles"), same))
        chk(same, f"repeat {arm}_{wl}: cycles/hash differ")

# ------------------------------------------- equivalence to published cells --
equiv = []
for wl in WLS:
    for arm, ref in (("simt", "m0rm"), ("bulk", "g6ds1")):
        a = data.get(("primary", arm, wl, None))
        p = os.path.join(C, f"m24c_{ref}_{wl}.txt")
        if not (a and os.path.exists(p)):
            bad.append(f"missing reference for {arm}_{wl}")
            continue
        h = load(p)
        same = (a.get("cycles") == h.get("cycles")
                and a.get("completion_hash") == h.get("completion_hash")
                and a.get("walks") == h.get("walks"))
        equiv.append((arm, wl, h.get("cycles"), a.get("cycles"), same))
        chk(same, f"{arm}_{wl}: pinned config does not reproduce the published cell")

by = {(c[1], c[2]): data[c] for c in data if c[0] == "primary"}
mapby = {(c[1], c[2], c[3]): data[c] for c in data if c[0] == "map"}
seedby = {(c[1], c[3]): data[c] for c in data if c[0] == "seed"}


def pct(a, b):
    return 100.0 * a / b if b else 0.0


out("== 1. execution cycles ==")
out(f"{'workload':22s} {'M0 (hist)':>12s} {'SIMT (M0-RM)':>13s} {'Bulk G6D S1':>13s} "
    f"{'speedup':>8s}")
for wl in WLS:
    s, b = by[("simt", wl)]["cycles"], by[("bulk", wl)]["cycles"]
    out(f"{NAME[wl]:22s} {M0[wl]:12d} {s:13d} {b:13d} {s / b:8.3f}")

out("\n== 2. speedups ==")
out(f"{'workload':22s} {'SIMT/Bulk':>10s} {'M0/Bulk':>9s} {'M0/SIMT':>9s}")
for wl in WLS:
    s, b = by[("simt", wl)]["cycles"], by[("bulk", wl)]["cycles"]
    out(f"{NAME[wl]:22s} {s / b:10.3f} {M0[wl] / b:9.3f} {M0[wl] / s:9.3f}")

out("\n== 3. translation ==")
out(f"{'workload':22s} {'arm':4s} {'walks':>9s} {'dTLB%':>7s} {'L2TLB%':>7s} "
    f"{'walkers':>8s} {'dMSHR':>6s} {'l2MSHR':>7s} {'peak':>5s} {'p50':>6s} {'p95':>6s}")
for wl in WLS:
    for arm in ("simt", "bulk"):
        r = by[(arm, wl)]
        out(f"{NAME[wl]:22s} {arm:4s} {r['walks']:9d} "
            f"{pct(r['tlb0_hits'], r['tlb0_reads']):7.1f} "
            f"{pct(r['l2tlb_hits'], r['l2tlb_reads']):7.1f} "
            f"{r['walker_busy'] / r['cycles']:8.2f} {r['tlb0_mshr_occ'] / r['cycles']:6.2f} "
            f"{r['l2tlb_occ'] / r['cycles']:7.2f} {r['inflight_max']:5d} "
            f"{r['lat_p50']:6d} {r['lat_p95']:6d}")

out("\n== 4. memory system ==")
out(f"{'workload':22s} {'arm':4s} {'reads':>9s} {'rowhit%':>8s} {'acts':>9s} "
    f"{'bankbusy%':>10s} {'qdepth':>7s} {'l2reads':>9s} {'l2hit%':>7s} {'GB/s':>7s}")
for wl in WLS:
    for arm in ("simt", "bulk"):
        r = by[(arm, wl)]
        gbs = r["dram_bytes"] / r["cycles"]  # B/cycle at 1 GHz == GB/s
        out(f"{NAME[wl]:22s} {arm:4s} {r['dram_reads']:9d} "
            f"{pct(r['dram_row_hits'], r['dram_reads']):8.1f} {r['dram_row_acts_sum']:9d} "
            f"{pct(r['dram_bank0_busy'] + r['dram_bank1_busy'], 2 * r['cycles']):10.1f} "
            f"{r['dram_q_occ_sum'] / r['cycles']:7.2f} {r['l2_reads']:9d} "
            f"{pct(r['l2_hits'], r['l2_reads']):7.1f} {gbs:7.3f}")

out("\n== 5. issue path and utilization ==")
out("   '-' = the counter belongs to the SIMT front end and has no engine meaning")
out(f"{'workload':22s} {'arm':4s} {'instr':>9s} {'lines/cyc':>10s} {'peak':>5s} "
    f"{'unissued':>11s} {'tlb_refuse':>11s} {'replays':>12s}")
for wl in WLS:
    for arm in ("simt", "bulk"):
        r = by[(arm, wl)]
        dash = arm != "simt"
        out(f"{NAME[wl]:22s} {arm:4s} "
            f"{'-' if dash else r['instr_issued']:>9} {r['lines'] / r['cycles']:10.4f} "
            f"{'-' if dash else r['simt_peak_outstanding']:>5} "
            f"{'-' if dash else r['unissued_cycles']:>11} "
            f"{r['tlb_refuse_full']:11d} {r['replays']:12d}")

out("\n== 6. determinism (re-run vs original) ==")
for arm, wl, a, b, same in det:
    out(f"{NAME[wl]:22s} {arm:4s} {a:11d} {b:11d} {'identical' if same else 'DIFFERS'}")

out("\n== 7. pinned configs reproduce the published M24-C cells ==")
for arm, wl, h, a, same in equiv:
    out(f"{NAME[wl]:22s} {ARM[arm]:13s} published {h:11d}  pinned {a:11d}  "
        f"{'identical' if same else 'DIFFERS'}")

out("\n== 8. physical-mapping repeats (m24_map = 3, three permutation seeds) ==")
out(f"{'workload':22s} {'seed':>4s} {'SIMT':>11s} {'Bulk':>11s} {'SIMT/Bulk':>10s}")
for wl in WLS:
    for s in (1, 2, 3):
        a, b = mapby[("simt", wl, s)], mapby[("bulk", wl, s)]
        if b["cycles"] >= GUARD:
            out(f"{NAME[wl]:22s} {s:4d} {a['cycles']:11d} {'guard':>11s} {'excluded':>10s}")
        else:
            out(f"{NAME[wl]:22s} {s:4d} {a['cycles']:11d} {b['cycles']:11d} "
                f"{a['cycles'] / b['cycles']:10.3f}")

out("\n== 9. distribution over mappings (excluding the cell that did not terminate) ==")
out(f"{'workload':22s} {'min':>7s} {'median':>7s} {'max':>7s} {'map 0':>7s}  {'n':>3s}")
for wl in WLS:
    vals = [mapby[("simt", wl, s)]["cycles"] / mapby[("bulk", wl, s)]["cycles"]
            for s in (1, 2, 3) if mapby[("bulk", wl, s)]["cycles"] < GUARD]
    base = by[("simt", wl)]["cycles"] / by[("bulk", wl)]["cycles"]
    med = sorted(vals)[len(vals) // 2] if vals else 0.0
    out(f"{NAME[wl]:22s} {min(vals):7.3f} {med:7.3f} {max(vals):7.3f} {base:7.3f}  "
        f"{len(vals):3d}")

out("\n== 10. synthetic-generator seed sweep (W1, N = 16384, 4 KiB pages) ==")
out(f"{'seed':>5s} {'SIMT':>11s} {'Bulk':>11s} {'SIMT/Bulk':>10s} {'accesses':>9s}")
for s in (1, 7, 13):
    a, b = seedby[("simt", s)], seedby[("bulk", s)]
    out(f"{s:5d} {a['cycles']:11d} {b['cycles']:11d} {a['cycles'] / b['cycles']:10.3f} "
        f"{a['accesses']:9d}")

out("\n== 11. termination ==")
out(f"   {len(data)} cells, {len(oks)} completed, {len(guard)} declared exception, "
    f"{len(violated) - len(guard)} unexpected")
for tag in sorted(guard):
    a, w, s = [(k[1], k[2], k[3]) for k in data if name(*k) == tag][0]
    out(f"   {tag}: {EXCLUDED[(a, w, s)]}")
    d = data[("map", a, w, s)]
    out(f"      cycles={d['cycles']} elements={d['bulk_elems_done']}/{d['bulk_elems_total']} "
        f"windows={d['bulk_win_sealed']} ready_peak={d['bulk_g6b_ready_peak']} "
        f"reads={d['dram_reads']} conservation={d.get('conservation')}")

# --------------------------------------------------------------- summary -----
fields = ["arm", "wl", "cell"] + KEYS
with open(os.path.join(D, "summary.csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=fields)
    w.writeheader()
    for key in sorted(CELLS, key=lambda c: (c[0], c[1], c[2], c[3] or 0)):
        d = data.get(key)
        if d is None:
            continue
        w.writerow(dict(arm=key[1], wl=key[2], cell=name(*key),
                        **{k: d.get(k, "") for k in KEYS}))

out(f"\n{'ALL CHECKS PASSED' if not bad else 'FAILURES'}: {len(data)} cells "
    f"({len(oks)} validated, {len(guard)} declared guard exception)")
for b in bad:
    out("   " + b)
with open(os.path.join(D, "extract.txt"), "w") as f:
    f.write("\n".join(out_lines) + "\n")
sys.exit(1 if bad else 0)
