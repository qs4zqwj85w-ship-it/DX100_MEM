#!/usr/bin/env python3
"""M24-E — mechanism ablation: extraction, effect checks and consistency checks.

Reads the M24-D baseline cells (build/wm/m24d/m24d_bulk_<wl>.txt) and the
ablation cells (build/wm/m24e/m24e_<ablation>_<wl>.txt), and writes
build/wm/m24e/summary.csv + ablate.txt.

For every ablation cell it checks two things that a cycle count alone cannot:
  * the mechanism was really switched (the model's own policy counters say so);
  * the run is conserved, bounded and complete.
"""
import csv
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
E = os.path.join(ROOT, "build", "wm", "m24e")
D = os.path.join(ROOT, "build", "wm", "m24d")
WLS = ["orkut", "livejournal", "ogbn", "criteo"]
NAME = {"orkut": "Orkut BFS S1", "livejournal": "LiveJournal",
        "ogbn": "ogbn-products train", "criteo": "Criteo embeddings"}
GUARD = 500000000

ABL = ["nocoh", "coh1", "coh2", "coh4", "coh16",
       "pool1", "pool16", "pool64", "pool512",
       "nodram", "unfused", "idx0", "idx1", "idx3"]
LABEL = {"base": "baseline (G6D S1)", "nocoh": "cohorts off (org 7)",
         "coh1": "cohorts = 1", "coh2": "cohorts = 2", "coh4": "cohorts = 4",
         "coh16": "cohorts = 16", "pool1": "ready pool = 1",
         "pool16": "ready pool = 16", "pool64": "ready pool = 64",
         "pool512": "ready pool = 512", "nodram": "DRAM order off",
         "unfused": "unfused window", "idx0": "idx_mode 0",
         "idx1": "idx_mode 1", "idx3": "idx_mode 3"}

KEYS = ("cycles accesses trace_accesses lines instr_issued walks pte_fetches "
        "walker_busy walker_max inflight_max lat_p50 lat_p95 "
        "tlb0_reads tlb0_hits tlb0_misses tlb0_mshr_occ tlb0_mshr_max "
        "l2tlb_reads l2tlb_hits l2tlb_misses l2tlb_occ l2tlb_max_occ "
        "l1_reads l1_hits l1_misses l1_mshr_max l2_reads l2_hits l2_misses l2_mshr_max "
        "dram_reads dram_arrivals dram_row_hits dram_row_conflicts dram_row_acts_sum "
        "dram_bank0_busy dram_bank1_busy dram_bytes dram_wait_sum dram_q_occ_sum "
        "dram_max_q conservation_ok completion_hash "
        "bulk_elems_total bulk_elems_done bulk_idx_elems_req bulk_idx_elems_done "
        "bulk_idx_lines_req bulk_idx_lines_done bulk_idx_sub bulk_idx_consumed "
        "bulk_tgt_req_gen bulk_tgt_req_done bulk_tgt_req_issued bulk_lines_issued "
        "bulk_win_sealed bulk_batches bulk_peak_slot bulk_residency_violations "
        "bulk_org_policy bulk_org_runs bulk_org_cycles bulk_sched_on bulk_sched_cohorts "
        "bulk_sched_bytes bulk_sched_switches bulk_sched_opens bulk_sched_defer "
        "bulk_g6c_admit_accepted bulk_g6c_admit_attempts bulk_g6c_admit_idle_cycles "
        "bulk_g6c_cohorts_opened bulk_g6c_refused bulk_g6c_refuse_full "
        "bulk_g6c_refuse_parked bulk_g6c_defer_events bulk_g6c_defer_depth_max "
        "bulk_g6c_defer_age_sum bulk_g6c_deferred_readmitted bulk_g6c_windows "
        "bulk_g6b_pa_capacity bulk_g6b_pa_bytes bulk_g6b_ready_peak "
        "bulk_g6b_ready_occ_sum bulk_g6b_full_cycles bulk_g6b_no_ready_cycles "
        "bulk_g6b_backpressure_cycles bulk_g6b_release_pops bulk_g6b_release_slot_stall "
        "bulk_g6b_release_blocked_cycles bulk_g6b_tgt_out_cycles "
        "bulk_g6b_tr_requests bulk_g6b_tr_completed bulk_g6b_tr_stall_cycles "
        "bulk_g6b_tr_wait_dram bulk_g6b_runs_released "
        "bulk_win_fused bulk_win_vec_bytes bulk_win_fused_vec_bytes bulk_win_bytes "
        "bulk_win_entries bulk_win_size_mean bulk_win_size_max "
        "bulk_idx_mode bulk_idx_bytes bulk_idx_lines_done_x bulk_idx_ooo_gen "
        "bulk_idx_wait_stall bulk_idx_refuse bulk_idx_resident_max "
        "bulk_m24_rel_inversions bulk_m24_rel_pairs "
        "bulk_dedup_merged bulk_fanout_elems").split()

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


