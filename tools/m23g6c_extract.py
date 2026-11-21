#!/usr/bin/env python3
"""M23-G6C — extract the P0-admission / G6B-streaming cells.

Cells: build/wm/m23g6c/g6c_<workload>_w<W>_<cfg>.txt
       cfg = m0 (SIMT, dispatch 0), p0 (bulk_org 0), p2 (2), p7 (G6B, 7), p8 (G6C, 8)

Every cell runs the frozen configs/m23g6b_base.cfg, so the arms differ only in
`dispatch` and `bulk_org`. P0, P2 and G6B are published arms: their cycles are
compared exactly against the published anchors below, and the extractor fails if
one drifts. Anything incomplete or failing a check is printed and counted; the
exit status is non-zero if any cell is missing or any check fails.
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g6c")
BANKS = 2
WL = ["orkut", "livejournal", "ogbntrain"]
CFG = {"m0": "M0", "p0": "P0", "p2": "P2", "p7": "G6B", "p8": "G6C"}

# Published cycles for the unchanged arms: M23-G3 / M23-G5B (P0, P2, 16K) and the
# M23-G6B matrix itself (G6B, 16K).
ANCHORS = {
    ("orkut", "p0"): 16714538,
    ("orkut", "p2"): 23965633,
    ("livejournal", "p0"): 28309390,
    ("livejournal", "p2"): 32260832,
    ("orkut", "p7"): 25938106,
    ("livejournal", "p7"): 39200163,
    ("ogbntrain", "p7"): 375172893,
}

G6C = [
    "bulk_g6c_windows", "bulk_g6c_admit_attempts", "bulk_g6c_admit_accepted",
    "bulk_g6c_refused", "bulk_g6c_refuse_parked", "bulk_g6c_refuse_full",
    "bulk_g6c_refuse_other", "bulk_g6c_defer_events",
    "bulk_g6c_deferred_readmitted", "bulk_g6c_defer_then_other_admit",
    "bulk_g6c_defer_age_sum", "bulk_g6c_defer_age_max",
    "bulk_g6c_defer_depth_sum", "bulk_g6c_defer_depth_n",
    "bulk_g6c_defer_depth_max", "bulk_g6c_cohorts_opened",
    "bulk_g6c_admit_idle_cycles", "bulk_g6c_dup_submit_blocked",
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


def div(x, y):
    return x / y if y else 0.0


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
            name = "g6c_%s_w16384_%s" % (wl, cfg)
            kv = load(name)
            if kv is None:
                missing.append(name)
            else:
                cells[(wl, cfg)] = kv

    problems = ["missing cells: " + ", ".join(missing)] if missing else []

    for (wl, cfg), kv in sorted(cells.items()):
        tag = "%s/%s" % (wl, cfg)
        # ---- every arm: run finished, conserved, same environment ----
        if kv.get("conservation_ok", 0) != 1:
            problems.append("%s: conservation_ok != 1" % tag)
        if kv.get("conservation_violations", 0) != 0:
            problems.append("%s: conservation_violations = %d"
                            % (tag, kv.get("conservation_violations", 0)))
        if kv["cycles"] >= 3000000000:
            problems.append("%s: hit max_cycles (DNF)" % tag)
        for k in ("walks", "pte_fetches", "tlb0_misses", "tlb0_mshr_occ",
                  "walker_busy", "dram_reads", "dram_row_hits",
                  "dram_row_conflicts", "dram_svc_events", "dram_bytes"):
            if k not in kv:
                problems.append("%s: missing key %s" % (tag, k))
        if cfg == "m0":
            # The SIMT path generates requests from the same trace and must
            # retire every one it generated; it has no engine and no bulk_* keys.
            if kv.get("completed", -1) != kv.get("requests_generated", -2):
                problems.append("%s: completed %s of %s generated"
                                % (tag, kv.get("completed"),
                                   kv.get("requests_generated")))
            continue
        # ---- bulk arms: request/element conservation, no mislabelled state ----
        for k in ("bulk_tgt_req_gen", "bulk_tgt_req_done", "bulk_elems_done",
                  "bulk_elems_total", "bulk_elems", "bulk_lines_issued",
                  "bulk_org_runs", "bulk_sched_opens", "bulk_sched_bytes"):
            if k not in kv:
                problems.append("%s: missing key %s" % (tag, k))
        if kv.get("bulk_tgt_req_gen") != kv.get("bulk_tgt_req_done"):
            problems.append("%s: targets generated %d != done %d"
                            % (tag, kv.get("bulk_tgt_req_gen", 0),
                               kv.get("bulk_tgt_req_done", 0)))
        if kv.get("bulk_elems_done") != kv.get("bulk_elems_total"):
            problems.append("%s: elements retired %d of %d"
                            % (tag, kv.get("bulk_elems_done", 0),
                               kv.get("bulk_elems_total", 0)))
        if kv.get("bulk_elems") != kv.get("bulk_elems_done"):
            problems.append("%s: bulk_elems %d != done %d"
                            % (tag, kv.get("bulk_elems", 0),
                               kv.get("bulk_elems_done", 0)))
        if kv.get("bulk_lines_issued") != kv.get("bulk_tgt_req_gen"):
            problems.append("%s: %d lines issued, %d targets generated"
                            % (tag, kv.get("bulk_lines_issued", 0),
                               kv.get("bulk_tgt_req_gen", 0)))
        for k in ("bulk_addr_mismatch", "bulk_residency_violations",
                  "bulk_issue_monotonic_violations"):
            if kv.get(k, 0) != 0:
                problems.append("%s: %s = %d" % (tag, k, kv.get(k, 0)))
        if kv.get("dispatch_partition_ok", 1) != 1:
            problems.append("%s: dispatch_partition_ok != 1" % tag)
        if kv.get("bulk_win_entries", 0) != kv.get("bulk_elems", -1):
            problems.append("%s: window entries %d != elements %d"
                            % (tag, kv.get("bulk_win_entries", 0),
                               kv.get("bulk_elems", 0)))

        if cfg in ("p7", "p8"):
            for k in ("bulk_g6b_tr_requests", "bulk_g6b_tr_completed",
                      "bulk_g6b_runs_released", "bulk_g6b_ready_peak",
                      "bulk_g6b_ready_occ_sum", "bulk_g6b_tr_wait_dram",
                      "bulk_g6b_overlap_cycles", "bulk_g6b_tgt_out_cycles",
                      "bulk_g6b_pa_capacity"):
                if k not in kv:
                    problems.append("%s: missing key %s" % (tag, k))
            if kv.get("bulk_g6b_tr_completed") != kv.get("bulk_g6b_tr_requests"):
                problems.append("%s: translations %d of %d"
                                % (tag, kv.get("bulk_g6b_tr_completed", 0),
                                   kv.get("bulk_g6b_tr_requests", 0)))
            if kv.get("bulk_g6b_tr_requests") != kv.get("bulk_org_runs"):
                problems.append("%s: %d probes for %d runs"
                                % (tag, kv.get("bulk_g6b_tr_requests", 0),
                                   kv.get("bulk_org_runs", 0)))
            if kv.get("bulk_g6b_runs_released") != kv.get("bulk_lines_issued"):
                problems.append("%s: %d released, %d lines issued"
                                % (tag, kv.get("bulk_g6b_runs_released", 0),
                                   kv.get("bulk_lines_issued", 0)))
            if kv.get("bulk_g6b_ready_peak", 0) > kv.get("bulk_g6b_pa_capacity", 0):
                problems.append("%s: peak %d entries > capacity %d"
                                % (tag, kv.get("bulk_g6b_ready_peak", 0),
                                   kv.get("bulk_g6b_pa_capacity", 0)))
            # occupancy integral == summed buffer residence time, exactly
            if kv.get("bulk_g6b_ready_occ_sum") != kv.get("bulk_g6b_tr_wait_dram"):
                problems.append("%s: occupancy integral %d != summed wait %d"
                                % (tag, kv.get("bulk_g6b_ready_occ_sum", 0),
                                   kv.get("bulk_g6b_tr_wait_dram", 0)))
            if kv.get("bulk_g6b_overlap_cycles", 0) > kv.get("bulk_g6b_tgt_out_cycles", 0):
                problems.append("%s: overlap above target-outstanding time" % tag)
            for k in ("bulk_g6b_overlap_cycles", "bulk_g6b_tgt_out_cycles",
                      "bulk_g6b_tr_active_cycles"):
                if kv.get(k, 0) > kv["cycles"]:
                    problems.append("%s: %s exceeds the run" % (tag, k))
        if cfg == "p8":
            for k in G6C:
                if k not in kv:
                    problems.append("%s: missing key %s" % (tag, k))
            if kv.get("bulk_g6c_admit_attempts") != (kv.get("bulk_g6c_admit_accepted", 0)
                                                     + kv.get("bulk_g6c_refused", 0)):
                problems.append("%s: attempts %d != accepted %d + refused %d"
                                % (tag, kv.get("bulk_g6c_admit_attempts", 0),
                                   kv.get("bulk_g6c_admit_accepted", 0),
                                   kv.get("bulk_g6c_refused", 0)))
            if kv.get("bulk_g6c_defer_events") != kv.get("bulk_g6c_refused"):
                problems.append("%s: defer events %d != refusals %d"
                                % (tag, kv.get("bulk_g6c_defer_events", 0),
                                   kv.get("bulk_g6c_refused", 0)))
            if kv.get("bulk_g6c_dup_submit_blocked", -1) != 0:
                problems.append("%s: duplicate submission blocked %d times"
                                % (tag, kv.get("bulk_g6c_dup_submit_blocked", 0)))
            if kv.get("bulk_g6c_refuse_other", -1) != 0:
                problems.append("%s: unclassified refusals %d"
                                % (tag, kv.get("bulk_g6c_refuse_other", 0)))
            if (kv.get("bulk_g6c_refuse_parked", 0) + kv.get("bulk_g6c_refuse_full", 0)
                    != kv.get("bulk_g6c_refused", 0)):
                problems.append("%s: refusal causes do not sum to refusals" % tag)
            if kv.get("bulk_g6c_deferred_readmitted", 0) > kv.get("bulk_g6c_defer_events", 0):
                problems.append("%s: re-admissions above deferral events" % tag)
            if kv.get("bulk_g6c_defer_depth_max", 0) > kv.get("bulk_sched_cohorts", 0):
                problems.append("%s: deferral depth %d above %d cohorts"
                                % (tag, kv.get("bulk_g6c_defer_depth_max", 0),
                                   kv.get("bulk_sched_cohorts", 0)))
            if kv.get("bulk_g6c_admit_accepted") != kv.get("bulk_g6b_tr_requests"):
                problems.append("%s: accepted %d != probes %d"
                                % (tag, kv.get("bulk_g6c_admit_accepted", 0),
                                   kv.get("bulk_g6b_tr_requests", 0)))
            if kv.get("bulk_g6c_windows", 0) != kv.get("bulk_g6b_windows", -1):
                problems.append("%s: %d G6C windows, %d G6B windows"
                                % (tag, kv.get("bulk_g6c_windows", 0),
                                   kv.get("bulk_g6b_windows", 0)))

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
        m0 = cells[(wl, "m0")]["cycles"] if (wl, "m0") in cells else 0
        p0 = cells[(wl, "p0")]["cycles"] if (wl, "p0") in cells else 0
        p2 = cells[(wl, "p2")]["cycles"] if (wl, "p2") in cells else 0
        g6b = cells[(wl, "p7")]["cycles"] if (wl, "p7") in cells else 0
        hits, conf = kv.get("dram_row_hits", 0), kv.get("dram_row_conflicts", 0)
        rel = kv.get("bulk_g6b_runs_released", 0)
        out = {
            "workload": wl, "cfg": CFG[cfg], "cycles": c,
            "speedup_vs_m0": div(m0, c),
            "vs_m0_pct": pct(c - m0, m0), "vs_p0_pct": pct(c - p0, p0),
            "vs_p2_pct": pct(c - p2, p2), "vs_g6b_pct": pct(c - g6b, g6b),
            # translation
            "walks": kv.get("walks", 0), "pte": kv.get("pte_fetches", 0),
            "tlb0_miss": kv.get("tlb0_misses", 0),
            "tlb0_reads": kv.get("tlb0_reads", 0),
            "dtlb_mshr_avg": div(kv.get("tlb0_mshr_occ", 0), c),
            "dtlb_mshr_max": kv.get("tlb0_mshr_max", 0),
            "tlb_mshr_stalls": kv.get("tlb0_mshr_stalls", 0),
            "refuse_parked": kv.get("tlb_refuse_parked", 0),
            "refuse_full": kv.get("tlb_refuse_full", 0),
            "l2tlb_miss": kv.get("l2tlb_misses", 0),
            "l2tlb_max_occ": kv.get("l2tlb_max_occ", 0),
            "l2tlb_occ_avg": div(kv.get("l2tlb_occ", 0), c),
            "walker_util_pct": pct(kv.get("walker_busy", 0), BANKS * c),
            "walker_max": kv.get("walker_max", 0),
            "walk_wc_hits": kv.get("walk_wc_hits", 0),
            # admission (G6C; P0 reports its own sched_* family)
            "admit_attempts": kv.get("bulk_g6c_admit_attempts", 0),
            "admit_ok": kv.get("bulk_g6c_admit_accepted", 0),
            "admit_refused": kv.get("bulk_g6c_refused", 0),
            "admit_ref_parked": kv.get("bulk_g6c_refuse_parked", 0),
            "admit_ref_full": kv.get("bulk_g6c_refuse_full", 0),
            "defer_events": kv.get("bulk_g6c_defer_events", 0),
            "defer_readmit": kv.get("bulk_g6c_deferred_readmitted", 0),
            "defer_other_admit": kv.get("bulk_g6c_defer_then_other_admit", 0),
            "defer_age_mean": (div(kv.get("bulk_g6c_defer_age_sum", 0),
                                   kv.get("bulk_g6c_deferred_readmitted", 0))),
            "defer_age_max": kv.get("bulk_g6c_defer_age_max", 0),
            "defer_depth_mean": div(kv.get("bulk_g6c_defer_depth_sum", 0),
                                    kv.get("bulk_g6c_defer_depth_n", 0)),
            "defer_depth_max": kv.get("bulk_g6c_defer_depth_max", 0),
            "cohorts_opened": kv.get("bulk_g6c_cohorts_opened", 0),
            "admit_idle_pct": pct(kv.get("bulk_g6c_admit_idle_cycles", 0), c),
            "tr_stall_pct": pct(kv.get("bulk_g6b_tr_stall_cycles", 0), c),
            "p0_sched_defer": kv.get("bulk_sched_defer", 0),
            # ready buffer
            "buf_cap": kv.get("bulk_g6b_pa_capacity", 0),
            "buf_peak": kv.get("bulk_g6b_ready_peak", 0),
            "buf_integral": kv.get("bulk_g6b_ready_occ_sum", 0),
            "buf_avg": div(kv.get("bulk_g6b_ready_occ_sum", 0), c),
            "residence_mean": div(kv.get("bulk_g6b_tr_wait_dram", 0), rel),
            "residence_max": kv.get("bulk_g6b_tr_wait_max", 0),
            "bp_pct": pct(kv.get("bulk_g6b_backpressure_cycles", 0), c),
            "no_ready_pct": pct(kv.get("bulk_g6b_no_ready_cycles", 0), c),
            "tr_active_pct": pct(kv.get("bulk_g6b_tr_active_cycles", 0), c),
            "tgt_out_pct": pct(kv.get("bulk_g6b_tgt_out_cycles", 0), c),
            "overlap_pct_run": pct(kv.get("bulk_g6b_overlap_cycles", 0), c),
            "overlap_pct_tgt": pct(kv.get("bulk_g6b_overlap_cycles", 0),
                                   kv.get("bulk_g6b_tgt_out_cycles", 0)),
            # DRAM
            "row_hits": hits, "conflicts": conf,
            "row_hit_pct": pct(hits, hits + conf) if (hits + conf) else 0.0,
            "rows_used": kv.get("dram_rows_used", 0),
            "cell_changes": kv.get("bulk_sched_opens", 0),
            "cell_changes_per_release": div(kv.get("bulk_sched_opens", 0), rel),
            "dram_reads": kv.get("dram_reads", 0),
            "dram_svc_events": kv.get("dram_svc_events", 0),
            "dram_bytes": kv.get("dram_bytes", 0),
            "bank_util_pct": pct(kv.get("dram_busy_cycles", 0), BANKS * c),
            "bytes_per_cycle": div(kv.get("dram_bytes", 0), c),
            "starved_pct": pct(kv.get("dram_starved_cycles", 0), c),
            "wait_mean": div(kv.get("dram_wait_sum", 0), kv.get("dram_svc_events", 0)),
            "svc_mean": div(kv.get("dram_svc_hit_cycles", 0)
                            + kv.get("dram_svc_miss_cycles", 0),
                            kv.get("dram_svc_events", 0)),
            "lat_mean": div(kv.get("lat_sum", 0), kv.get("completed", 0)),
        }
        return out

    rows = [row(wl, cfg) for wl in WL if (wl, "m0") in cells for cfg in CFG
            if (wl, cfg) in cells]

    if rows:
        os.makedirs(OUT, exist_ok=True)
        cols = []
        for r in rows:
            for k in r:
                if k not in cols:
                    cols.append(k)
        with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=cols)
            w.writeheader()
            for r in rows:
                w.writerow({k: (round(v, 4) if isinstance(v, float) else v)
                            for k, v in r.items()})

    print("== overall (cycles; speedup vs M0; relative to the other arms)")
    print("%-12s %-5s %13s %8s %9s %9s %9s"
          % ("workload", "cfg", "cycles", "xM0", "vs P0", "vs P2", "vs G6B"))
    for r in rows:
        print("%-12s %-5s %13d %8.3f %+8.2f%% %+8.2f%% %+8.2f%%"
              % (r["workload"], r["cfg"], r["cycles"], r["speedup_vs_m0"],
                 r["vs_p0_pct"], r["vs_p2_pct"], r["vs_g6b_pct"]))

    print("\n== translation")
    print("%-12s %-5s %8s %8s %7s %7s %7s %7s %7s %8s %8s"
          % ("workload", "cfg", "walks", "tlb0ms", "dMSHRav", "dMSHRmx",
             "walkut%", "walkmx", "l2occ", "tlbmshl", "refuse(p/f)"))
    for r in rows:
        print("%-12s %-5s %8d %8d %7.2f %7d %7.1f %7d %7.2f %8d %d/%d"
              % (r["workload"], r["cfg"], r["walks"], r["tlb0_miss"],
                 r["dtlb_mshr_avg"], r["dtlb_mshr_max"], r["walker_util_pct"],
                 r["walker_max"], r["l2tlb_occ_avg"], r["tlb_mshr_stalls"],
                 r["refuse_parked"], r["refuse_full"]))

    print("\n== admission (G6C) and the G6B stall it replaces")
    print("%-12s %10s %9s %9s %9s %9s %9s %7s %8s %7s"
          % ("workload", "attempts", "accepted", "refused", "pk/full",
             "defer", "readmit", "ageMx", "depthMx", "idle%"))
    for r in rows:
        if r["cfg"] != "G6C":
            continue
        print("%-12s %10d %9d %9d %4d/%-4d %9d %9d %7d %8.2f %6.2f%%"
              % (r["workload"], r["admit_attempts"], r["admit_ok"],
                 r["admit_refused"], r["admit_ref_parked"], r["admit_ref_full"],
                 r["defer_events"], r["defer_readmit"], r["defer_age_max"],
                 r["defer_depth_max"], r["admit_idle_pct"]))

    print("\n== ready buffer")
    print("%-12s %-5s %7s %7s %8s %9s %9s %7s %8s %9s"
          % ("workload", "cfg", "cap", "peak", "avg", "resid", "residmax",
             "bp%", "noRdy%", "ovl/tgt%"))
    for r in rows:
        if r["cfg"] not in ("G6B", "G6C"):
            continue
        print("%-12s %-5s %7d %7d %8.1f %9.1f %9d %6.2f%% %7.2f%% %8.2f%%"
              % (r["workload"], r["cfg"], r["buf_cap"], r["buf_peak"],
                 r["buf_avg"], r["residence_mean"], r["residence_max"],
                 r["bp_pct"], r["no_ready_pct"], r["overlap_pct_tgt"]))

    print("\n== DRAM")
    print("%-12s %-5s %10s %8s %8s %8s %8s %8s %8s %8s"
          % ("workload", "cfg", "reads", "rowhit%", "conf", "rows", "cells",
             "bankut%", "B/cyc", "starved%"))
    for r in rows:
        print("%-12s %-5s %10d %8.2f %8d %8d %8d %8.2f %8.3f %7.2f%%"
              % (r["workload"], r["cfg"], r["dram_reads"], r["row_hit_pct"],
                 r["conflicts"], r["rows_used"], r["cell_changes"],
                 r["bank_util_pct"], r["bytes_per_cycle"], r["starved_pct"]))

    print()
    for wl, cfg, want, got, ok in anchors:
        print("anchor %-12s %-3s %12d %12d %s"
              % (wl, cfg, want, got, "OK" if ok else "MISMATCH"))
    print()
    if problems:
        print("%d problem(s):" % len(problems))
        for p in problems:
            print("  " + p)
        return 1
    print("all %d cells complete; all checks passed" % len(cells))
    return 0


if __name__ == "__main__":
    sys.exit(main())
