#!/usr/bin/env python3
"""M23-G6A — extract the translation-first, DRAM-second cells.

Cells: build/wm/m23g6a/g6a_<workload>_w<W>_<cfg>.txt
       cfg = p0 (bulk_org 0), p2 (bulk_org 2), p6 (bulk_org 6), ip2 (bulk_org 2,
       ideal_translation 1)

Every cell is one dx100sim run on configs/m23g6a_base.cfg, so the four
configurations differ only in `bulk_org` (and `ideal_translation` for the
reference arm). P0 and P2 are the published policies selected on the command
line; the Orkut and LiveJournal anchors below are the published M23-G3 / M23-G5B
16K cycles and are compared exactly, so "preserved" is verified, not asserted.

Anything that fails a check is printed and counted; the exit status is non-zero
if any cell is incomplete or any check fails.
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g6a")
BANKS = 2
WL = ["orkut", "livejournal", "ogbntrain"]
CFG = {"p0": "P0", "p2": "P2", "p6": "G6A", "ip2": "ideal-P2"}

# published 16K, real translation (M23-G3 §3 for Orkut, M23-G5B §4 for LiveJournal)
ANCHORS = {
    ("orkut", "p0"): 16714538,
    ("orkut", "p2"): 23965633,
    ("livejournal", "p0"): 28309390,
    ("livejournal", "p2"): 32260832,
}

REQUIRED = [
    "cycles", "walks", "pte_fetches", "tlb0_misses", "tlb0_mshr_stalls",
    "l2tlb_misses", "inflight_max", "peak_total_outstanding",
    "dram_reads", "dram_row_hits", "dram_row_conflicts", "dram_busy_cycles",
    "dram_svc_events", "dram_starved_cycles", "dram_bytes", "dram_max_q",
    "l1_reads", "l1_hits", "l2_reads", "l2_hits", "l2_misses", "replays",
    "bulk_lines_issued", "bulk_tgt_req_gen", "bulk_tgt_req_done",
    "bulk_elems_total", "bulk_elems_done", "bulk_org_runs", "bulk_org_cycles",
    "bulk_sched_opens", "bulk_sched_idle", "bulk_sched_defer",
    "bulk_dedup_merged", "bulk_win_sealed", "bulk_batches",
    "conservation_ok", "conservation_violations",
    "bulk_residency_violations", "bulk_addr_mismatch",
    "bulk_issue_monotonic_violations", "dispatch_partition_ok",
]

G6A = [
    "bulk_g6a_windows", "bulk_g6a_tr_requests", "bulk_g6a_tr_completed",
    "bulk_g6a_tr_cycles", "bulk_g6a_dr_cycles", "bulk_g6a_tr_stall_cycles",
    "bulk_g6a_barrier_cycles", "bulk_g6a_pa_capacity", "bulk_g6a_pa_need_max",
    "bulk_g6a_pa_occ_max", "bulk_g6a_pa_bytes", "bulk_g6a_runs_released",
    "bulk_g6a_tr_wait_dram", "bulk_g6a_tr_wait_max",
    "bulk_g6a_dram_idle_bank_cycles_tr",
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


def main():
    cells = {}
    missing = []
    for wl in WL:
        for cfg in CFG:
            name = "g6a_%s_w16384_%s" % (wl, cfg)
            path = os.path.join(OUT, name + ".txt")
            if not os.path.exists(path):
                missing.append(name)
                continue
            kv = parse(path)
            if "cycles" not in kv:      # still running, or the run died
                missing.append(name)
                continue
            cells[(wl, cfg)] = kv

    problems = []
    if missing:
        problems.append("missing cells: " + ", ".join(missing))

    # ---- per-cell checks -----------------------------------------------------
    for (wl, cfg), kv in sorted(cells.items()):
        tag = "%s/%s" % (wl, cfg)
        for k in REQUIRED + (G6A if cfg == "p6" else []):
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
        if kv["bulk_tgt_req_issued"] != kv["bulk_lines_issued"]:
            problems.append("%s: issued %d != lines %d"
                            % (tag, kv["bulk_tgt_req_issued"], kv["bulk_lines_issued"]))
        if kv["bulk_elems_done"] != kv["bulk_elems_total"]:
            problems.append("%s: elements retired %d of %d"
                            % (tag, kv["bulk_elems_done"], kv["bulk_elems_total"]))
        if cfg == "p6":
            if kv["bulk_g6a_tr_completed"] != kv["bulk_g6a_tr_requests"]:
                problems.append("%s: translations %d completed of %d"
                                % (tag, kv["bulk_g6a_tr_completed"],
                                   kv["bulk_g6a_tr_requests"]))
            if kv["bulk_g6a_tr_requests"] != kv["bulk_org_runs"]:
                problems.append("%s: %d probes for %d runs"
                                % (tag, kv["bulk_g6a_tr_requests"], kv["bulk_org_runs"]))
            if kv["bulk_g6a_runs_released"] != kv["bulk_lines_issued"]:
                problems.append("%s: %d runs released, %d lines issued"
                                % (tag, kv["bulk_g6a_runs_released"],
                                   kv["bulk_lines_issued"]))
            if kv["bulk_g6a_pa_need_max"] > kv["bulk_g6a_pa_capacity"]:
                problems.append("%s: needed %d PA entries, capacity %d"
                                % (tag, kv["bulk_g6a_pa_need_max"],
                                   kv["bulk_g6a_pa_capacity"]))
            if kv["bulk_g6a_tr_cycles"] + kv["bulk_g6a_dr_cycles"] > kv["cycles"]:
                problems.append("%s: phase cycles exceed the run" % tag)
            if kv["cycles"] >= 3000000000:
                problems.append("%s: hit max_cycles (DNF)" % tag)

    # ---- anchors: P0/P2 must be the published cycles -------------------------
    anchors = []
    for (wl, cfg), want in sorted(ANCHORS.items()):
        kv = cells.get((wl, cfg))
        if not kv:
            continue
        got = kv["cycles"]
        ok = got == want
        anchors.append((wl, cfg, want, got, ok))
        if not ok:
            problems.append("%s/%s: published %d, measured %d (%+.2f%%)"
                            % (wl, cfg, want, got, pct(got - want, want)))

    # ---- table ---------------------------------------------------------------
    def row(wl, cfg):
        kv = cells[(wl, cfg)]
        c = kv["cycles"]
        p0 = cells[(wl, "p0")]["cycles"]
        hits = kv["dram_row_hits"]
        conf = kv["dram_row_conflicts"]
        return {
            "workload": wl, "cfg": CFG[cfg], "cycles": c,
            "vs_p0": pct(c - p0, p0),
            "speedup": p0 / c if c else 0.0,
            "walks": kv["walks"], "pte": kv["pte_fetches"],
            "tlb0_miss": kv["tlb0_misses"],
            "tlb0_stall": kv["tlb0_mshr_stalls"],
            "l2tlb_miss": kv["l2tlb_misses"],
            "row_hit_pct": pct(hits, hits + conf) if (hits + conf) else 0.0,
            "conflicts": conf,
            "bank_util_pct": pct(kv["dram_busy_cycles"], BANKS * c),
            "bytes_per_cycle": kv["dram_bytes"] / c if c else 0.0,
            "dram_reads": kv["dram_reads"],
            "starved_pct": pct(kv["dram_starved_cycles"], c),
            "inflight_max": kv["inflight_max"],
            "residency_max": kv["peak_total_outstanding"],
            "lines": kv["bulk_lines_issued"],
        }

    rows = [row(wl, cfg) for wl in WL if (wl, "p0") in cells for cfg in CFG
            if (wl, cfg) in cells]

    cols = list(rows[0].keys()) if rows else []
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow({k: (round(v, 4) if isinstance(v, float) else v)
                        for k, v in r.items()})

    print("%-12s %-9s %12s %9s %8s %9s %8s %8s %9s %8s %7s %8s"
          % ("workload", "cfg", "cycles", "vs P0", "speedup", "walks",
             "rowhit%", "bankut%", "B/cycle", "conf", "tlb0m", "l2tlb"))
    for r in rows:
        print("%-12s %-9s %12d %+8.2f%% %8.3f %9d %8.2f %8.2f %9.3f %8d %7d %8d"
              % (r["workload"], r["cfg"], r["cycles"], r["vs_p0"], r["speedup"],
                 r["walks"], r["row_hit_pct"], r["bank_util_pct"],
                 r["bytes_per_cycle"], r["conflicts"], r["tlb0_miss"],
                 r["l2tlb_miss"]))

    print()
    print("%-12s %11s %9s %9s %10s %9s %9s %9s %10s"
          % ("workload", "G6A tr cyc", "phase1%", "phase2%", "dTLB stall%",
             "DRAM idle", "PA peak", "PA need", "wait DRAM"))
    for wl in WL:
        kv = cells.get((wl, "p6"))
        if not kv:
            continue
        c = kv["cycles"]
        trc = kv["bulk_g6a_tr_cycles"]
        idle = kv["bulk_g6a_dram_idle_bank_cycles_tr"]
        print("%-12s %11d %8.2f%% %8.2f%% %9.2f%% %8.2f%% %9d %9d %10d"
              % (wl, trc, pct(trc, c), pct(kv["bulk_g6a_dr_cycles"], c),
                 pct(kv["bulk_g6a_tr_stall_cycles"], trc if trc else 1),
                 pct(idle, BANKS * trc if trc else 1),
                 kv["bulk_g6a_pa_occ_max"], kv["bulk_g6a_pa_need_max"],
                 (kv["bulk_g6a_tr_wait_dram"] // kv["bulk_g6a_runs_released"])
                 if kv["bulk_g6a_runs_released"] else 0))

    print()
    if anchors:
        print("anchors (published == measured):")
        for wl, cfg, want, got, ok in anchors:
            print("  %-12s %-3s %12d %12d %s" % (wl, cfg, want, got,
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