cells = {}
for w in WLS:
    p = os.path.join(D, f"m24d_bulk_{w}.txt")
    if not os.path.exists(p):
        bad.append(f"missing baseline {w}")
        continue
    cells[("base", w)] = load(p)
for a in ABL + ["nocohps"]:
    wls = ["orkut", "ogbn"] if a == "nocohps" else WLS
    for w in wls:
        p = os.path.join(E, f"m24e_{a}_{w}.txt")
        if not os.path.exists(p):
            bad.append(f"missing cell {a}/{w}")
            continue
        d = load(p)
        if "cycles" not in d:  # still being written, or the run died
            bad.append(f"incomplete cell {a}/{w}")
            continue
        cells[(a, w)] = d

# ---------------------------------------------------- did the switch act? --
EXPECT = {
    "base":    dict(org=8, cohorts=8, pool=4096, fused=1, idx=2, org_runs=">0"),
    "nocoh":   dict(org=7, cohorts=0, pool=4096, fused=1, idx=2, org_runs=">0"),
    "coh1":    dict(org=8, cohorts=1, pool=4096, fused=1, idx=2, org_runs=">0"),
    "coh2":    dict(org=8, cohorts=2, pool=4096, fused=1, idx=2, org_runs=">0"),
    "coh4":    dict(org=8, cohorts=4, pool=4096, fused=1, idx=2, org_runs=">0"),
    "coh16":   dict(org=8, cohorts=16, pool=4096, fused=1, idx=2, org_runs=">0"),
    "pool1":   dict(org=8, cohorts=8, pool=1, fused=1, idx=2, org_runs=">0"),
    "pool16":  dict(org=8, cohorts=8, pool=16, fused=1, idx=2, org_runs=">0"),
    "pool64":  dict(org=8, cohorts=8, pool=64, fused=1, idx=2, org_runs=">0"),
    "pool512": dict(org=8, cohorts=8, pool=512, fused=1, idx=2, org_runs=">0"),
    "nodram":  dict(org=8, cohorts=8, pool=4096, fused=1, idx=2, org_runs=0),
    "unfused": dict(org=0, cohorts=8, pool=0, fused=0, idx=2, org_runs=0),
    "idx0":    dict(org=8, cohorts=8, pool=4096, fused=1, idx=0, org_runs=">0"),
    "idx1":    dict(org=8, cohorts=8, pool=4096, fused=1, idx=1, org_runs=">0"),
    "idx3":    dict(org=8, cohorts=8, pool=4096, fused=1, idx=3, org_runs=">0"),
    # configured 1/8 but the scheduler is off: bulk_sched_cohorts echoes the
    # field, bulk_sched_on says whether anything reads it
    "nocohps": dict(org=7, cohorts=8, pool=4096, fused=1, idx=2, org_runs=">0"),
}
for (a, w), d in cells.items():
    e = EXPECT[a]
    tag = f"{a}/{w}"
    chk(d.get("bulk_org_policy") == e["org"], f"{tag}: org policy {d.get('bulk_org_policy')}")
    chk(d.get("bulk_sched_cohorts") == e["cohorts"],
        f"{tag}: cohort width {d.get('bulk_sched_cohorts')}")
    chk(d.get("bulk_g6b_pa_capacity") == e["pool"], f"{tag}: pool {d.get('bulk_g6b_pa_capacity')}")
    chk(d.get("bulk_win_fused") == e["fused"], f"{tag}: fused {d.get('bulk_win_fused')}")
    chk(d.get("bulk_idx_mode") == e["idx"], f"{tag}: idx mode {d.get('bulk_idx_mode')}")
    if e["org_runs"] == 0:
        chk(d.get("bulk_org_runs") == 0, f"{tag}: organizer ran {d.get('bulk_org_runs')} times")
    else:
        chk(d.get("bulk_org_runs", 0) > 0, f"{tag}: organizer never ran")
    if a in ("nocoh", "nocohps"):
        chk(d.get("bulk_g6c_admit_accepted") == 0, f"{tag}: cohorts admitted work")
    if a == "nocoh":
        chk(d.get("bulk_sched_on") == 0, f"{tag}: scheduler still on")
    if a == "nocohps":
        # the fields are set, the scheduler object is built, and nothing reads it
        chk(d.get("bulk_sched_on") == 1, f"{tag}: scheduler not constructed")
        chk(d["cycles"] == cells[("nocoh", w)]["cycles"],
            f"{tag}: cycles differ from the ps=0 arm")
    if a.startswith("coh"):
        chk(d.get("bulk_sched_on") == 1, f"{tag}: scheduler off")
        chk(d.get("bulk_g6c_admit_accepted", 0) > 0, f"{tag}: cohorts admitted nothing")

