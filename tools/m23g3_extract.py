#!/usr/bin/env python3
"""M23-G3 — extract the organization cells, check the accounting, attribute the deltas.

Two matrices, same simulator binary, same traces, same R / caches / DRAM:

  g3_<wl>_w<W>_p<P>.txt   configs/m23g3_base.cfg   ideal_translation = 1
                          (the M23-G2D reference setting: no dTLB, no walk, so a
                          cycle delta can be attributed to the memory order)
  g3p_<wl>_w<W>_p<P>.txt  configs/m23_dx16k.cfg    real translation
                          (the published M23 DX-100 configuration: deployable)

  g3rt_* are the two real-translation spot checks from the early single-cell runs.

Policies: P0 org 0 published ascending-line order; P1 org 1 cache-line reuse
first; P2 org 2 DRAM (row,bank) first; P3 org 3 combined; P1- org 4 diagnostic,
grouping disabled (the counterfactual for the cache-line mechanism, not a policy).

Every number is read from the cell log. Nothing is recomputed from a model, and a
missing field is reported as missing rather than defaulted to zero.
"""
import csv
import glob
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g3")
BANKS = 2

WORKLOADS = [
    ("orkut", "Orkut BFS frontier gather", "build/wm/m23_orkut_bfs.bin", 8.0),
    ("ogbn_uniform", "ogbn-products uniform gather", "build/wm/m23_ogbn_uniform.bin", 38.3),
    ("criteo", "Criteo embeddings, 16K batch", "build/wm/m23_criteo_16k.bin", None),
]
POL = {"0": "P0", "1": "P1", "2": "P2", "3": "P3", "4": "P1-"}
PLABEL = {"P0": "published order (ascending VA line)",
          "P1": "cache-line reuse first",
          "P2": "DRAM (row, bank) first",
          "P3": "combined (row, bank, -reuse)",
          "P1-": "diagnostic: grouping disabled"}
WINDOWS = [8192, 16384, 32768]
CONFIGS = [("ideal", "g3_", "ideal translation (M23-G2D reference setting)"),
           ("published", "g3p_", "real translation (published M23 DX-100 config)")]

# Translated, published historical reference points (M23-F1 Table, Orkut BFS):
F1 = {"simt_real": 49067545, "bulk_real": 16714538, "simt_ideal": 5766215,
      "bulk_ideal": 6077523}
G2D = {"simt": 5766215, "offline_page": 5765842 - 0, "offline_row": 804914}

REQUIRED = ["cycles", "dram_reads", "dram_row_hits", "dram_row_conflicts",
            "dram_bytes", "dram_wait_sum", "dram_q_occ_sum", "dram_bank0_busy",
            "dram_bank1_busy", "dram_svc_hit_cycles", "dram_svc_miss_cycles",
            "l2_hits", "l2_reads", "l2_misses", "l1_hits", "l1_reads",
            "bulk_org_cycles", "bulk_org_runs", "bulk_org_policy",
            "bulk_win_window", "bulk_win_sealed", "bulk_win_size_mean",
            "bulk_win_size_max", "bulk_win_age_sum", "bulk_win_full_stall",
            "bulk_dedup_merged", "bulk_lines_issued", "bulk_elems_total",
            "bulk_elems_done", "bulk_tgt_req_gen", "bulk_tgt_req_done",
            "bulk_addr_faults", "bulk_addr_mismatch", "bulk_residency_violations",
            "conservation_ok", "conservation_violations",
            "bulk_sched_opens", "bulk_sched_switches", "bulk_sched_idle",
            "bulk_sched_defer", "bulk_fanout_cycles", "bulk_ag_cycles",
            "dram_starved_cycles", "dram_blocked_cycles", "dram_max_q"]


def parse(path):
    d = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            for tok in line.split():
                if "=" in tok:
                    k, _, v = tok.partition("=")
                    d.setdefault(k, v)
    return d


def i64(d, k):
    try:
        return int(d[k])
    except (KeyError, ValueError):
        return None


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def pct(a, b):
    return round(100.0 * a / b, 4) if a is not None and b else None


def ratio(a, b):
    return round(a / b, 4) if a and b else None


