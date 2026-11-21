#!/usr/bin/env python3
"""M23-G4B — extract the page/row-organization cells, check accounting, tabulate.

Cells:  g4b_<wl>_w<W>_p<org>.txt   published translation (deployable)
        g4bi_<wl>_w<W>_p<org>.txt  ideal_translation = 1 (reference only)
        org 0 = P0 published order, 2 = P2 (row, bank) first with cell cohorts,
            5 = P4 page-first, row-second with page cohorts

Every number is read from a cell log; a missing field is reported as missing
rather than defaulted to zero. A cell that hit the cycle guard or failed a
conservation identity is reported as DNF/flagged and never quoted as a result.

Outputs (build/wm/m23g4b/): summary.csv, checks.txt, tables.md
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g4b")
BANKS = 2
GUARD = 200_000_000

WORKLOADS = [("orkut", "Orkut BFS"), ("ogbn_uniform", "ogbn-products uniform")]
WINDOWS = [8192, 16384, 32768]
POL = {"0": "P0", "2": "P2", "5": "P4"}
ARMS = [("pub", "g4b_", "published"), ("ideal", "g4bi_", "ideal")]

REQUIRED = [
    "cycles", "tlb0_reads", "tlb0_hits", "tlb0_misses", "tlb0_mshr_stalls",
    "tlb0_mshr_max", "tlb0_mshr_occ", "tlb_refuse_parked", "tlb_refuse_full",
    "l2tlb_reads", "l2tlb_hits", "l2tlb_misses", "l2tlb_walk_stalls",
    "l2tlb_max_occ", "walks", "pte_fetches", "walker_busy", "walker_max",
    "lat_tlb_sum", "lat_tlb_mean",
    "dram_reads", "dram_row_hits", "dram_row_conflicts", "dram_bytes",
    "dram_svc_hit_cycles", "dram_svc_miss_cycles", "dram_bank0_busy",
    "dram_bank1_busy", "dram_wait_sum", "dram_q_occ_sum", "dram_starved_cycles",
    "dram_blocked_cycles",
    "bulk_org_cycles", "bulk_org_runs", "bulk_org_policy",
    "bulk_sched_opens", "bulk_sched_scan", "bulk_sched_switches",
    "bulk_sched_idle", "bulk_sched_defer", "bulk_win_window", "bulk_win_sealed",
    "bulk_win_size_mean", "bulk_win_full_stall", "bulk_dedup_merged",
    "bulk_lines_issued", "bulk_elems_total", "bulk_elems_done",
    "bulk_tgt_req_gen", "bulk_tgt_req_done", "bulk_addr_faults",
    "bulk_addr_mismatch", "bulk_residency_violations", "conservation_ok",
    "conservation_violations",
]


def parse(path):
    d = {}
    with open(path) as f:
        for line in f:
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


def div(a, b, nd=4):
    return round(a / b, nd) if a is not None and b else None


def pct(a, b):
    return round(100.0 * a / b, 4) if a is not None and b else None


def cell(arm, wl, desc, w, org, d):
    cycles = i64(d, "cycles")
    reads = i64(d, "dram_reads")
    hits = i64(d, "dram_row_hits")
    svc = (i64(d, "dram_svc_hit_cycles") or 0) + (i64(d, "dram_svc_miss_cycles") or 0)
    bsum = (i64(d, "dram_bank0_busy") or 0) + (i64(d, "dram_bank1_busy") or 0)
    elems = i64(d, "bulk_elems_done")
    tgt = i64(d, "bulk_tgt_req_gen")
    win = i64(d, "bulk_win_window") or w
    opens = i64(d, "bulk_sched_opens")
    scan = i64(d, "bulk_sched_scan")
    missing = [k for k in REQUIRED if k not in d]

    finished = (elems == i64(d, "bulk_elems_total") and elems and
                i64(d, "bulk_tgt_req_done") == tgt and tgt)
    guarded = cycles is not None and cycles >= GUARD
    status = "ok" if (finished and not guarded) else ("GUARD" if guarded else "DNF")

    checks = {
        "conservation": i64(d, "conservation_ok") == 1 and i64(d, "conservation_violations") == 0,
        "elems_done_eq_total": finished,
        "every_target_request_served": i64(d, "bulk_tgt_req_done") == tgt and bool(tgt),
        "no_addr_fault_or_mismatch": i64(d, "bulk_addr_faults") == 0 and i64(d, "bulk_addr_mismatch") == 0,
        "grouping_identity": elems == (tgt or 0) + (i64(d, "bulk_dedup_merged") or 0),
        "residency_bound": i64(d, "bulk_residency_violations") == 0,
        "bytes_eq_64_per_command": i64(d, "dram_bytes") == 64 * (reads or 0),
        "queue_identity_0p5pct": (abs((i64(d, "dram_wait_sum") or 0) -
                                      (i64(d, "dram_q_occ_sum") or 0))
                                  <= 0.005 * max(1, i64(d, "dram_q_occ_sum") or 0)),
        "window_matches_config": win == w,
        "org_policy_logged": i64(d, "bulk_org_policy") == int(org),
    }
    failed = [k for k, v in checks.items() if not v]

    return {
        "arm": arm,
        "workload": wl,
        "workload_detail": desc,
        "window": win,
        "policy": POL[org],
        "status": status,
        "checks_ok": "yes" if not failed else "NO",
        "checks_failed": ";".join(failed),
        "fields_missing": ";".join(missing),
        # ---- performance ----
        "cycles": cycles,
        "p0_cycles": None,
        "speedup_vs_p0": None,
        "dram_bytes_per_cycle": div(i64(d, "dram_bytes"), cycles),
        # ---- translation ----
        "dtlb_misses": i64(d, "tlb0_misses"),
        "dtlb_reads": i64(d, "tlb0_reads"),
        "dtlb_miss_pct": pct(i64(d, "tlb0_misses"), i64(d, "tlb0_reads")),
        "dtlb_mshr_stalls": i64(d, "tlb0_mshr_stalls"),
        "dtlb_mshr_max": i64(d, "tlb0_mshr_max"),
        "dtlb_mshr_avg": div(i64(d, "tlb0_mshr_occ"), cycles),
        "refuse_parked": i64(d, "tlb_refuse_parked"),
        "refuse_full": i64(d, "tlb_refuse_full"),
        "l2tlb_misses": i64(d, "l2tlb_misses"),
        "l2tlb_walk_stalls": i64(d, "l2tlb_walk_stalls"),
        "l2tlb_max_occ": i64(d, "l2tlb_max_occ"),
        "walks": i64(d, "walks"),
        "pte_fetches": i64(d, "pte_fetches"),
        "walker_max": i64(d, "walker_max"),
        "walker_busy": i64(d, "walker_busy"),
        "tlb_lat_mean": div(i64(d, "lat_tlb_sum"), max(1, i64(d, "tlb0_reads") or 1), 2),
        # ---- DRAM ----
        "dram_commands": reads,
        "row_hits": hits,
        "row_hit_pct": pct(hits, reads),
        "row_conflicts": i64(d, "dram_row_conflicts"),
        "mean_service": div(svc, reads, 3),
        "bank_busy_pct": pct(bsum, BANKS * cycles) if cycles else None,
        "wait_avg": div(i64(d, "dram_wait_sum"), reads, 2),
        "starved_pct": pct(i64(d, "dram_starved_cycles"), cycles),
        # ---- organization / issue ----
        "sched_opens": opens,
        "cohort_mean": div(scan, opens, 2),
        "opens_per_1k_runs": div(1000.0 * (opens or 0), i64(d, "bulk_lines_issued"), 2),
        "sched_switches": i64(d, "bulk_sched_switches"),
        "sched_idle": i64(d, "bulk_sched_idle"),
        "sched_idle_pct": pct(i64(d, "bulk_sched_idle"), cycles),
        "sched_defer": i64(d, "bulk_sched_defer"),
        "org_cycles": i64(d, "bulk_org_cycles"),
        "org_runs": i64(d, "bulk_org_runs"),
        "org_pct": pct(i64(d, "bulk_org_cycles"), cycles),
        "win_util_pct": pct(i64(d, "bulk_win_size_mean"), win),
        "win_full_stall": i64(d, "bulk_win_full_stall"),
        "lines_issued": i64(d, "bulk_lines_issued"),
        # ---- conservation ----
        "elems": elems,
        "elems_total": i64(d, "bulk_elems_total"),
        "tgt_gen": tgt,
        "tgt_done": i64(d, "bulk_tgt_req_done"),
        "dedup_merged": i64(d, "bulk_dedup_merged"),
    }


def main():
    rows = []
    for arm, pre, _ in ARMS:
        for wl, desc in WORKLOADS:
            for w in WINDOWS:
                for org in ("0", "2", "5"):
                    p = os.path.join(OUT, f"{pre}{wl}_w{w}_p{org}.txt")
                    if not os.path.exists(p) or os.path.getsize(p) == 0:
                        continue
                    rows.append(cell(arm, wl, desc, w, org, parse(p)))
    if not rows:
        print("no cells found", file=sys.stderr)
        return 1

    p0 = {(r["arm"], r["workload"], r["window"]): r for r in rows if r["policy"] == "P0"}
    for r in rows:
        base = p0.get((r["arm"], r["workload"], r["window"]))
        if base and base["cycles"] and r["cycles"]:
            r["p0_cycles"] = base["cycles"]
            r["speedup_vs_p0"] = round(base["cycles"] / r["cycles"], 4)

    armorder = {a: i for i, (a, _, _) in enumerate(ARMS)}
    wlorder = {w: i for i, (w, _) in enumerate(WORKLOADS)}
    polorder = {"P0": 0, "P2": 1, "P4": 2}
    rows.sort(key=lambda r: (armorder[r["arm"]], wlorder[r["workload"]],
                             r["window"], polorder[r["policy"]]))
    with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        wr.writeheader()
        wr.writerows(rows)

    lines = ["M23-G4B accounting", ""]
    bad = 0
    for r in rows:
        ok = r["checks_ok"] == "yes" and r["status"] == "ok"
        if not ok:
            bad += 1
        lines.append(f"{r['arm']:7s} {r['workload']:13s} w{r['window']:<6d} {r['policy']:2s} "
                     f"status={r['status']:5s} checks={r['checks_ok']:3s}"
                     + (f" failed={r['checks_failed']}" if r["checks_failed"] else "")
                     + (f" missing={r['fields_missing']}" if r["fields_missing"] else ""))
    lines += ["", f"{len(rows)} cells, {len(rows) - bad} complete and clean, {bad} flagged"]
    with open(os.path.join(OUT, "checks.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))

    # ---- main table: per arm/workload/window, the three policies ----
    md = ["### Cycles, locality and issue structure\n"]
    md.append("| Arm | Workload | W | Pol | Cycles | vs P0 | Row hit % | Conflicts | "
              "Service | Cmds | Opens | Idle % | Org % |")
    md.append("|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for arm, _, armdesc in ARMS:
        for wl, desc in WORKLOADS:
            for w in WINDOWS:
                for pol in ("P0", "P2", "P4"):
                    r = next((x for x in rows if x["arm"] == arm and x["workload"] == wl
                              and x["window"] == w and x["policy"] == pol), None)
                    if not r:
                        continue
                    if r["status"] != "ok":
                        md.append(f"| {arm} | {desc} | {w//1024}K | {pol} | "
                                  f"{r['status']} | — | — | — | — | — | — | — | — |")
                        continue
                    md.append("| {} | {} | {}K | {} | {:,} | {:.3f} | {:.2f} | {:,} | {:.2f} | "
                              "{:,} | {:,} | {:.1f} | {:.1f} |".format(
                                  arm, desc, w // 1024, pol, r["cycles"],
                                  r["speedup_vs_p0"], r["row_hit_pct"], r["row_conflicts"],
                                  r["mean_service"], r["dram_commands"], r["sched_opens"],
                                  r["sched_idle_pct"], r["org_pct"]))
    md.append("")

    md.append("### Translation\n")
    md.append("| Arm | Workload | W | Pol | dTLB miss | Walk | MSHR max | MSHR avg | "
              "Walker max | MSHR stall cyc | L2TLB walk stall | TLB lat mean |")
    md.append("|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for arm, _, _ in ARMS:
        for wl, desc in WORKLOADS:
            for w in WINDOWS:
                for pol in ("P0", "P2", "P4"):
                    r = next((x for x in rows if x["arm"] == arm and x["workload"] == wl
                              and x["window"] == w and x["policy"] == pol), None)
                    if not r or r["status"] != "ok":
                        continue
                    md.append("| {} | {} | {}K | {} | {:,} | {:,} | {} | {:.2f} | {} | "
                              "{:,} | {:,} | {:.2f} |".format(
                                  arm, desc, w // 1024, pol, r["dtlb_misses"], r["walks"],
                                  r["dtlb_mshr_max"], r["dtlb_mshr_avg"], r["walker_max"],
                                  r["dtlb_mshr_stalls"], r["l2tlb_walk_stalls"],
                                  r["tlb_lat_mean"]))
    md.append("")

    md.append("### Delta against P0, same arm/workload/window\n")
    md.append("| Arm | Workload | W | Pol | Δcycles | Δrow hit (pt) | Δconflicts | "
              "Δservice | Δopens | Δidle | ΔDTLB miss | Δwalks |")
    md.append("|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for arm, _, _ in ARMS:
        for wl, desc in WORKLOADS:
            for w in WINDOWS:
                a = p0.get((arm, wl, w))
                if not a or a["status"] != "ok":
                    continue
                for pol in ("P2", "P4"):
                    b = next((x for x in rows if x["arm"] == arm and x["workload"] == wl
                              and x["window"] == w and x["policy"] == pol), None)
                    if not b:
                        continue
                    if b["status"] != "ok":
                        md.append(f"| {arm} | {desc} | {w//1024}K | {pol} | DNF | — | — | — | "
                                  f"— | — | — | — |")
                        continue
                    md.append("| {} | {} | {}K | {} | {:+.2f}% | {:+.2f} | {:+d} | {:+.3f} | "
                              "{:+d} | {:+d} | {:+d} | {:+d} |".format(
                                  arm, desc, w // 1024, pol,
                                  100.0 * (b["cycles"] - a["cycles"]) / a["cycles"],
                                  b["row_hit_pct"] - a["row_hit_pct"],
                                  b["row_conflicts"] - a["row_conflicts"],
                                  b["mean_service"] - a["mean_service"],
                                  b["sched_opens"] - a["sched_opens"],
                                  b["sched_idle"] - a["sched_idle"],
                                  b["dtlb_misses"] - a["dtlb_misses"],
                                  (b["walks"] or 0) - (a["walks"] or 0)))
    md.append("")

    with open(os.path.join(OUT, "tables.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