# ------------------------------------------------------------- validation --
for (a, w), d in cells.items():
    tag = f"{a}/{w}"

    def c(ok, msg):
        chk(ok, f"{tag}: {msg}")

    c(d["cycles"] > 0 and d["cycles"] < GUARD, f"cycles {d['cycles']} (guard/incomplete)")
    c(d.get("conservation_ok") == 1, f"conservation_ok={d.get('conservation_ok')}")
    c(d.get("completion_hash") not in ("", 0), "no completion hash")
    c(d["accesses"] == d["trace_accesses"], "accesses != trace_accesses")
    c(d["pte_fetches"] == 2 * d["walks"], "pte != 2*walks")
    c(d["tlb0_hits"] + d["tlb0_misses"] == d["tlb0_reads"], "dTLB sum")
    c(d["l1_hits"] + d["l1_misses"] == d["l1_reads"], "L1 sum")
    c(d["l2_hits"] + d["l2_misses"] == d["l2_reads"], "L2 sum")
    c(d["dram_row_hits"] + d["dram_row_conflicts"] + 2 == d["dram_reads"], "row sum")
    c(d["dram_row_acts_sum"] == d["dram_row_conflicts"] + 2, "acts")
    c(d["dram_reads"] == d["dram_arrivals"], "arrivals")
    c(d["bulk_elems_done"] == d["bulk_elems_total"], "elements not all served")
    c(d["bulk_tgt_req_done"] == d["bulk_tgt_req_gen"] == d["bulk_tgt_req_issued"], "tgt reqs")
    c(d["bulk_win_sealed"] == d["bulk_batches"], "windows != batches")
    c(d["bulk_peak_slot"] <= 128, f"slots {d['bulk_peak_slot']} > 128")
    c(d["bulk_residency_violations"] == 0, "residency violations")
    if d.get("bulk_g6b_pa_capacity"):
        c(d["bulk_g6b_ready_peak"] <= d["bulk_g6b_pa_capacity"], "pool over capacity")
    if a != "unfused":
        c(d["bulk_idx_lines_done"] == d["bulk_idx_lines_req"] == d["bulk_idx_sub"], "idx lines")

# determinism, where a repeat exists
for a in ABL:
    for w in WLS:
        p = os.path.join(E, f"m24e_{a}_{w}__repeat.txt")
        if not os.path.exists(p):
            continue
        r = load(p)
        d = cells[(a, w)]
        chk(r["cycles"] == d["cycles"] and r["completion_hash"] == d["completion_hash"],
            f"repeat {a}/{w} differs")

by = {(a, w): cells[(a, w)] for a, w in cells}


def I(a, w, k):
    return int(cells[(a, w)][k])


def F(a, w, k):
    return float(cells[(a, w)][k])


def pct(x, y):
    return 100.0 * x / y if y else 0.0


def rel(a, w):
    """cycles of ablation a against the baseline, as a slowdown factor"""
    return I(a, w, "cycles") / I("base", w, "cycles")