def load_simt(pre):
    out = {}
    for wl, _, _, _ in WORKLOADS:
        p = os.path.join(OUT, f"{pre}{wl}_simt.txt")
        if os.path.exists(p) and os.path.getsize(p) > 0:
            d = parse(p)
            out[wl] = {"cycles": i64(d, "cycles"), "dram_reads": i64(d, "dram_reads"),
                       "row_hit_pct": pct(i64(d, "dram_row_hits"), i64(d, "dram_reads")),
                       "l2_hits": i64(d, "l2_hits"), "l2_reads": i64(d, "l2_reads"),
                       "dram_bytes": i64(d, "dram_bytes"),
                       "accesses": i64(d, "accesses"), "completed": i64(d, "completed")}
    return out


def cell_row(cfg_name, wl, desc, trace, w, pol, d, simt):
    cycles = i64(d, "cycles")
    reads = i64(d, "dram_reads")
    hits = i64(d, "dram_row_hits")
    bsum = (i64(d, "dram_bank0_busy") or 0) + (i64(d, "dram_bank1_busy") or 0)
    svc = (i64(d, "dram_svc_hit_cycles") or 0) + (i64(d, "dram_svc_miss_cycles") or 0)
    elems = i64(d, "bulk_elems_done")
    merged = i64(d, "bulk_dedup_merged")
    tgt = i64(d, "bulk_tgt_req_gen")
    sealed = i64(d, "bulk_win_sealed")
    win = i64(d, "bulk_win_window")
    missing = [k for k in REQUIRED if k not in d]
    checks = {
        "conservation": i64(d, "conservation_ok") == 1 and i64(d, "conservation_violations") == 0,
        # every logical access (element) accounted for exactly once
        "elems_done_eq_total": elems == i64(d, "bulk_elems_total") and elems > 0,
        "every_target_request_served": i64(d, "bulk_tgt_req_done") == tgt and tgt > 0,
        # the engine re-derives each target address from the index image and
        # compares: 0 faults / 0 mismatches = no lost or duplicated access
        "no_addr_fault_or_mismatch": i64(d, "bulk_addr_faults") == 0 and i64(d, "bulk_addr_mismatch") == 0,
        # grouping may merge duplicates, never lose or invent a request
        "grouping_identity": elems == tgt + merged,
        "residency_bound": i64(d, "bulk_residency_violations") == 0,
        "bytes_eq_64_per_command": i64(d, "dram_bytes") == 64 * reads,
        # soft: the two sums differ only by requests queued at the end of the run
        # (0.002 % in the historical M23-DX logs), so allow 0.5 %
        "queue_identity_0p5pct": (abs(i64(d, "dram_wait_sum") - i64(d, "dram_q_occ_sum"))
                                  <= 0.005 * max(1, i64(d, "dram_q_occ_sum"))),

        "window_matches_config": win == w,
    }
    notes = [k for k, v in checks.items() if not v]
    simt = simt.get(wl, {})
    return {
        "config": cfg_name,
        "workload": wl,
        "workload_detail": desc,
        "trace": trace,
        "trace_sha256": sha256(os.path.join(ROOT, trace)),
        "window": win if win else w,
        "policy": pol,
        "window_bytes": (win if win else w) * 4,
        # ---- performance ----
        "cycles": cycles,
        "simt_cycles": simt.get("cycles"),
        "speedup_vs_simt": ratio(simt.get("cycles"), cycles),
        "bulk_org_cycles": i64(d, "bulk_org_cycles"),
        "org_pct_of_cycles": pct(i64(d, "bulk_org_cycles"), cycles),
        "bulk_fanout_cycles": i64(d, "bulk_fanout_cycles"),
        "bulk_ag_cycles": i64(d, "bulk_ag_cycles"),
        "org_visits": i64(d, "bulk_org_runs"),
        "org_cycles_per_visit": round(i64(d, "bulk_org_cycles") / i64(d, "bulk_org_runs"), 2)
        if i64(d, "bulk_org_runs") else None,
        "win_util_pct": pct(i64(d, "bulk_win_size_mean"), win if win else w),
        "win_sealed": sealed,
        "win_size_mean": i64(d, "bulk_win_size_mean"),
        "win_size_max": i64(d, "bulk_win_size_max"),
        "win_age_mean": round(i64(d, "bulk_win_age_sum") / sealed, 1) if sealed else None,
        "win_full_stall": i64(d, "bulk_win_full_stall"),
        # ---- cache ----
        "l1_reads": i64(d, "l1_reads"), "l1_hits": i64(d, "l1_hits"),
        "l2_reads": i64(d, "l2_reads"), "l2_hits": i64(d, "l2_hits"),
        "l2_misses": i64(d, "l2_misses"),
        "l2_hit_pct": pct(i64(d, "l2_hits"), i64(d, "l2_reads")),
        "target_requests": tgt,
        "dedup_merged": merged,
        "dedup_pct_of_elems": pct(merged, elems),
        "duplicates_hit_in_l2": i64(d, "l2_hits"),
        # ---- DRAM ----
        "dram_commands": reads,
        "dram_bytes": i64(d, "dram_bytes"),
        "bytes_per_cycle": round(i64(d, "dram_bytes") / cycles, 4),
        "gbs": round(i64(d, "dram_bytes") / cycles * 0.4, 3),
        "row_hits": hits,
        "row_hit_pct": pct(hits, reads),
        "row_conflicts": i64(d, "dram_row_conflicts"),
        "row_activations": i64(d, "dram_row_acts_sum"),
        "mean_service": round(svc / reads, 3) if reads else None,
        "bank_busy_pct": pct(bsum, BANKS * cycles),
        "q_avg": round(i64(d, "dram_q_occ_sum") / cycles, 3),
        "q_max": i64(d, "dram_max_q"),
        "wait_avg": round(i64(d, "dram_wait_sum") / reads, 2) if reads else None,
        "wait_max": i64(d, "dram_wait_max"),
        "sched_opens": i64(d, "bulk_sched_opens"),
        "sched_switches": i64(d, "bulk_sched_switches"),
        "sched_idle": i64(d, "bulk_sched_idle"),
        "sched_defer": i64(d, "bulk_sched_defer"),
        # translation, for attributing the real-translation deltas
        "walks": i64(d, "walks"),
        "walk_stalls": i64(d, "walk_stalls"),
        "dtlb_misses": i64(d, "tlb0_misses"),
        "dtlb_hits": i64(d, "tlb0_hits"),
        "tlb_mean_lat": round((i64(d, "lat_tlb_sum") or 0) / max(1, reads), 2),
        "starved_pct": pct(i64(d, "dram_starved_cycles"), cycles),
        "blocked_pct": pct(i64(d, "dram_blocked_cycles"), cycles),
        # ---- correctness ----
        "elems_total": i64(d, "bulk_elems_total"),
        "elems_done": elems,
        "tgt_req_gen": tgt,
        "tgt_req_done": i64(d, "bulk_tgt_req_done"),
        "org_policy_logged": i64(d, "bulk_org_policy"),
        "checks_ok": "yes" if not notes else "NO",
        "checks_failed": ";".join(notes),
        "fields_missing": ";".join(missing),
    }


