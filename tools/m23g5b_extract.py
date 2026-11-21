#!/usr/bin/env python3
"""M23-G5B — extract the cross-graph cells, check the accounting, compare to Orkut.

  g5b_<w>_p<P>.txt    configs/m23g5b_base.cfg   ideal_translation = 1
  g5bp_<w>_p<P>.txt   configs/m23_dx16k.cfg     real translation (deployable)

Policies: P0 bulk_org 0 (published ascending-line order), P2 bulk_org 2 (DRAM
(row,bank) first), P3 bulk_org 3 (combined).

Every number is read from a cell log. Nothing is recomputed from a model, and a
missing field is reported as missing rather than defaulted to zero. The Orkut
reference column is read from the M23-G3 summary (build/wm/m23g3/summary.csv),
not re-typed, so the cross-graph comparison cannot drift from the published run.
"""
import csv
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g5b")
G3 = os.path.join(ROOT, "build/wm/m23g3/summary.csv")
BANKS = 2

WL = ("livejournal", "LiveJournal BFS frontier gather", "build/wm/m23_livejournal_bfs.bin")
ORKUT = ("orkut", "Orkut BFS frontier gather", "build/wm/m23_orkut_bfs.bin")
POL = {"0": "P0", "2": "P2", "3": "P3"}
PLABEL = {"P0": "published order (ascending VA line)",
          "P2": "DRAM (row, bank) first",
          "P3": "combined (row, bank, -reuse)"}
WINDOWS = [8192, 16384, 32768]
CONFIGS = [("ideal", "g5b_", "ideal translation (M23-G2D reference setting)"),
           ("published", "g5bp_", "real translation (published M23 DX-100 config)")]

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


def pct(a, b):
    return round(100.0 * a / b, 4) if a is not None and b else None


def ratio(a, b):
    return round(a / b, 4) if a and b else None


