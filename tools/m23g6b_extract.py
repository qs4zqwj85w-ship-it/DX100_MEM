#!/usr/bin/env python3
"""M23-G6B — extract the streaming translation/DRAM decoupling cells.

Cells: build/wm/m23g6b/g6b_<workload>_w<W>_<cfg>.txt
       cfg = p0 (bulk_org 0), p2 (2), p6 (G6A, 6), p7 (G6B, 7),
             ip2 (bulk_org 2, ideal_translation 1)
       plus build/wm/m23g6b/g6bc_<workload>_cap<C>.txt for the capacity sweep.

All cells run configs/m23g6b_base.cfg, so the arms differ only in `bulk_org`
(and `ideal_translation` for the reference arm). P0/P2 are the published
policies selected on the command line; the Orkut and LiveJournal anchors below
are the published M23-G3 / M23-G5B 16K cycles and are compared exactly.

Anything incomplete or failing a check is printed and counted; the exit status
is non-zero if any cell is missing or any check fails.
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g6b")
BANKS = 2
WL = ["orkut", "livejournal", "ogbntrain"]
CFG = {"p0": "P0", "p2": "P2", "p6": "G6A", "p7": "G6B", "ip2": "ideal-P2"}

ANCHORS = {
    ("orkut", "p0"): 16714538,
    ("orkut", "p2"): 23965633,
    ("livejournal", "p0"): 28309390,
    ("livejournal", "p2"): 32260832,
}

REQUIRED = [
    "cycles", "walks", "pte_fetches", "tlb0_misses", "tlb0_mshr_stalls",
    "tlb0_mshr_max", "tlb0_mshr_occ", "walker_busy", "l2tlb_misses",
    "inflight_max", "peak_total_outstanding",
    "dram_reads", "dram_row_hits", "dram_row_conflicts", "dram_busy_cycles",
    "dram_svc_events", "dram_starved_cycles", "dram_bytes",
    "l1_reads", "l1_hits", "l2_reads", "l2_hits", "l2_misses", "replays",
    "bulk_lines_issued", "bulk_tgt_req_gen", "bulk_tgt_req_done",
    "bulk_elems_total", "bulk_elems_done", "bulk_org_runs", "bulk_org_cycles",
    "bulk_sched_opens", "bulk_sched_switches", "bulk_sched_idle",
    "bulk_sched_cycles", "bulk_sched_scan", "bulk_slot_stall",
    "bulk_win_alloc", "bulk_win_drain_mean",
    "conservation_ok", "conservation_violations",
    "bulk_residency_violations", "bulk_addr_mismatch",
    "bulk_issue_monotonic_violations", "dispatch_partition_ok",
]

G6B = [
    "bulk_g6b_windows", "bulk_g6b_tr_requests", "bulk_g6b_tr_completed",
    "bulk_g6b_pa_capacity", "bulk_g6b_pa_bytes", "bulk_g6b_ready_peak",
    "bulk_g6b_ready_occ_sum", "bulk_g6b_backpressure_cycles",
    "bulk_g6b_tr_stall_cycles", "bulk_g6b_no_ready_cycles",
    "bulk_g6b_tr_active_cycles", "bulk_g6b_tgt_out_cycles",
    "bulk_g6b_overlap_cycles", "bulk_g6b_runs_released",
    "bulk_g6b_tr_wait_dram", "bulk_g6b_tr_wait_max",
]


def parse(path):
    kv = {}
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if "=" not in line or line.startswith("#"):
                continue
            k, _, v = line.partition("=")
            if not k.replace("_", "").isalnum():
                continue
            try:
                kv[k] = int(v)
            except ValueError:
                pass
    return kv


def pct(x, y):
    return 100.0 * x / y if y else 0.0


def load(name):
    path = os.path.join(OUT, name + ".txt")
    if not os.path.exists(path):
        return None
    kv = parse(path)
    return kv if "cycles" in kv else None


def main():
    cells, missing = {}, []
    for wl in WL:
        for cfg in CFG:
            name = "g6b_%s_w16384_%s" % (wl, cfg)
            kv = load(name)
            if kv is None:
                missing.append(name)
            else:
                cells[(wl, cfg)] = kv

    problems = ["missing cells: " + ", ".join(missing)] if missing else []

    for (wl, cfg), kv in sorted(cells.items()):
        tag = "%s/%s" % (wl, cfg)
        for k in REQUIRED + (G6B if cfg == "p7" else []):
            if k not in kv:
                problems.append("%s: missing key %s" % (tag, k))
        if kv.get("conservation_ok", 0) != 1:
            problems.append("%s: conservation_ok != 1" % tag)
        for k in ("conservation_violations", "bulk_residency_violations",
                  "bulk_addr_mismatch", "bulk_issue_monotonic_violations"):
            if kv.get(k, 0) != 0:
                problems.append("%s: %s = %d" % (tag, k, kv.get(k, 0)))
        if kv.get("dispatch_partition_ok", 1) != 1:
            problems.append("%s: dispatch_partition_ok != 1" % tag)
        if kv["bulk_tgt_req_gen"] != kv["bulk_tgt_req_done"]:
            problems.append("%s: targets generated %d != done %d"
                            % (tag, kv["bulk_tgt_req_gen"], kv["bulk_tgt_req_done"]))
        if kv["bulk_elems_done"] != kv["bulk_elems_total"]:
            problems.append("%s: elements retired %d of %d"
                            % (tag, kv["bulk_elems_done"], kv["bulk_elems_total"]))
        if kv["cycles"] >= 3000000000:
            problems.append("%s: hit max_cycles (DNF)" % tag)
        if cfg == "p7":
            if kv["bulk_g6b_tr_completed"] != kv["bulk_g6b_tr_requests"]:
                problems.append("%s: translations %d of %d"
                                % (tag, kv["bulk_g6b_tr_completed"],
                                   kv["bulk_g6b_tr_requests"]))
            if kv["bulk_g6b_tr_requests"] != kv["bulk_org_runs"]:
                problems.append("%s: %d probes for %d runs"
                                % (tag, kv["bulk_g6b_tr_requests"], kv["bulk_org_runs"]))
            if kv["bulk_g6b_runs_released"] != kv["bulk_lines_issued"]:
                problems.append("%s: %d released, %d lines issued"
                                % (tag, kv["bulk_g6b_runs_released"],
                                   kv["bulk_lines_issued"]))
            if kv["bulk_g6b_ready_peak"] > kv["bulk_g6b_pa_capacity"]:
                problems.append("%s: peak %d entries > capacity %d"
                                % (tag, kv["bulk_g6b_ready_peak"],
                                   kv["bulk_g6b_pa_capacity"]))
            # occupancy integral must equal the summed buffer residence time
            if kv["bulk_g6b_ready_occ_sum"] != kv["bulk_g6b_tr_wait_dram"]:
                problems.append("%s: occupancy integral %d != summed wait %d"
                                % (tag, kv["bulk_g6b_ready_occ_sum"],
                                   kv["bulk_g6b_tr_wait_dram"]))
            for k in ("bulk_g6b_overlap_cycles", "bulk_g6b_tgt_out_cycles"):
                if kv[k] > kv["cycles"]:
                    problems.append("%s: %s exceeds the run" % (tag, k))
            if kv["bulk_g6b_overlap_cycles"] > kv["bulk_g6b_tgt_out_cycles"]:
                problems.append("%s: overlap above target-outstanding time" % tag)

    anchors = []
    for (wl, cfg), want in sorted(ANCHORS.items()):
        kv = cells.get((wl, cfg))
        if not kv:
            continue
        ok = kv["cycles"] == want
        anchors.append((wl, cfg, want, kv["cycles"], ok))
        if not ok:
            problems.append("%s/%s: published %d, measured %d (%+.2f%%)"
                            % (wl, cfg, want, kv["cycles"],
                               pct(kv["cycles"] - want, want)))

    def row(wl, cfg):
        kv = cells[(wl, cfg)]
        c = kv["cycles"]
        p0 = cells[(wl, "p0")]["cycles"]
        hits, conf = kv["dram_row_hits"], kv["dram_row_conflicts"]
        out = {
            "workload": wl, "cfg": CFG[cfg], "cycles": c,
            "vs_p0_pct": pct(c - p0, p0), "speedup_vs_p0": p0 / c,
            "walks": kv["walks"], "pte": kv["pte_fetches"],
            "tlb0_miss": kv["tlb0_misses"], "l2tlb_miss": kv["l2tlb_misses"],
            "tlb0_mshr_max": kv["tlb0_mshr_max"],
            "row_hit_pct": pct(hits, hits + conf) if (hits + conf) else 0.0,
            "conflicts": conf,
            "bank_util_pct": pct(kv["dram_busy_cycles"], BANKS * c),
            "bytes_per_cycle": kv["dram_bytes"] / c if c else 0.0,
            "dram_reads": kv["dram_reads"],
            "starved_pct": pct(kv["dram_starved_cycles"], c),
            "cohort_opens": kv["bulk_sched_opens"],
            "sched_idle": kv["bulk_sched_idle"],
            "slot_stall": kv["bulk_slot_stall"],
            "inflight_max": kv["inflight_max"],
            "lines": kv["bulk_lines_issued"],
        }
        if cfg == "p7":
            out.update({
                "buf_cap": kv["bulk_g6b_pa_capacity"],
                "buf_peak": kv["bulk_g6b_ready_peak"],
                "buf_avg": kv["bulk_g6b_ready_occ_sum"] / c if c else 0.0,
                "bp_pct": pct(kv["bulk_g6b_backpressure_cycles"], c),
                "no_ready_pct": pct(kv["bulk_g6b_no_ready_cycles"], c),
                "tr_active_pct": pct(kv["bulk_g6b_tr_active_cycles"], c),
                "tgt_out_pct": pct(kv["bulk_g6b_tgt_out_cycles"], c),
                "overlap_pct_of_run": pct(kv["bulk_g6b_overlap_cycles"], c),
                "overlap_pct_of_tgt": pct(kv["bulk_g6b_overlap_cycles"],
                                          kv["bulk_g6b_tgt_out_cycles"]),
                "wait_mean": (kv["bulk_g6b_tr_wait_dram"]
                              // kv["bulk_g6b_runs_released"])
                             if kv["bulk_g6b_runs_released"] else 0,
                "walker_util": pct(kv["walker_busy"], BANKS * c),
            })
        return out

    rows = [row(wl, cfg) for wl in WL if (wl, "p0") in cells for cfg in CFG
            if (wl, cfg) in cells]
    if rows:
        cols = []
        for r in rows:                       # union, first-seen order
            for k in r:
                if k not in cols:
                    cols.append(k)
        os.makedirs(OUT, exist_ok=True)
        with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=cols)
            w.writeheader()
            for r in rows:
                w.writerow({k: (round(v, 4) if isinstance(v, float) else v)
                            for k, v in r.items()})

    print("%-12s %-9s %12s %9s %8s %9s %8s %8s %9s %8s %8s %7s"
          % ("workload", "cfg", "cycles", "vs P0", "speedup", "walks", "rowhit%",
             "bankut%", "B/cycle", "conf", "starved%", "opens"))
    for r in rows:
        print("%-12s %-9s %12d %+8.2f%% %8.3f %9d %8.2f %8.2f %9.3f %8d %7.2f%% %7d"
              % (r["workload"], r["cfg"], r["cycles"], r["vs_p0_pct"],
                 r["speedup_vs_p0"], r["walks"], r["row_hit_pct"],
                 r["bank_util_pct"], r["bytes_per_cycle"], r["conflicts"],
                 r["starved_pct"], r["cohort_opens"]))

    print()
    print("%-12s %8s %8s %8s %8s %8s %9s %9s %9s %8s"
          % ("workload", "bufcap", "bufpk", "bufavg", "bp%", "noRdy%",
             "trAct%", "tgtOut%", "ovl/tgt", "walkut%"))
    for r in rows:
        if r["cfg"] != "G6B":
            continue
        print("%-12s %8d %8d %8.1f %7.2f%% %7.2f%% %8.1f%% %8.1f%% %8.1f%% %7.1f%%"
              % (r["workload"], r["buf_cap"], r["buf_peak"], r["buf_avg"],
                 r["bp_pct"], r["no_ready_pct"], r["tr_active_pct"],
                 r["tgt_out_pct"], r["overlap_pct_of_tgt"], r["walker_util"]))

    # capacity sweep, when present
    caps = []
    for wl in WL:
        for c in (64, 256, 1024, 4096, 16384):
            kv = load("g6bc_%s_cap%d" % (wl, c))
            if kv is None:
                continue
            caps.append((wl, c, kv["cycles"],
                         pct(kv["cycles"] - cells[(wl, "p0")]["cycles"],
                             cells[(wl, "p0")]["cycles"]),
                         pct(kv["dram_row_hits"],
                             kv["dram_row_hits"] + kv["dram_row_conflicts"]),
                         kv["bulk_g6b_ready_peak"],
                         pct(kv["bulk_g6b_backpressure_cycles"], kv["cycles"])))
            if kv.get("conservation_ok", 0) != 1:
                problems.append("%s cap%d: conservation_ok != 1" % (wl, c))
    if caps:
        print()
        print("%-12s %8s %12s %9s %8s %8s %8s"
              % ("workload", "capacity", "cycles", "vs P0", "rowhit%", "peak", "bp%"))
        for wl, c, cyc, d, rh, pk, bp in caps:
            print("%-12s %8d %12d %+8.2f%% %8.2f %8d %7.2f%%" % (wl, c, cyc, d, rh, pk, bp))

    print()
    for wl, cfg, want, got, ok in anchors:
        print("anchor %-12s %-3s %12d %12d %s" % (wl, cfg, want, got,
                                                  "OK" if ok else "MISMATCH"))
    print()
    if problems:
        print("%d problem(s):" % len(problems))
        for p in problems:
            print("  " + p)
        return 1
    print("all cells complete; all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
