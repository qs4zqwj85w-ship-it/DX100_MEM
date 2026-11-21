#!/usr/bin/env python3
"""M23-G4A — extract the translation-sensitivity cells, check accounting, tabulate.

Cells:  g4_<wl>_<tag>_p<org>.txt   configs/m23g4a_base.cfg, batch_window = 16384
        tag m4w2  dTLB MSHR 4, 2 walkers   (published DX-100 baseline)
            m8w2  dTLB MSHR 8, 2 walkers
            m4w4  dTLB MSHR 4, 4 walkers
            m8w4  dTLB MSHR 8, 4 walkers
            ideal ideal_translation = 1     (reference only, not deployable)
        org 0 = P0 published order, 2 = P2 DRAM (row,bank) first

Nothing is recomputed from a model: every number is read from a cell log, and a
missing field is reported as missing rather than defaulted to zero. A cell that
ran into the cycle guard is reported as DNF and never quoted as a result.

Outputs (build/wm/m23g4a/): summary.csv, checks.txt, tables.md
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g4a")
BANKS = 2
WINDOW = 16384
GUARD = 200_000_000

WORKLOADS = [
    ("orkut", "Orkut BFS"),
    ("ogbn_uniform", "ogbn-products uniform gather"),
    ("criteo", "Criteo embeddings 16K"),
]
TAGS = [("m4w2", "4/2"), ("m8w2", "8/2"), ("m4w4", "4/4"), ("m8w4", "8/4"),
        ("ideal", "ideal")]
POL = {"0": "P0", "2": "P2"}

REQUIRED = [
    "cycles", "tlb0_reads", "tlb0_hits", "tlb0_misses", "tlb0_mshr_stalls",
    "tlb0_mshr_occ", "tlb0_mshr_max", "tlb_refuse_parked", "tlb_refuse_full",
    "tlb_max_parked", "l2tlb_reads", "l2tlb_hits", "l2tlb_misses",
    "l2tlb_walk_stalls", "l2tlb_max_occ", "walks", "pte_fetches",
    "walker_busy", "walker_max", "lat_tlb_sum", "lat_tlb_mean",
    "dram_reads", "dram_row_hits", "dram_row_conflicts", "dram_bytes",
    "dram_svc_hit_cycles", "dram_svc_miss_cycles", "dram_bank0_busy",
    "dram_bank1_busy", "dram_wait_sum", "dram_q_occ_sum", "dram_starved_cycles",
    "dram_blocked_cycles",
    "bulk_org_cycles", "bulk_org_runs", "bulk_org_policy",
    "bulk_sched_opens", "bulk_sched_switches", "bulk_sched_idle",
    "bulk_sched_defer", "bulk_win_window", "bulk_win_sealed",
    "bulk_win_size_mean", "bulk_win_full_stall", "bulk_dedup_merged",
    "bulk_elems_total", "bulk_elems_done", "bulk_tgt_req_gen",
    "bulk_tgt_req_done", "bulk_addr_faults", "bulk_addr_mismatch",
    "bulk_residency_violations", "conservation_ok", "conservation_violations",
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


def cell(wl, desc, tag, tlabel, org, d):
    cycles = i64(d, "cycles")
    reads = i64(d, "dram_reads")
    hits = i64(d, "dram_row_hits")
    svc = (i64(d, "dram_svc_hit_cycles") or 0) + (i64(d, "dram_svc_miss_cycles") or 0)
    bsum = (i64(d, "dram_bank0_busy") or 0) + (i64(d, "dram_bank1_busy") or 0)
    elems = i64(d, "bulk_elems_done")
    tgt = i64(d, "bulk_tgt_req_gen")
    win = i64(d, "bulk_win_window") or WINDOW
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
        "window_matches_config": win == WINDOW,
        "org_policy_logged": i64(d, "bulk_org_policy") == int(org),
    }
    failed = [k for k, v in checks.items() if not v]

    return {
        "workload": wl,
        "workload_detail": desc,
        "translation": tag,
        "translation_label": tlabel,
        "policy": POL[org],
        "status": status,
        "checks_ok": "yes" if not failed else "NO",
        "checks_failed": ";".join(failed),
        "fields_missing": ";".join(missing),
        # ---- performance ----
        "cycles": cycles,
        "p0_cycles": None,          # filled in below
        "speedup_vs_p0": None,
        "dram_bytes_per_cycle": div(i64(d, "dram_bytes"), cycles),
        # ---- translation ----
        "dtlb_reads": i64(d, "tlb0_reads"),
        "dtlb_hits": i64(d, "tlb0_hits"),
        "dtlb_misses": i64(d, "tlb0_misses"),
        "dtlb_miss_pct": pct(i64(d, "tlb0_misses"), i64(d, "tlb0_reads")),
        "dtlb_mshr_stalls": i64(d, "tlb0_mshr_stalls"),
        "dtlb_mshr_max": i64(d, "tlb0_mshr_max"),
        "dtlb_mshr_avg": div(i64(d, "tlb0_mshr_occ"), cycles),
        "refuse_parked": i64(d, "tlb_refuse_parked"),
        "refuse_full": i64(d, "tlb_refuse_full"),
        "max_parked": i64(d, "tlb_max_parked"),
        "l2tlb_reads": i64(d, "l2tlb_reads"),
        "l2tlb_misses": i64(d, "l2tlb_misses"),
        "l2tlb_walk_stalls": i64(d, "l2tlb_walk_stalls"),
        "l2tlb_max_occ": i64(d, "l2tlb_max_occ"),
        "walks": i64(d, "walks"),
        "pte_fetches": i64(d, "pte_fetches"),
        "walker_max": i64(d, "walker_max"),
        "walker_busy_cycles": i64(d, "walker_busy"),
        "walker_avg": div(i64(d, "walker_busy"), cycles),
        "tlb_lat_mean": div(i64(d, "lat_tlb_sum"), max(1, i64(d, "tlb0_reads") or 1), 2),
        "lat_tlb_sum": i64(d, "lat_tlb_sum"),
        "walker_util_pct": None,    # filled in below (needs the walker count)
        # ---- DRAM ----
        "dram_commands": reads,
        "row_hits": hits,
        "row_hit_pct": pct(hits, reads),
        "row_conflicts": i64(d, "dram_row_conflicts"),
        "mean_service": div(svc, reads, 3),
        "bank_busy_pct": pct(bsum, BANKS * cycles) if cycles else None,
        "wait_avg": div(i64(d, "dram_wait_sum"), reads, 2),
        "starved_pct": pct(i64(d, "dram_starved_cycles"), cycles),
        "blocked_pct": pct(i64(d, "dram_blocked_cycles"), cycles),
        # ---- organization ----
        "sched_opens": i64(d, "bulk_sched_opens"),
        "sched_switches": i64(d, "bulk_sched_switches"),
        "sched_idle": i64(d, "bulk_sched_idle"),
        "sched_defer": i64(d, "bulk_sched_defer"),
        "org_cycles": i64(d, "bulk_org_cycles"),
        "org_runs": i64(d, "bulk_org_runs"),
        "org_pct": pct(i64(d, "bulk_org_cycles"), cycles),
        "win_sealed": i64(d, "bulk_win_sealed"),
        "win_util_pct": pct(i64(d, "bulk_win_size_mean"), win),
        "win_full_stall": i64(d, "bulk_win_full_stall"),
        # ---- conservation ----
        "elems": elems,
        "elems_total": i64(d, "bulk_elems_total"),
        "tgt_gen": tgt,
        "tgt_done": i64(d, "bulk_tgt_req_done"),
        "dedup_merged": i64(d, "bulk_dedup_merged"),
    }


def main():
    rows = []
    for wl, desc in WORKLOADS:
        for tag, tlabel in TAGS:
            for org in ("0", "2"):
                p = os.path.join(OUT, f"g4_{wl}_{tag}_p{org}.txt")
                if not os.path.exists(p) or os.path.getsize(p) == 0:
                    continue
                rows.append(cell(wl, desc, tag, tlabel, org, parse(p)))
    if not rows:
        print("no cells found", file=sys.stderr)
        return 1

    # P2 speedup against P0 at the SAME translation setting
    p0 = {(r["workload"], r["translation"]): r for r in rows if r["policy"] == "P0"}
    walkers = {"m4w2": 2, "m8w2": 2, "m4w4": 4, "m8w4": 4, "ideal": 0}
    for r in rows:
        base = p0.get((r["workload"], r["translation"]))
        if base and base["cycles"] and r["cycles"]:
            r["p0_cycles"] = base["cycles"]
            r["speedup_vs_p0"] = round(base["cycles"] / r["cycles"], 4)
        w = walkers[r["translation"]]
        if w and r["walker_busy_cycles"] is not None:
            r["walker_util_pct"] = pct(r["walker_busy_cycles"], w * r["cycles"])

    order = {t: i for i, (t, _) in enumerate(TAGS)}
    polorder = {"P0": 0, "P2": 1}
    rows.sort(key=lambda r: (list(dict(WORKLOADS)).index(r["workload"]), polorder[r["policy"]],
                             order[r["translation"]]))
    with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        wr.writeheader()
        wr.writerows(rows)

    # ---- checks ----
    lines = ["M23-G4A accounting", ""]
    bad = 0
    for r in rows:
        st = r["status"]
        ok = r["checks_ok"] == "yes" and st == "ok"
        if not ok:
            bad += 1
        lines.append(f"{r['workload']:13s} {r['translation']:6s} {r['policy']:2s} "
                     f"status={st:5s} checks={r['checks_ok']:3s}"
                     + (f" failed={r['checks_failed']}" if r["checks_failed"] else "")
                     + (f" missing={r['fields_missing']}" if r["fields_missing"] else ""))
    lines += ["", f"{len(rows)} cells, {len(rows) - bad} complete and clean, {bad} flagged"]
    with open(os.path.join(OUT, "checks.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))

    # ---- tables.md ----
    md = []

    def table(title, cols, key, fmt):
        md.append(f"### {title}\n")
        md.append("| Workload | Trans | " + " | ".join(c for c, _ in cols) + " |")
        md.append("|---|---|" + "---:|" * len(cols))
        for wl, desc in WORKLOADS:
            for tag, tlabel in TAGS:
                got = [r for r in rows if r["workload"] == wl and r["translation"] == tag]
                if not got:
                    continue
                for pol in ("P0", "P2"):
                    r = next((x for x in got if x["policy"] == pol), None)
                    if not r:
                        continue
                    cells = []
                    for _, f in cols:
                        if r["status"] != "ok":
                            cells.append("—")
                        else:
                            cells.append(fmt(r, f))
                    md.append(f"| {desc} | {tlabel} | {pol} | " + " | ".join(cells) + " |")
        md.append("")

    table("Performance", [("Cycles", "cycles"), ("vs P0", "speedup_vs_p0"),
                          ("B/cyc", "dram_bytes_per_cycle"), ("Org %", "org_pct"),
                          ("Win util %", "win_util_pct")], None,
          lambda r, f: (f"{r[f]:,}" if f == "cycles" else
                        f"{r[f]:.3f}" if f == "speedup_vs_p0" else
                        f"{r[f]:.3f}" if f == "dram_bytes_per_cycle" else
                        f"{r[f]:.1f}" if f == "org_pct" else f"{r[f]:.1f}"))
    table("Translation", [("dTLB miss", "dtlb_misses"), ("Walk", "walks"),
                          ("MSHR max", "dtlb_mshr_max"), ("MSHR avg", "dtlb_mshr_avg"),
                          ("Walkers max", "walker_max"), ("Walker util %", "walker_util_pct"),
                          ("MSHR stall cyc", "dtlb_mshr_stalls"),
                          ("L2TLB walk stall", "l2tlb_walk_stalls"),
                          ("Trans lat mean", "tlb_lat_mean")], None,
          lambda r, f: (f"{r[f]:,}" if isinstance(r[f], int) and r[f] >= 1000 else
                        f"{r[f]:.3f}" if isinstance(r[f], float) and f in
                        ("dtlb_mshr_avg",) else
                        str(r[f]) if not isinstance(r[f], float) else f"{r[f]:.2f}"))
    table("DRAM", [("Cmds", "dram_commands"), ("Row hit %", "row_hit_pct"),
                   ("Conflicts", "row_conflicts"), ("Service", "mean_service"),
                   ("Bank busy %", "bank_busy_pct"), ("Wait/cmd", "wait_avg")], None,
          lambda r, f: (f"{r[f]:,}" if isinstance(r[f], int) else
                        f"{r[f]:.2f}" if f == "mean_service" else f"{r[f]:.2f}"))
    table("Organization", [("Cohort opens", "sched_opens"), ("Switches", "sched_switches"),
                           ("Sched idle", "sched_idle"), ("Org cyc", "org_cycles"),
                           ("Org runs", "org_runs"), ("Dedup merged", "dedup_merged")], None,
          lambda r, f: f"{r[f]:,}")

    # P2 delta against P0 at each translation setting
    md.append("### P2 vs P0 at each translation setting\n")
    md.append("| Workload | Trans | Δcycles | Δrow hit (pt) | Δconflicts | Δservice | "
              "ΔDTLB miss | Δwalks | Δsched opens |")
    md.append("|---|---|---:|---:|---:|---:|---:|---:|---:|")
    for wl, desc in WORKLOADS:
        for tag, tlabel in TAGS:
            a = p0.get((wl, tag))
            b = next((r for r in rows if r["workload"] == wl and r["translation"] == tag
                      and r["policy"] == "P2"), None)
            if not a or not b or "ok" not in (a["status"], b["status"]) or \
                    a["status"] != b["status"]:
                continue
            if a["status"] != "ok" or b["status"] != "ok":
                md.append(f"| {desc} | {tlabel} | DNF | — | — | — | — | — | — |")
                continue
            md.append("| {} | {} | {:+.2f}% | {:+.2f} | {:+d} | {:+.3f} | {:+d} | {:+d} | {:+d} |"
                      .format(desc, tlabel,
                              100.0 * (b["cycles"] - a["cycles"]) / a["cycles"],
                              b["row_hit_pct"] - a["row_hit_pct"],
                              b["row_conflicts"] - a["row_conflicts"],
                              b["mean_service"] - a["mean_service"],
                              b["dtlb_misses"] - a["dtlb_misses"],
                              (b["walks"] or 0) - (a["walks"] or 0),
                              b["sched_opens"] - a["sched_opens"]))
    md.append("")

    with open(os.path.join(OUT, "tables.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