def meta(path):
    with open(path + ".json") as f:
        return json.load(f)


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
        "elems_done_eq_total": elems == i64(d, "bulk_elems_total") and elems and elems > 0,
        "every_target_request_served": i64(d, "bulk_tgt_req_done") == tgt and tgt and tgt > 0,
        "no_addr_fault_or_mismatch": i64(d, "bulk_addr_faults") == 0 and i64(d, "bulk_addr_mismatch") == 0,
        "grouping_identity": elems == tgt + merged,
        "residency_bound": i64(d, "bulk_residency_violations") == 0,
        "bytes_eq_64_per_command": i64(d, "dram_bytes") == 64 * reads,
        "queue_identity_0p5pct": (abs(i64(d, "dram_wait_sum") - i64(d, "dram_q_occ_sum"))
                                  <= 0.005 * max(1, i64(d, "dram_q_occ_sum"))),
        "window_matches_config": win == w,
    }
    notes = [k for k, v in checks.items() if not v]
    s = simt or {}
    return {
        "config": cfg_name, "workload": wl, "workload_detail": desc, "trace": trace,
        "window": win if win else w,
        "policy": pol, "window_bytes": (win if win else w) * 4,
        "cycles": cycles, "simt_cycles": s.get("cycles"),
        "speedup_vs_simt": ratio(s.get("cycles"), cycles),
        "bulk_org_cycles": i64(d, "bulk_org_cycles"),
        "org_pct_of_cycles": pct(i64(d, "bulk_org_cycles"), cycles),
        "bulk_fanout_cycles": i64(d, "bulk_fanout_cycles"),
        "bulk_ag_cycles": i64(d, "bulk_ag_cycles"),
        "org_visits": i64(d, "bulk_org_runs"),
        "org_cycles_per_visit": round(i64(d, "bulk_org_cycles") / i64(d, "bulk_org_runs"), 2)
        if i64(d, "bulk_org_runs") else None,
        "win_util_pct": pct(i64(d, "bulk_win_size_mean"), win if win else w),
        "win_sealed": sealed, "win_size_mean": i64(d, "bulk_win_size_mean"),
        "win_size_max": i64(d, "bulk_win_size_max"),
        "win_age_mean": round(i64(d, "bulk_win_age_sum") / sealed, 1) if sealed else None,
        "win_full_stall": i64(d, "bulk_win_full_stall"),
        "l1_reads": i64(d, "l1_reads"), "l1_hits": i64(d, "l1_hits"),
        "l2_reads": i64(d, "l2_reads"), "l2_hits": i64(d, "l2_hits"),
        "l2_misses": i64(d, "l2_misses"),
        "l2_hit_pct": pct(i64(d, "l2_hits"), i64(d, "l2_reads")),
        "target_requests": tgt, "dedup_merged": merged,
        "dedup_pct_of_elems": pct(merged, elems),
        "dram_commands": reads, "dram_bytes": i64(d, "dram_bytes"),
        "bytes_per_cycle": round(i64(d, "dram_bytes") / cycles, 4) if cycles else None,
        "row_hits": hits, "row_hit_pct": pct(hits, reads),
        "row_conflicts": i64(d, "dram_row_conflicts"),
        "row_activations": i64(d, "dram_row_acts_sum"),
        "mean_service": round(svc / reads, 3) if reads else None,
        "bank_busy_pct": pct(bsum, BANKS * cycles) if cycles else None,
        "q_avg": round(i64(d, "dram_q_occ_sum") / cycles, 3) if cycles else None,
        "q_max": i64(d, "dram_max_q"),
        "wait_avg": round(i64(d, "dram_wait_sum") / reads, 2) if reads else None,
        "wait_max": i64(d, "dram_wait_max"),
        "sched_opens": i64(d, "bulk_sched_opens"),
        "sched_switches": i64(d, "bulk_sched_switches"),
        "sched_idle": i64(d, "bulk_sched_idle"),
        "sched_defer": i64(d, "bulk_sched_defer"),
        "walks": i64(d, "walks"), "walk_stalls": i64(d, "walk_stalls"),
        "dtlb_misses": i64(d, "tlb0_misses"), "dtlb_hits": i64(d, "tlb0_hits"),
        "starved_pct": pct(i64(d, "dram_starved_cycles"), cycles) if cycles else None,
        "blocked_pct": pct(i64(d, "dram_blocked_cycles"), cycles) if cycles else None,
        "elems_total": i64(d, "bulk_elems_total"), "elems_done": elems,
        "tgt_req_gen": tgt, "tgt_req_done": i64(d, "bulk_tgt_req_done"),
        "org_policy_logged": i64(d, "bulk_org_policy"),
        "checks_ok": "yes" if not notes else "NO",
        "checks_failed": ";".join(notes), "fields_missing": ";".join(missing),
    }


def g3_rows():
    """Published M23-G3 cells, read from its own summary (not re-typed)."""
    if not os.path.exists(G3):
        return []
    with open(G3) as f:
        return list(csv.DictReader(f))


def fnum(r, k, scale=1.0):
    v = r.get(k)
    if v in (None, "", "None"):
        return None
    try:
        return float(v) / scale
    except ValueError:
        return None