out("== 1. cycles per ablation (millions of cycles, slowdown vs baseline) ==")
hdr = f"{'ablation':22s}" + "".join(f"{NAME[w][:12]:>16s}" for w in WLS)
out(hdr)
for a in ["base"] + ABL:
    if ("base", "orkut") not in cells:
        continue
    row = f"{LABEL[a]:22s}"
    for w in WLS:
        if (a, w) not in cells:
            row += f"{'—':>16s}"
        elif a == "base":
            row += f"{I(a, w, 'cycles'):16d}"
        else:
            row += f"{I(a, w, 'cycles'):10d} {rel(a, w):5.3f}x"
    out(row)

out("\n== 2. A — page-cohort organization ==")
out(f"{'cell':10s} {'workload':22s} {'cycles':>11s} {'vs base':>8s} {'walker%':>8s} "
    f"{'dMSHR':>6s} {'admits':>10s} {'defer':>10s} {'depth':>6s} {'coh open':>9s} "
    f"{'switches':>9s}")
for a in ["base", "nocoh", "coh1", "coh2", "coh4", "coh16"]:
    for w in WLS:
        if (a, w) not in cells:
            continue
        d = cells[(a, w)]
        out(f"{a:10s} {NAME[w]:22s} {d['cycles']:11d} "
            f"{'—' if a == 'base' else format(rel(a, w), '.3f') + 'x':>8s} "
            f"{pct(d['walker_busy'], d['cycles'] * 8):8.1f} "
            f"{F(a, w, 'tlb0_mshr_occ') / d['cycles']:6.2f} "
            f"{d['bulk_g6c_admit_accepted']:10d} {d['bulk_g6c_defer_events']:10d} "
            f"{d['bulk_g6c_defer_depth_max']:6d} {d['bulk_g6c_cohorts_opened']:9d} "
            f"{d['bulk_sched_switches']:9d}")

out("\n== 3. B — shared ready pool ==")
out(f"{'cell':10s} {'workload':22s} {'cycles':>11s} {'vs base':>8s} {'occ':>8s} "
    f"{'peak':>6s} {'full%':>6s} {'empty%':>7s} {'bp%':>6s} {'resid':>7s} "
    f"{'rel/cyc':>8s}")
for a in ["base", "pool1", "pool16", "pool64", "pool512"]:
    for w in WLS:
        if (a, w) not in cells:
            continue
        d = cells[(a, w)]
        pops = d["bulk_g6b_release_pops"] or 1
        out(f"{a:10s} {NAME[w]:22s} {d['cycles']:11d} "
            f"{'—' if a == 'base' else format(rel(a, w), '.3f') + 'x':>8s} "
            f"{d['bulk_g6b_ready_occ_sum'] / d['cycles']:8.1f} "
            f"{d['bulk_g6b_ready_peak']:6d} "
            f"{pct(d['bulk_g6b_full_cycles'], d['cycles']):6.1f} "
            f"{pct(d['bulk_g6b_no_ready_cycles'], d['cycles']):7.1f} "
            f"{pct(d['bulk_g6b_backpressure_cycles'], d['cycles']):6.1f} "
            f"{d['bulk_g6b_ready_occ_sum'] / pops:7.1f} "
            f"{pops / d['cycles']:8.4f}")

out("\n== 4. C — DRAM-aware release ==")
out(f"{'cell':8s} {'workload':22s} {'cycles':>11s} {'vs base':>8s} {'rowhit%':>8s} "
    f"{'acts':>9s} {'bankbusy%':>10s} {'qdepth':>7s} {'wait/read':>10s} {'inv%':>6s}")
for a in ["base", "nodram"]:
    for w in WLS:
        d = cells[(a, w)]
        out(f"{a:8s} {NAME[w]:22s} {d['cycles']:11d} "
            f"{'—' if a == 'base' else format(rel(a, w), '.3f') + 'x':>8s} "
            f"{pct(d['dram_row_hits'], d['dram_reads']):8.1f} {d['dram_row_acts_sum']:9d} "
            f"{pct(d['dram_bank0_busy'] + d['dram_bank1_busy'], 2 * d['cycles']):10.1f} "
            f"{d['dram_q_occ_sum'] / d['cycles']:7.2f} "
            f"{d['dram_wait_sum'] / d['dram_reads']:10.1f} "
            f"{pct(d['bulk_m24_rel_inversions'], d['bulk_m24_rel_pairs']):6.2f}")

