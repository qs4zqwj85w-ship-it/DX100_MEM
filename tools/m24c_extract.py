#!/usr/bin/env python3
"""M24-C — resource-matched SIMT baseline: extraction and consistency checks.

Reads build/wm/m24c/m24c_<cfg>_<wl>.txt and writes summary.csv + extract.txt.
Every identity must hold before any figure from this milestone is quoted.
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "wm", "m24c")
CFGS = ["m0", "m0rm", "b8k", "b16k", "b32k", "g6ds1"]
NAMES = {"m0": "M0", "m0rm": "M0-RM", "b8k": "Bulk 2x8K", "b16k": "Bulk 2x16K",
         "b32k": "Bulk 2x32K", "g6ds1": "Bulk G6D S1"}
WLS = ["orkut", "livejournal", "ogbn", "criteo"]
# Historical M0 controls, from logs written before this milestone:
#   build/wm/m23g6c/g6c_orkut_w16384_m0.txt, g6c_livejournal_w16384_m0.txt,
#   build/wm/m23d_train_m0.txt, build/wm/m23b_m0.txt
HIST = {"orkut": 49067545, "livejournal": 67234692, "ogbn": 176184939,
        "criteo": 28511774}

KEYS = ("cycles accesses trace_accesses trace_lines instr_issued issued "
        "requests_generated lines requests_accepted walks pte_fetches walker_busy "
        "walker_max inflight_max inflight_occ simt_peak_outstanding lat_p50 lat_p95 "
        "tlb0_reads tlb0_hits tlb0_misses tlb0_mshr_occ tlb0_mshr_max tlb0_mshr_stalls "
        "l2tlb_reads l2tlb_hits l2tlb_misses l2tlb_occ l2tlb_max_occ l2tlb_walk_stalls "
        "l2tlb_dedups tlb_refuse_full tlb_refuse_parked replays walk_stalls "
        "l1_hits l1_misses l1_mshr_occ l1_mshr_max l1_stall_cycles "
        "l2_hits l2_misses l2_mshr_occ l2_mshr_max "
        "simt_issue_stalls unissued_cycles unissued_max simt_slot_full_cycles "
        "dram_reads dram_arrivals dram_row_hits dram_row_conflicts dram_row_acts_sum "
        "dram_rows_used dram_bank0_busy dram_bank1_busy dram_busy_cycles "
        "dram_bytes dram_gbs dram_wait_sum dram_wait_max dram_q_occ_sum dram_max_q "
        "dram_blocked_cycles dram_starved_cycles dram_missed_hit conservation_ok "
        "completion_hash").split()


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


rows, bad = [], []
for wl in WLS:
    for c in CFGS:
        p = os.path.join(LOG, f"m24c_{c}_{wl}.txt")
        if not os.path.exists(p):
            bad.append(f"missing cell {c}_{wl}")
            continue
        d = load(p)
        rows.append(dict(cfg=c, wl=wl, **{k: d.get(k, "") for k in KEYS}))

for r in rows:
    def chk(ok, msg):
        if not ok:
            bad.append(f"{r['cfg']}_{r['wl']}: {msg}")
    chk(r["cycles"] > 0, "no cycles")
    chk(r["conservation_ok"] == 1, f"conservation_ok={r['conservation_ok']}")
    chk(r["completion_hash"] not in ("", 0), "no completion hash")
    chk(r["pte_fetches"] == 2 * r["walks"], "pte_fetches != 2*walks")
    chk(r["walks"] <= r["l2tlb_misses"], "walks > l2tlb_misses")
    if r["cfg"] in ("m0", "m0rm"):
        # one walk per L2 TLB miss, up to the walks still in flight at the end
        chk(r["l2tlb_misses"] - r["walks"] <= 8,
            f"SIMT walks {r['walks']} != l2tlb_misses {r['l2tlb_misses']}")
        chk(r["tlb0_mshr_max"] == (4 if r["cfg"] == "m0" else 8),
            f"dTLB MSHR peak {r['tlb0_mshr_max']} exceeds its capacity")
        chk(r["walker_max"] == (2 if r["cfg"] == "m0" else 8),
            f"walker peak {r['walker_max']} differs from the configured count")
    # the first activation of each of the two banks is neither a hit nor a
    # conflict, so hits + conflicts = reads - 2 and activations = conflicts + 2
    chk(r["dram_row_hits"] + r["dram_row_conflicts"] + 2 == r["dram_reads"],
        f"hits+conflicts+2 != reads")
    chk(r["dram_row_acts_sum"] == r["dram_row_conflicts"] + 2, "acts != conf + 2")
    chk(r["dram_reads"] == r["dram_arrivals"], "reads != arrivals")
    chk(r["dram_q_occ_sum"] >= r["cycles"] * 0, "q occupancy negative")

for wl, cyc in HIST.items():
    m0 = [r for r in rows if r["cfg"] == "m0" and r["wl"] == wl]
    if m0:
        chk(m0[0]["cycles"] == cyc, f"historical M0 {wl} {m0[0]['cycles']} != {cyc}")

keys = list(rows[0].keys()) if rows else []
with open(os.path.join(LOG, "summary.csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=keys)
    w.writeheader()
    w.writerows(rows)

lines = []


def out(s=""):
    lines.append(s)
    print(s)


by = {(r["cfg"], r["wl"]): r for r in rows}


def g(r, k):
    v = r.get(k, 0)
    return 0 if v == "" else v


out("== cycles (millions) ==")
out(f"{'workload':12s} " + " ".join(f"{NAMES[c]:>11s}" for c in CFGS))
for wl in WLS:
    out(f"{wl:12s} " + " ".join(f"{by[(c, wl)]['cycles']:11d}" for c in CFGS))

out("\n== speedups (execution cycles) ==")
out(f"{'workload':12s} {'M0/M0-RM':>9s} {'RM/2x16K':>9s} {'RM/G6D':>9s} "
    f"{'M0/2x16K':>9s} {'M0/G6D':>9s} {'M0/2x32K':>9s}")
for wl in WLS:
    m0, rm = by[("m0", wl)]["cycles"], by[("m0rm", wl)]["cycles"]
    b16, b32, g6 = (by[("b16k", wl)]["cycles"], by[("b32k", wl)]["cycles"],
                    by[("g6ds1", wl)]["cycles"])
    out(f"{wl:12s} {m0/rm:9.3f} {rm/b16:9.3f} {rm/g6:9.3f} "
        f"{m0/b16:9.3f} {m0/g6:9.3f} {m0/b32:9.3f}")

out("\n== translation ==")
out(f"{'workload':12s} {'config':12s} {'cycles':>10s} {'walks':>8s} {'dTLB%':>6s} "
    f"{'L2%':>6s} {'walkers':>8s} {'dMSHR':>6s} {'l2MSHR':>6s} {'infl':>5s} "
    f"{'p50':>5s} {'p95':>5s}")
for wl in WLS:
    for c in CFGS:
        r = by[(c, wl)]
        hr = 100.0 * r["tlb0_hits"] / (r["tlb0_hits"] + r["tlb0_misses"])
        l2 = 100.0 * r["l2tlb_hits"] / (r["l2tlb_hits"] + r["l2tlb_misses"])
        wb = r["walker_busy"] / r["cycles"]
        dm = r["tlb0_mshr_occ"] / r["cycles"]
        lm = r["l2tlb_occ"] / r["cycles"]
        out(f"{wl:12s} {NAMES[c]:12s} {r['cycles']:10d} {r['walks']:8d} {hr:6.1f} "
            f"{l2:6.1f} {wb:8.2f} {dm:6.2f} {lm:6.2f} {r['inflight_max']:5d} "
            f"{r['lat_p50']:5d} {r['lat_p95']:5d}")

out("\n== memory system ==")
out(f"{'workload':12s} {'config':12s} {'reads':>8s} {'rowhit%':>7s} {'acts':>8s} "
    f"{'bankbusy%':>9s} {'qdepth':>6s} {'wait':>11s} {'l1MSHR':>6s} {'l2MSHR':>6s}")
for wl in WLS:
    for c in CFGS:
        r = by[(c, wl)]
        rh = 100.0 * r["dram_row_hits"] / r["dram_reads"]
        bu = 100.0 * (r["dram_bank0_busy"] + r["dram_bank1_busy"]) / (2.0 * r["cycles"])
        qd = r["dram_q_occ_sum"] / r["cycles"]
        l1 = r["l1_mshr_occ"] / r["cycles"]
        l2 = r["l2_mshr_occ"] / r["cycles"]
        out(f"{wl:12s} {NAMES[c]:12s} {r['dram_reads']:8d} {rh:7.1f} "
            f"{r['dram_row_acts_sum']:8d} {bu:9.1f} {qd:6.2f} {r['dram_wait_sum']:11d} "
            f"{l1:6.2f} {l2:6.2f}")

out("\n== frontend ==")
out(f"{'workload':12s} {'config':12s} {'IPC':>6s} {'acc/instr':>9s} {'req/acc':>8s} "
    f"{'peakout':>7s} {'unissued':>11s} {'tlb_refuse':>11s} {'replays':>11s}")
for wl in WLS:
    for c in CFGS:
        r = by[(c, wl)]
        ipc = g(r, "instr_issued") / r["cycles"]
        ap = g(r, "accesses") / max(1, g(r, "instr_issued"))
        ra = g(r, "requests_generated") / max(1, g(r, "accesses"))
        out(f"{wl:12s} {NAMES[c]:12s} {ipc:6.3f} {ap:9.2f} {ra:8.2f} "
            f"{g(r, 'simt_peak_outstanding'):7d} {g(r, 'unissued_cycles'):11d} "
            f"{g(r, 'tlb_refuse_full'):11d} {g(r, 'replays'):11d}")

out(f"\n{'ALL CHECKS PASSED' if not bad else 'FAILURES'}: {len(rows)} cells")
for b in bad:
    out("   " + b)
with open(os.path.join(LOG, "extract.txt"), "w") as f:
    f.write("\n".join(lines) + "\n")
sys.exit(1 if bad else 0)