def main():
    rows = []
    for cfg_name, pre, _ in CONFIGS:
        sp = os.path.join(OUT, f"{pre}livejournal_simt.txt")
        simt = None
        if os.path.exists(sp) and os.path.getsize(sp) > 0:
            d = parse(sp)
            simt = {"cycles": i64(d, "cycles")}
        for w in WINDOWS:
            for org in ["0", "2", "3"]:
                p = os.path.join(OUT, f"{pre}w{w}_p{org}.txt")
                if not os.path.exists(p) or os.path.getsize(p) == 0:
                    continue
                rows.append(cell_row(cfg_name, WL[0], WL[1], WL[2], w, POL[org],
                                     parse(p), simt))
    if not rows:
        print("no cells found", file=sys.stderr)
        return 1

    order = {c: i for i, (c, _, _) in enumerate(CONFIGS)}
    polorder = {"P0": 0, "P2": 1, "P3": 2}
    rows.sort(key=lambda r: (order[r["config"]], polorder[r["policy"]], r["window"]))
    with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        wr.writeheader()
        wr.writerows(rows)

    # ---- attribution: P2/P3 delta against P0 at the same window --------------
    attr = []
    for cfg_name, _, _ in CONFIGS:
        for w in WINDOWS:
            base = next((r for r in rows if r["config"] == cfg_name
                         and r["window"] == w and r["policy"] == "P0"), None)
            if not base or base["checks_ok"] != "yes":
                continue
            for pol in ["P2", "P3"]:
                r = next((x for x in rows if x["config"] == cfg_name
                          and x["window"] == w and x["policy"] == pol), None)
                if not r or r["checks_ok"] != "yes":
                    continue
                attr.append({
                    "config": cfg_name, "workload": WL[0], "window": w, "policy": pol,
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
    if attr:
        with open(os.path.join(OUT, "attribution.csv"), "w", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=list(attr[0].keys()))
            wr.writeheader()
            wr.writerows(attr)

    # ---- cross-graph table --------------------------------------------------
    lj = meta(os.path.join(ROOT, WL[2]))
    ork = meta(os.path.join(ROOT, ORKUT[2]))
    g3 = g3_rows()

    def shape(m):
        return {
            "accesses": m["kernel"]["accesses"],
            "vertices_selected": m["kernel"]["vertices_selected"],
            "subset_vertices": m["subset"]["vertices"],
            "induced_edges_undirected": m["subset"]["induced_edges_undirected"],
            "degree_mean": round(m["degree"]["mean"], 2),
            "degree_median": m["degree"]["median"],
            "degree_p90": m["degree"]["p90"],
            "degree_p99": m["degree"]["p99"],
            "degree_max": m["degree"]["max"],
            "selected_mean": round(m["degree"]["selected_mean"], 1),
            "selected_median": m["degree"]["selected_median"],
            "prop_array_mib": round(m["layout"]["prop_bytes"] / (1 << 20), 2),
            "unique_lines": m["totals"]["unique_lines"],
            "touched_mib": round(m["totals"]["working_set_bytes"] / (1 << 20), 2),
            "unique_pages": m["totals"]["unique_pages"],
            "dup_line_frac": m["totals"]["duplicate_line_frac"],
            "reuse_per_line": round(m["kernel"]["accesses"] / m["totals"]["unique_lines"], 3),
            "bulk_instr_frac": round(m["kernel"]["instructions_bulk"] / m["kernel"]["instructions"], 4),
            "trace_sha256": m["trace_sha256"],
        }

    cross = []
    for (name, desc, trace), m in [(WL, lj), (ORKUT, ork)]:
        sh = shape(m)
        for cfg_name in ["ideal", "published"]:
            # this experiment's rows for LiveJournal, G3's rows for Orkut
            if name == "livejournal":
                src = [r for r in rows if r["config"] == cfg_name]
            else:
                src = []
                for r in g3:
                    if r["workload"] != "orkut" or r["config"] != cfg_name:
                        continue
                    if r["policy"] not in ("P0", "P2", "P3"):
                        continue
                    src.append({"window": int(fnum(r, "window_bytes", 1) or 0) // 4,
                                "policy": r["policy"],
                                "cycles": int(fnum(r, "cycles", 1)),
                                "row_hit_pct": fnum(r, "row_hit_pct", 1),
                                "row_conflicts": int(fnum(r, "row_conflicts", 1) or 0),
                                "dram_commands": int(fnum(r, "dram_commands", 1) or 0),
                                "l2_hit_pct": fnum(r, "l2_hit_pct", 1),
                                "checks_ok": r["checks_ok"]})
            base = next((r for r in src if r["window"] == 16384 and r["policy"] == "P0"), None)
            for w in WINDOWS:
                b = next((r for r in src if r["window"] == w and r["policy"] == "P0"), None)
                if not b:
                    continue
                for pol in ["P2", "P3"]:
                    r = next((x for x in src if x["window"] == w and x["policy"] == pol), None)
                    if not r:
                        continue
                    rec = {"graph": name, "graph_detail": desc, "config": cfg_name,
                           "window": w, "policy": pol,
                           "p0_cycles": b["cycles"], "cycles": r["cycles"],
                           "d_cycles_pct": round(100.0 * (r["cycles"] - b["cycles"]) / b["cycles"], 3),
                           "speedup_vs_P0": round(b["cycles"] / r["cycles"], 4),
                           "p0_row_hit_pct": b["row_hit_pct"], "row_hit_pct": r["row_hit_pct"],
                           "d_row_hit_pts": round((r["row_hit_pct"] or 0) - (b["row_hit_pct"] or 0), 3),
                           "checks": r.get("checks_ok")}
                    rec.update({k: v for k, v in sh.items()})
                    cross.append(rec)
    if cross:
        with open(os.path.join(OUT, "crossgraph.csv"), "w", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=list(cross[0].keys()))
            wr.writeheader()
            wr.writerows(cross)

    # ---- console ------------------------------------------------------------
    for cfg_name, _, desc in CONFIGS:
        print(f"\n### LiveJournal — {cfg_name}: {desc}")
        print(f"{'P':4s} {'W':>6s} {'cycles':>10s} {'vsSIMT':>7s} {'rowhit':>7s} "
              f"{'conf':>8s} {'cmd':>8s} {'svc':>6s} {'L2hit':>6s} {'org%':>5s} "
              f"{'win%':>5s} {'walk':>7s} {'chk':>4s}")
        for r in rows:
            if r["config"] != cfg_name:
                continue
            print(f"{r['policy']:4s} {r['window']:6d} {r['cycles']:10,} "
                  f"{(r['speedup_vs_simt'] or 0):7.3f} {(r['row_hit_pct'] or 0):6.2f}% "
                  f"{r['row_conflicts']:8,} {r['dram_commands']:8,} "
                  f"{(r['mean_service'] or 0):6.2f} {(r['l2_hit_pct'] or 0):5.1f}% "
                  f"{(r['org_pct_of_cycles'] or 0):5.1f} {(r['win_util_pct'] or 0):5.1f} "
                  f"{r['walks'] if r['walks'] is not None else 0:7d} {r['checks_ok']:>4s}")

    if cross:
        print("\n### cross-graph: P2 delta vs P0, same config, same window")
        print(f"{'graph':12s} {'cfg':10s} {'W':>6s} {'P':3s} {'P0 cycles':>11s} "
              f"{'cycles':>11s} {'d%':>8s} {'rowhit pts':>10s}")
        for c in cross:
            print(f"{c['graph']:12s} {c['config']:10s} {c['window']:6d} {c['policy']:3s} "
                  f"{c['p0_cycles']:11,} {c['cycles']:11,} {c['d_cycles_pct']:+8.2f} "
                  f"{c['d_row_hit_pts']:+10.2f}")

    with open(os.path.join(OUT, "checks.txt"), "w") as f:
        f.write("# M23-G5B accounting and correctness (one line per cell)\n")
        f.write("# config policy W checks fields_failed fields_missing\n")
        for r in rows:
            f.write(f"{r['config']:10s} {r['policy']:4s} {r['window']:6d} {r['checks_ok']:4s} "
                    f"{r['checks_failed'] or '-'} {r['fields_missing'] or '-'}\n")
        f.write("\n# every logical element is either its own target request or a merged duplicate\n")
        for r in rows:
            f.write(f"{r['config']:10s} {r['policy']:4s} {r['window']:6d} "
                    f"{r['elems_done']} = {r['tgt_req_gen']} + {r['dedup_merged']}  "
                    f"(done {r['tgt_req_done']})\n")

    bad = [r for r in rows if r["checks_ok"] != "yes"]
    nofield = [r for r in rows if r["fields_missing"]]
    print(f"\n{len(rows)} cells, {len(attr)} attribution rows")
    print(f"cells failing an accounting check: {len(bad)}")
    for b in bad:
        print(f"  {b['config']} W={b['window']} {b['policy']}: {b['checks_failed']}")
    print(f"cells with a missing field: {len(nofield)}")
    for b in nofield[:5]:
        print(f"  {b['config']} W={b['window']} {b['policy']}: {b['fields_missing']}")
    return 1 if (bad or nofield) else 0


if __name__ == "__main__":
    sys.exit(main())
