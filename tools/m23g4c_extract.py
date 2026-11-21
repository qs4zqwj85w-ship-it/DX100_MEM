#!/usr/bin/env python3
"""M23-G4C — extract the DRAM-mapping cells, check accounting, tabulate.

Cells:  g4c_<map>_<wl>_w<W>_p<org>.txt   published translation (deployable)
        g4ci_<map>_<wl>_w<W>_p<org>.txt  ideal_translation = 1 (reference arm)
        map m0..m3 (see configs/m23g4c_base.cfg), org 0 = P0, 2 = P2.

Every number is read from the cell log; nothing is recomputed from a model and a
missing field is reported as missing rather than defaulted to zero. Each cell is
also checked against the *mapping it claims to run*, read from the log's own
`dram_map / row_bytes / dram_il_lines / capacity_mib` header line.

Window ceilings are joined from build/wm/m23g4c/ceilings.csv
(tools/m23g4c_ceiling.py), which is analysis over the trace, not simulation.

Outputs: summary.csv, checks.txt, tables.md
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g4c")
BANKS = 2
GUARD = 200_000_000

WORKLOADS = [("orkut", "Orkut BFS"), ("ogbn_uniform", "ogbn-products uniform")]
WINDOWS = [8192, 16384, 32768]
POL = {"0": "P0", "2": "P2"}
ARMS = [("pub", "g4c_", "published"), ("ideal", "g4ci_", "ideal")]
# name, row_bytes, rows, il, label
MAPS = [("m0", 1024, 4096, 1, "M0 16 lines/row, il 1 (production)"),
        ("m1", 512, 8192, 1, "M1 8 lines/row, il 1"),
        ("m2", 2048, 2048, 1, "M2 32 lines/row, il 1"),
        ("m3", 1024, 4096, 4, "M3 16 lines/row, il 4")]
MAPBY = {m: (rb, rr, il) for m, rb, rr, il, _ in MAPS}

REQUIRED = [
    "cycles", "dram_reads", "dram_row_hits", "dram_row_conflicts", "dram_bytes",
    "dram_svc_hit_cycles", "dram_svc_miss_cycles", "dram_bank0_busy",
    "dram_bank1_busy", "dram_bank0_reads", "dram_bank1_reads",
    "dram_bank0_hits", "dram_bank1_hits", "dram_bank0_conf", "dram_bank1_conf",
    "dram_wait_sum", "dram_q_occ_sum", "dram_rows_used", "dram_row_acts_sum",
    "dram_row_acts_max",
    "l2_reads", "l2_hits", "l2_misses",
    "tlb0_misses", "walks", "tlb0_mshr_stalls",
    "bulk_org_cycles", "bulk_org_runs", "bulk_org_policy",
    "bulk_sched_opens", "bulk_sched_idle",
    "bulk_win_window", "bulk_win_sealed", "bulk_dedup_merged",
    "bulk_lines_issued", "bulk_elems_total", "bulk_elems_done",
    "bulk_tgt_req_gen", "bulk_tgt_req_done", "bulk_addr_faults",
    "bulk_addr_mismatch", "bulk_residency_violations", "conservation_ok",
    "conservation_violations", "row_bytes", "dram_il_lines", "capacity_mib",
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


def cell(arm, m, wl, desc, w, org, d, ceil):
    cycles = i64(d, "cycles")
    reads = i64(d, "dram_reads")
    hits = i64(d, "dram_row_hits")
    svc = (i64(d, "dram_svc_hit_cycles") or 0) + (i64(d, "dram_svc_miss_cycles") or 0)
    b0 = i64(d, "dram_bank0_busy") or 0
    b1 = i64(d, "dram_bank1_busy") or 0
    elems = i64(d, "bulk_elems_done")
    tgt = i64(d, "bulk_tgt_req_gen")
    win = i64(d, "bulk_win_window") or w
    missing = [k for k in REQUIRED if k not in d]
    rb, rr, il = MAPBY[m]

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
        # the mapping this cell claims to run must be the mapping in the log
        "mapping_row_bytes": i64(d, "row_bytes") == rb,
        "mapping_interleave": i64(d, "dram_il_lines") == il,
        "mapping_capacity_8mib": i64(d, "capacity_mib") == 8,
        "bank_reads_sum_to_total": (i64(d, "dram_bank0_reads") or 0) +
                                   (i64(d, "dram_bank1_reads") or 0) == (reads or 0),
    }
    failed = [k for k, v in checks.items() if not v]

    return {
        "arm": arm, "mapping": m,
        "mapping_label": next(lab for mm, _, _, _, lab in MAPS if mm == m),
        "row_bytes": rb, "rows": rr, "il": il,
        "workload": wl, "workload_detail": desc, "window": win,
        "policy": POL[org], "status": status,
        "checks_ok": "yes" if not failed else "NO",
        "checks_failed": ";".join(failed),
        "fields_missing": ";".join(missing),
        # ---- performance ----
        "cycles": cycles, "p0_cycles": None, "speedup_vs_p0": None,
        "dram_bytes_per_cycle": div(i64(d, "dram_bytes"), cycles),
        # ---- locality ----
        "dram_commands": reads,
        "row_hits": hits,
        "row_hit_pct": pct(hits, reads),
        "row_conflicts": i64(d, "dram_row_conflicts"),
        "mean_service": div(svc, reads, 3),
        "svc_hit": i64(d, "dram_svc_hit_cycles"),
        "svc_miss": i64(d, "dram_svc_miss_cycles"),
        "rows_used": i64(d, "dram_rows_used"),
        "row_acts_sum": i64(d, "dram_row_acts_sum"),
        "row_acts_max": i64(d, "dram_row_acts_max"),
        "wait_avg": div(i64(d, "dram_wait_sum"), reads, 2),
        # ---- banks ----
        "bank0_busy_pct": pct(b0, cycles),
        "bank1_busy_pct": pct(b1, cycles),
        "bank_busy_pct": pct(b0 + b1, BANKS * cycles) if cycles else None,
        "bank0_reads": i64(d, "dram_bank0_reads"),
        "bank1_reads": i64(d, "dram_bank1_reads"),
        "bank0_hit_pct": pct(i64(d, "dram_bank0_hits"), i64(d, "dram_bank0_reads")),
        "bank1_hit_pct": pct(i64(d, "dram_bank1_hits"), i64(d, "dram_bank1_reads")),
        # ---- ceiling (analysis, from the trace) ----
        "ceiling_pct": ceil,
        "p2_over_ceiling": round(100.0 * (pct(hits, reads) or 0) / ceil, 1) if ceil else None,
        # ---- organization ----
        "org_cycles": i64(d, "bulk_org_cycles"),
        "org_runs": i64(d, "bulk_org_runs"),
        "org_pct": pct(i64(d, "bulk_org_cycles"), cycles),
        "sched_opens": i64(d, "bulk_sched_opens"),
        "sched_idle": i64(d, "bulk_sched_idle"),
        "win_util_pct": pct(i64(d, "bulk_win_size_mean"), win),
        # ---- translation (context) ----
        "dtlb_misses": i64(d, "tlb0_misses"),
        "walks": i64(d, "walks"),
        "dtlb_mshr_stalls": i64(d, "tlb0_mshr_stalls"),
        # ---- conservation ----
        "elems": elems, "tgt_gen": tgt, "tgt_done": i64(d, "bulk_tgt_req_done"),
        "dedup_merged": i64(d, "bulk_dedup_merged"),
        "lines_issued": i64(d, "bulk_lines_issued"),
    }


def load_ceilings():
    p = os.path.join(OUT, "ceilings.csv")
    out = {}
    if not os.path.exists(p):
        return out
    with open(p) as f:
        for r in csv.DictReader(f):
            out[(r["mapping"], r["workload"], int(r["window"]))] = float(r["ceiling_pct"])
    return out


def main():
    ceil = load_ceilings()
    rows = []
    for arm, pre, _ in ARMS:
        for m, _, _, _, _ in MAPS:
            for wl, desc in WORKLOADS:
                for w in WINDOWS:
                    for org in ("0", "2"):
                        p = os.path.join(OUT, f"{pre}{m}_{wl}_w{w}_p{org}.txt")
                        if not os.path.exists(p) or os.path.getsize(p) == 0:
                            continue
                        rows.append(cell(arm, m, wl, desc, w, org, parse(p),
                                         ceil.get((m, wl, w))))
    if not rows:
        print("no cells found", file=sys.stderr)
        return 1

    p0 = {(r["arm"], r["mapping"], r["workload"], r["window"]): r
          for r in rows if r["policy"] == "P0"}
    for r in rows:
        base = p0.get((r["arm"], r["mapping"], r["workload"], r["window"]))
        if base and base["cycles"] and r["cycles"]:
            r["p0_cycles"] = base["cycles"]
            r["speedup_vs_p0"] = round(base["cycles"] / r["cycles"], 4)

    armo = {a: i for i, (a, _, _) in enumerate(ARMS)}
    mapo = {m: i for i, (m, _, _, _, _) in enumerate(MAPS)}
    wlo = {w: i for i, (w, _) in enumerate(WORKLOADS)}
    po = {"P0": 0, "P2": 1}
    rows.sort(key=lambda r: (armo[r["arm"]], mapo[r["mapping"]], wlo[r["workload"]],
                             r["window"], po[r["policy"]]))
    with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        wr.writeheader()
        wr.writerows(rows)

    lines = ["M23-G4C accounting", ""]
    bad = 0
    for r in rows:
        ok = r["checks_ok"] == "yes" and r["status"] == "ok"
        if not ok:
            bad += 1
        lines.append(f"{r['arm']:6s} {r['mapping']} {r['workload']:13s} w{r['window']:<6d} "
                     f"{r['policy']:2s} status={r['status']:5s} checks={r['checks_ok']:3s}"
                     + (f" failed={r['checks_failed']}" if r["checks_failed"] else "")
                     + (f" missing={r['fields_missing']}" if r["fields_missing"] else ""))
    lines += ["", f"{len(rows)} cells, {len(rows) - bad} complete and clean, {bad} flagged"]
    with open(os.path.join(OUT, "checks.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines[-3:]))

    md = []

    def emit(title, cols, fmt):
        md.append(f"### {title}\n")
        md.append("| Arm | Map | Workload | W | " + " | ".join(c for c, _ in cols) + " |")
        md.append("|---|---|---|---|" + "---:|" * len(cols))
        for arm, _, _ in ARMS:
            for m, _, _, _, _ in MAPS:
                for wl, desc in WORKLOADS:
                    for w in WINDOWS:
                        for pol in ("P0", "P2"):
                            r = next((x for x in rows if x["arm"] == arm and x["mapping"] == m
                                      and x["workload"] == wl and x["window"] == w
                                      and x["policy"] == pol), None)
                            if not r:
                                continue
                            if r["status"] != "ok":
                                md.append(f"| {arm} | {m} | {desc} | {w//1024}K | {pol} | "
                                          + " | ".join("—" for _ in cols) + " |")
                                continue
                            md.append(f"| {arm} | {m} | {desc} | {w//1024}K | {pol} | "
                                      + " | ".join(fmt(r, f) for _, f in cols) + " |")
        md.append("")

    emit("Cycles and locality", [
        ("Cycles", "cycles"), ("vs P0", "speedup_vs_p0"), ("Row hit %", "row_hit_pct"),
        ("Ceiling %", "ceiling_pct"), ("P2/ceil %", "p2_over_ceiling"),
        ("Conflicts", "row_conflicts"), ("Cmds", "dram_commands"),
        ("Service", "mean_service"), ("Bank busy %", "bank_busy_pct"),
        ("B/cyc", "dram_bytes_per_cycle")],
         lambda r, f: (f"{r[f]:,}" if f in ("cycles", "conflicts", "dram_commands")
                       else f"{r[f]:.3f}" if f == "speedup_vs_p0"
                       else f"{r[f]:.2f}" if isinstance(r[f], float) else
                       ("—" if r[f] is None else str(r[f]))))

    emit("Per-bank distribution", [
        ("Bank0 share", "bank0_reads"), ("Bank1 share", "bank1_reads"),
        ("Bank0 hit %", "bank0_hit_pct"), ("Bank1 hit %", "bank1_hit_pct"),
        ("Bank0 busy %", "bank0_busy_pct"), ("Bank1 busy %", "bank1_busy_pct"),
        ("Rows used", "rows_used"), ("Acts", "row_acts_sum"), ("Max acts/row", "row_acts_max"),
        ("Org %", "org_pct")],
         lambda r, f: (f"{r[f]:,}" if f in ("bank0_reads", "bank1_reads", "rows_used",
                                            "row_acts_sum", "row_acts_max")
                       else ("—" if r[f] is None else (f"{r[f]:.2f}" if isinstance(r[f], float)
                                                       else str(r[f])))))

    # P2 delta against P0 within each mapping
    md.append("### P2 vs P0, per mapping\n")
    md.append("| Arm | Map | Workload | W | Δcycles | Δrow hit (pt) | Δconflicts | "
              "Δservice | Ceiling |")
    md.append("|---|---|---|---|---:|---:|---:|---:|---:|")
    for arm, _, _ in ARMS:
        for m, _, _, _, _ in MAPS:
            for wl, desc in WORKLOADS:
                for w in WINDOWS:
                    a = p0.get((arm, m, wl, w))
                    b = next((x for x in rows if x["arm"] == arm and x["mapping"] == m
                              and x["workload"] == wl and x["window"] == w
                              and x["policy"] == "P2"), None)
                    if not a or not b or a["status"] != "ok" or b["status"] != "ok":
                        continue
                    md.append("| {} | {} | {} | {}K | {:+.2f}% | {:+.2f} | {:+d} | {:+.3f} | {:.2f}% |"
                              .format(arm, m, desc, w // 1024,
                                      100.0 * (b["cycles"] - a["cycles"]) / a["cycles"],
                                      b["row_hit_pct"] - a["row_hit_pct"],
                                      b["row_conflicts"] - a["row_conflicts"],
                                      b["mean_service"] - a["mean_service"],
                                      a["ceiling_pct"] or 0))
    md.append("")

    with open(os.path.join(OUT, "tables.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