out("\n== 5. D — fused vs unfused window ==")
out(f"{'cell':8s} {'workload':22s} {'cycles':>11s} {'vs base':>8s} {'reads':>9s} "
    f"{'win vec B':>10s} {'design B':>9s} {'entries':>8s} {'elems':>8s} {'runs/el':>8s}")
for a in ["unfused", "base"]:
    for w in WLS:
        d = cells[(a, w)]
        out(f"{a:8s} {NAME[w]:22s} {d['cycles']:11d} "
            f"{'—' if a == 'base' else format(rel(a, w), '.3f') + 'x':>8s} "
            f"{d['dram_reads']:9d} {d['bulk_win_vec_bytes']:10d} {d['bulk_win_bytes']:9d} "
            f"{d['bulk_win_entries']:8d} {d['bulk_elems_total']:8d} "
            f"{d['bulk_win_entries'] / d['bulk_elems_total']:8.3f}")

out("\n== 6. E — index-stream acquisition ==")
out(f"{'cell':8s} {'workload':22s} {'cycles':>11s} {'vs base':>8s} {'idx lines':>10s} "
    f"{'ooo gen':>10s} {'idx stall':>10s} {'idx refuse':>11s} {'idx B':>7s}")
for a in ["idx0", "idx1", "base", "idx3"]:
    for w in WLS:
        d = cells[(a, w)]
        out(f"{a:8s} {NAME[w]:22s} {d['cycles']:11d} "
            f"{'—' if a == 'base' else format(rel(a, w), '.3f') + 'x':>8s} "
            f"{d['bulk_idx_lines_done']:10d} {d['bulk_idx_ooo_gen']:10d} "
            f"{d['bulk_idx_wait_stall']:10d} {d['bulk_idx_refuse']:11d} "
            f"{d['bulk_idx_bytes']:7d}")

out("\n== 7. equivalence: page_sched / page_cohorts are inert at bulk_org = 7 ==")
for w in ["orkut", "ogbn"]:
    if ("nocohps", w) not in cells:
        continue
    out(f"{NAME[w]:22s} nocoh(ps=0,coh=0) {I('nocoh', w, 'cycles'):11d}   "
        f"nocohps(ps=1,coh=8) {I('nocohps', w, 'cycles'):11d}   "
        f"{'identical' if I('nocoh', w, 'cycles') == I('nocohps', w, 'cycles') else 'DIFFERS'}")

out("\n== 8. contribution ranking (slowdown if the mechanism is removed) ==")
out(f"{'mechanism':26s} {'Orkut':>8s} {'LiveJ':>8s} {'ogbn':>8s} {'Criteo':>8s} "
    f"{'best case':>10s}")
RANK = [("cohort admission (A)", "nocoh"), ("ready pool 4096→512 (B)", "pool512"),
        ("ready pool 4096→64 (B)", "pool64"), ("ready pool 4096→16 (B)", "pool16"),
        ("ready pool 4096→1 (B)", "pool1"), ("DRAM-aware release (C)", "nodram"),
        ("fused window (D)", "unfused"), ("index tiling (E)", "idx0")]
for label, a in RANK:
    vals = [rel(a, w) for w in WLS]
    out(f"{label:26s} " + " ".join(f"{v:8.3f}" for v in vals)
        + f" {max(vals):10.3f}")

fields = ["ablation", "wl", "cell"] + KEYS
with open(os.path.join(E, "summary.csv"), "w", newline="") as f:
    wr = csv.DictWriter(f, fieldnames=fields)
    wr.writeheader()
    for (a, w) in sorted(cells):
        wr.writerow(dict(ablation=a, wl=w, cell=f"{a}_{w}",
                         **{k: cells[(a, w)].get(k, "") for k in KEYS}))

out(f"\n{'ALL CHECKS PASSED' if not bad else 'FAILURES'}: {len(cells)} cells")
for b in bad:
    out("   " + b)
with open(os.path.join(E, "ablate.txt"), "w") as f:
    f.write("\n".join(out_lines) + "\n")
sys.exit(1 if bad else 0)