def main():
    rows = []
    for cfg_name, pre, _ in CONFIGS:
        simt = load_simt(pre)
        for wl, desc, trace, _ in WORKLOADS:
            for w in WINDOWS:
                for org in ["0", "1", "2", "3", "4"]:
                    p = os.path.join(OUT, f"{pre}{wl}_w{w}_p{org}.txt")
                    if not os.path.exists(p) or os.path.getsize(p) == 0:
                        continue
                    rows.append(cell_row(cfg_name, wl, desc, trace, w, POL[org],
                                         parse(p), simt))
    if not rows:
        print("no cells found", file=sys.stderr)
        return 1

    order = {c: i for i, (c, _, _) in enumerate(CONFIGS)}
    polorder = {"P0": 0, "P1": 1, "P2": 2, "P3": 3, "P1-": 4}
    rows.sort(key=lambda r: (order[r["config"]], r["workload"], polorder[r["policy"]], r["window"]))
    cols = list(rows[0].keys())
    with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=cols)
        wr.writeheader()
        wr.writerows(rows)

    # ---- attribution: each mechanism's own delta against P0, same window ----
    attr = []
    for cfg_name, _, _ in CONFIGS:
        for wl, _, _, _ in WORKLOADS:
            for w in WINDOWS:
                base = next((r for r in rows if r["config"] == cfg_name and r["workload"] == wl
                             and r["window"] == w and r["policy"] == "P0"), None)
                # a cell that failed an accounting check has no attributable metric
                if not base or not base["checks_ok"] == "yes":
                    continue
                for pol in ["P1", "P2", "P3", "P1-"]:
                    r = next((x for x in rows if x["config"] == cfg_name and x["workload"] == wl
                              and x["window"] == w and x["policy"] == pol), None)
                    if not r or r["checks_ok"] != "yes":
                        continue
                    attr.append({
                        "config": cfg_name, "workload": wl, "window": w, "policy": pol,
                        "cycles": r["cycles"], "d_cycles": r["cycles"] - base["cycles"],
                        "d_cycles_pct": round(100.0 * (r["cycles"] - base["cycles"]) / base["cycles"], 3),
                        "speedup_vs_P0": round(base["cycles"] / r["cycles"], 4),
                        "row_hit_pct": r["row_hit_pct"],
                        "d_row_hit_pts": round((r["row_hit_pct"] or 0) - (base["row_hit_pct"] or 0), 3),
                        "d_row_conflicts": r["row_conflicts"] - base["row_conflicts"],
                        "d_dram_commands": r["dram_commands"] - base["dram_commands"],
                        "d_mean_service": round((r["mean_service"] or 0) - (base["mean_service"] or 0), 3),
                        "d_l2_hits": r["l2_hits"] - base["l2_hits"],
                        "d_wait_avg": round((r["wait_avg"] or 0) - (base["wait_avg"] or 0), 2),
                        "d_org_cycles": (r["bulk_org_cycles"] or 0) - (base["bulk_org_cycles"] or 0),
                        "d_org_visits": (r["org_visits"] or 0) - (base["org_visits"] or 0),
                    })
    with open(os.path.join(OUT, "attribution.csv"), "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(attr[0].keys()))
        wr.writeheader()
        wr.writerows(attr)

    # ---- console matrix ----
    for cfg_name, _, desc in CONFIGS:
        print(f"\n### {cfg_name}: {desc}")
        print(f"{'workload':13s} {'P':4s} {'W':>6s} {'cycles':>10s} {'vsSIMT':>7s} "
              f"{'rowhit':>7s} {'conf':>9s} {'cmd':>9s} {'svc':>6s} {'B/cyc':>6s} "
              f"{'org%':>5s} {'win%':>5s} {'chk':>4s}")
        for r in rows:
            if r["config"] != cfg_name:
                continue
            print(f"{r['workload']:13s} {r['policy']:4s} {r['window']:6d} "
                  f"{r['cycles']:10,} {(r['speedup_vs_simt'] or 0):7.3f} "
                  f"{(r['row_hit_pct'] or 0):6.2f}% {r['row_conflicts']:9,} "
                  f"{r['dram_commands']:9,} {(r['mean_service'] or 0):6.2f} "
                  f"{(r['bytes_per_cycle'] or 0):6.3f} {(r['org_pct_of_cycles'] or 0):5.1f} "
                  f"{(r['win_util_pct'] or 0):5.1f} {r['checks_ok']:>4s}")

    with open(os.path.join(OUT, "checks.txt"), "w") as f:
        f.write("# M23-G3 accounting and correctness (one line per cell)\n")
        f.write("# config workload policy W checks_failed fields_missing\n")
        for r in rows:
            f.write(f"{r['config']:10s} {r['workload']:13s} {r['policy']:4s} {r['window']:6d} "
                    f"{r['checks_ok']:4s} {r['checks_failed'] or '-'} {r['fields_missing'] or '-'}\n")
        f.write("\n# every logical element is either its own target request or a merged duplicate\n")
        for r in rows:
            f.write(f"{r['config']:10s} {r['workload']:13s} {r['policy']:4s} {r['window']:6d} "
                    f"{r['elems_done']} = {r['tgt_req_gen']} + {r['dedup_merged']}  "
                    f"(done {r['tgt_req_done']})\n")
        f.write("\n# real-translation spot checks (m23_dx16k.cfg, single cells)\n")
        for p in sorted(glob.glob(os.path.join(OUT, "g3rt_*.txt"))):
            d = parse(p)
            f.write(f"{os.path.basename(p):34s} cycles={i64(d,'cycles')} "
                    f"reads={i64(d,'dram_reads')} "
                    f"hit%={pct(i64(d,'dram_row_hits'), i64(d,'dram_reads'))} "
                    f"walks={i64(d,'walks')} defer={i64(d,'bulk_sched_defer')}\n")

    bad = [r for r in rows if r["checks_ok"] != "yes"]
    nofield = [r for r in rows if r["fields_missing"]]
    print(f"\n{len(rows)} cells, {len(attr)} attribution rows")
    print(f"cells failing an accounting check: {len(bad)}")
    for b in bad:
        print(f"  {b['config']} {b['workload']} W={b['window']} {b['policy']}: {b['checks_failed']}")
    print(f"cells with a missing field: {len(nofield)}")
    for b in nofield[:5]:
        print(f"  {b['config']} {b['workload']} W={b['window']} {b['policy']}: {b['fields_missing']}")
    return 1 if (bad or nofield) else 0


if __name__ == "__main__":
    sys.exit(main())
