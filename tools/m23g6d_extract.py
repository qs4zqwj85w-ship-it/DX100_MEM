#!/usr/bin/env python3
"""M23-G6D — translation throughput scaling: the cell extractor.

Cells: build/wm/m23g6d/g6d_<CELL>.txt

  A0  walkers 2, cohorts 2, dTLB MSHR 4, L 150   (G6C baseline, Orkut)
  A1  walkers 4                                 (Orkut)
  A2  walkers 8                                 (Orkut)
  B0  walkers 4, cohorts 2, MSHR 4              (Orkut; == A1, determinism)
  B1  walkers 4, cohorts 4, MSHR 8              (Orkut)
  B2  walkers 4, cohorts 8, MSHR 8              (Orkut)
  S1  walkers 8, cohorts 8, MSHR 8              (Orkut; supplementary)
  C0  winner, L 150                             (Orkut)
  C1  winner, L 75                              (Orkut)
  DLJ  winner on LiveJournal
  DOGBN winner on ogbn-products train

WALKER UTILIZATION — corrected. `walker_busy` is the per-cycle sum of busy
walkers, so the fraction of the *walker resource* in use is

    walker_busy / (walkers x cycles)

The M23-G6C extractor divided by `BANKS x cycles`; that was only correct because
the baseline has walkers == banks == 2. Here the divisor is the configured walker
count, parsed from each log's own `walkers=N wc=.. walk_lat=..x..` header, so a
cell cannot be scored against the wrong resource. Utilisation above 100 % or a
`walker_max` above the configured count is a failed cell, not a result.
"""
import csv
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g6d")

# cell -> (workload, walkers, cohorts, dtlb_mshr, walk_latency_per_level)
CELLS = {
    "A0": ("orkut", 2, 2, 4, 150),
    "A1": ("orkut", 4, 2, 4, 150),
    "A2": ("orkut", 8, 2, 4, 150),
    "B0": ("orkut", 4, 2, 4, 150),
    "B1": ("orkut", 4, 4, 8, 150),
    "B2": ("orkut", 4, 8, 8, 150),
    "S1": ("orkut", 8, 8, 8, 150),
    "C0": ("orkut", 4, 8, 8, 150),
    "C1": ("orkut", 4, 8, 8, 75),
    "DLJ": ("livejournal", 4, 8, 8, 150),
    "DOGBN": ("ogbntrain", 4, 8, 8, 150),
}

# Published G6C baselines (M23-G6C matrix, bulk_org = 8, frozen config).
BASE = {"orkut": 16505468, "livejournal": 28256445, "ogbntrain": 199738512}
# Other arms, for the relative columns (M23-G6C matrix).
ARMS = {
    "orkut": {"M0": 49067545, "P0": 16714538, "P2": 23965633, "G6B": 25938106},
    "livejournal": {"M0": 67234692, "P0": 28309390, "P2": 32260832, "G6B": 39200163},
    "ogbntrain": {"M0": 207287728, "P0": 199882548, "P2": 347389697, "G6B": 375172893},
}
REF = "A0"           # the G6C baseline cell (Orkut)
L2TLB_MSHR = 8       # fixed in every cell


def parse(path):
    kv = {}
    txt = {}
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
                txt[k] = v
    return kv, txt


def div(x, y):
    return x / y if y else 0.0


def pct(x, y):
    return 100.0 * x / y if y else 0.0


def hist_mean(kv, prefix, n):
    tot = sum(kv.get("%s%d" % (prefix, i), 0) for i in range(n))
    s = sum(i * kv.get("%s%d" % (prefix, i), 0) for i in range(n))
    return (div(s, tot), tot)


def load(cell):
    path = os.path.join(OUT, "g6d_%s.txt" % cell)
    if not os.path.exists(path):
        return None
    kv, txt = parse(path)
    return (kv, txt) if "cycles" in kv else None


def main():
    data, missing, problems = {}, [], []
    for cell in CELLS:
        r = load(cell)
        if r is None:
            missing.append(cell)
        else:
            data[cell] = r
    if missing:
        problems.append("missing cells: " + ", ".join(missing))

    rows = []
    for cell in CELLS:
        if cell not in data:
            continue
        kv, txt = data[cell]
        wl, want_w, want_c, want_m, want_l = CELLS[cell]
        c = kv["cycles"]

        # ---- provenance: the log header must echo the cell we asked for ----
        hdr = txt.get("walkers", "")          # "2 wc=16 walk_lat=150x2"
        wk = int(re.search(r"(\d+)", hdr.split(" wc=")[0]).group(1)) if hdr else -1
        lv = txt.get("l2tlb", "")             # "512x4 mshr=8 lat=4"
        m_lv = re.search(r"mshr=(\d+)", lv or "")
        dt = txt.get("dtlb", "")              # "16x4 mshr=4 parked=2"
        m_dt = re.search(r"mshr=(\d+)", dt or "")
        pw = re.search(r"walk_lat=(\d+)x(\d+)", hdr or "")
        got = {
            "walkers": wk,
            "dtlb_mshr": int(m_dt.group(1)) if m_dt else -1,
            "l2tlb_mshr": int(m_lv.group(1)) if m_lv else -1,
            "cohorts": kv.get("bulk_sched_cohorts", -1),
            "walk_lat": int(pw.group(1)) if pw else -1,
            "levels": int(pw.group(2)) if pw else -1,
        }
        if got["walkers"] != want_w:
            problems.append("%s: walkers %d, cell says %d" % (cell, got["walkers"], want_w))
        if got["dtlb_mshr"] != want_m:
            problems.append("%s: dtlb_mshr %d, cell says %d" % (cell, got["dtlb_mshr"], want_m))
        if got["cohorts"] != want_c:
            problems.append("%s: cohorts %d, cell says %d" % (cell, got["cohorts"], want_c))
        if got["walk_lat"] != want_l:
            problems.append("%s: walk_lat %d, cell says %d" % (cell, got["walk_lat"], want_l))
        if got["l2tlb_mshr"] != L2TLB_MSHR:
            problems.append("%s: l2tlb_mshr %d != %d" % (cell, got["l2tlb_mshr"], L2TLB_MSHR))

        walks = kv.get("walks", 0)
        wbusy = kv.get("walker_busy", 0)
        service = got["levels"] * got["walk_lat"]
        util = pct(wbusy, got["walkers"] * c)
        floor = div(walks * service, got["walkers"])

        # ---- validation ----
        if kv.get("conservation_ok", 0) != 1 or kv.get("conservation_violations", 0) != 0:
            problems.append("%s: conservation not OK" % cell)
        if c >= 3000000000:
            problems.append("%s: hit max_cycles" % cell)
        for k in ("bulk_tgt_req_gen", "bulk_tgt_req_done", "bulk_elems_done",
                  "bulk_elems_total", "bulk_lines_issued", "bulk_org_runs"):
            if k not in kv:
                problems.append("%s: missing %s" % (cell, k))
        if kv.get("bulk_tgt_req_gen") != kv.get("bulk_tgt_req_done"):
            problems.append("%s: targets %d != done %d" % (cell, kv.get("bulk_tgt_req_gen", 0),
                                                           kv.get("bulk_tgt_req_done", 0)))
        if kv.get("bulk_elems_done") != kv.get("bulk_elems_total"):
            problems.append("%s: elements %d != total %d" % (cell, kv.get("bulk_elems_done", 0),
                                                             kv.get("bulk_elems_total", 0)))
        if kv.get("bulk_lines_issued") != kv.get("bulk_tgt_req_gen"):
            problems.append("%s: lines %d != targets %d" % (cell, kv.get("bulk_lines_issued", 0),
                                                            kv.get("bulk_tgt_req_gen", 0)))
        if kv.get("bulk_g6b_tr_requests") != kv.get("bulk_g6b_tr_completed"):
            problems.append("%s: translations %d != %d" % (cell, kv.get("bulk_g6b_tr_completed", 0),
                                                           kv.get("bulk_g6b_tr_requests", 0)))
        if kv.get("bulk_g6b_runs_released") != kv.get("bulk_lines_issued"):
            problems.append("%s: released %d != issued %d" % (cell, kv.get("bulk_g6b_runs_released", 0),
                                                              kv.get("bulk_lines_issued", 0)))
        if kv.get("bulk_g6b_tr_requests") != kv.get("bulk_org_runs"):
            problems.append("%s: probes %d != runs %d" % (cell, kv.get("bulk_g6b_tr_requests", 0),
                                                          kv.get("bulk_org_runs", 0)))
        if kv.get("bulk_g6b_ready_occ_sum") != kv.get("bulk_g6b_tr_wait_dram"):
            problems.append("%s: buffer integral != sum of residence" % cell)
        if kv.get("bulk_g6b_ready_peak", 0) > kv.get("bulk_g6b_pa_capacity", 0):
            problems.append("%s: buffer peak above capacity" % cell)
        for k in ("bulk_addr_mismatch", "bulk_residency_violations",
                  "bulk_issue_monotonic_violations"):
            if kv.get(k, 0) != 0:
                problems.append("%s: %s = %d" % (cell, k, kv.get(k, 0)))
        if kv.get("bulk_g6c_dup_submit_blocked", -1) != 0:
            problems.append("%s: duplicate submissions" % cell)
        if kv.get("bulk_g6c_refuse_other", -1) != 0:
            problems.append("%s: unclassified refusals" % cell)
        if (kv.get("bulk_g6c_refuse_parked", 0) + kv.get("bulk_g6c_refuse_full", 0)
                != kv.get("bulk_g6c_refused", 0)):
            problems.append("%s: refusal causes do not sum" % cell)
        if kv.get("walker_max", 0) > got["walkers"]:
            problems.append("%s: %d simultaneous walkers of %d configured"
                            % (cell, kv.get("walker_max", 0), got["walkers"]))
        if util > 100.5:
            problems.append("%s: walker utilisation %.1f%% > 100" % (cell, util))
        if kv.get("tlb0_mshr_max", 0) > got["dtlb_mshr"]:
            problems.append("%s: dTLB MSHR peak above capacity" % cell)
        if kv.get("bulk_g6c_defer_depth_max", 0) > got["cohorts"]:
            problems.append("%s: deferral depth above cohorts" % cell)

        # ---- occupancy distributions ----
        # The model ticks cycles 0..N where N is the reported last cycle, so a
        # per-cycle sample covers N+1 rows; that is structural, not drift.
        mshr_mean, mshr_n = hist_mean(kv, "tlb0_mshr_h", 17)
        l2_mean, l2_n = hist_mean(kv, "l2tlb_occ_h", 17)
        if mshr_n not in (c, c + 1):
            problems.append("%s: dTLB histogram covers %d of %d cycles" % (cell, mshr_n, c))
        if l2_n not in (c, c + 1):
            problems.append("%s: L2 TLB histogram covers %d of %d cycles" % (cell, l2_n, c))
        # The ready buffer is only sampled while a window is being issued, so its
        # histogram covers the window-active cycles, not the whole run: check the
        # coverage is a subset, and that the bucket bounds bracket the exact
        # occupancy integral.
        ready_n = sum(kv.get("bulk_g6b_ready_h%d" % i, 0) for i in range(13))
        cap = kv.get("bulk_g6b_pa_capacity", 0)
        lo = hi = 0
        for b in range(13):
            n = kv.get("bulk_g6b_ready_h%d" % b, 0)
            blo, bhi = (0, 0) if b == 0 else ((1 << (b - 1)), cap if b >= 12 else (1 << b) - 1)
            lo += blo * n
            hi += bhi * n
        occ = kv.get("bulk_g6b_ready_occ_sum", 0)
        if ready_n > c:
            problems.append("%s: buffer histogram covers %d of %d cycles" % (cell, ready_n, c))
        if not (lo <= occ <= hi):
            problems.append("%s: buffer occupancy integral %d outside the histogram's [%d, %d]"
                            % (cell, occ, lo, hi))
        if ready_n < 0.9 * c:
            problems.append("%s: buffer histogram covers only %.1f%% of cycles"
                            % (cell, 100.0 * ready_n / c))

        rows.append({
            "cell": cell, "wl": wl, "cycles": c,
            "walkers": got["walkers"], "cohorts": got["cohorts"],
            "dtlb_mshr": got["dtlb_mshr"], "walk_lat": got["walk_lat"],
            "service": service,
            "vs_A0_pct": pct(c - BASE["orkut"], BASE["orkut"]) if wl == "orkut" else "",
            "vs_base_pct": pct(c - BASE[wl], BASE[wl]),
            "speedup_M0": div(ARMS[wl]["M0"], c),
            "vs_P0_pct": pct(c - ARMS[wl]["P0"], ARMS[wl]["P0"]),
            "vs_P2_pct": pct(c - ARMS[wl]["P2"], ARMS[wl]["P2"]),
            "vs_G6B_pct": pct(c - ARMS[wl]["G6B"], ARMS[wl]["G6B"]),
            # translation
            "walks": walks, "walk_service_cycles": div(wbusy, walks),
            "walker_busy": wbusy, "walker_util_pct": util,
            "active_walkers_avg": div(wbusy, c), "walker_max": kv.get("walker_max", 0),
            "walker_idle_cycles": got["walkers"] * c - wbusy,
            "walk_floor": floor, "gap_pct": pct(c - floor, floor),
            "walks_per_cycle": div(walks, c), "walk_rate_1k": 1000.0 * div(walks, c),
            "tlb0_reads": kv.get("tlb0_reads", 0), "tlb0_hit_pct": pct(kv.get("tlb0_hits", 0),
                                                                     kv.get("tlb0_reads", 1)),
            "tlb0_misses": kv.get("tlb0_misses", 0),
            "tlb0_mshr_avg": div(kv.get("tlb0_mshr_occ", 0), c),
            "tlb0_mshr_max": kv.get("tlb0_mshr_max", 0),
            "tlb0_mshr_at_cap_pct": pct(kv.get("tlb0_mshr_h%d" % min(got["dtlb_mshr"], 16), 0), c),
            "tlb0_mshr_cap": got["dtlb_mshr"],
            "l2tlb_hits": kv.get("l2tlb_hits", 0), "l2tlb_misses": kv.get("l2tlb_misses", 0),
            "l2tlb_evict": kv.get("l2tlb_evict", 0), "l2tlb_dedups": kv.get("l2tlb_dedups", 0),
            "l2tlb_occ_avg": div(kv.get("l2tlb_occ", 0), c),
            "l2tlb_max_occ": kv.get("l2tlb_max_occ", 0),
            "l2tlb_at_cap_pct": pct(kv.get("l2tlb_occ_h%d" % L2TLB_MSHR, 0), c),
            "l2tlb_walk_stalls": kv.get("l2tlb_walk_stalls", 0),
            "wc_hits": kv.get("walk_wc_hits", 0), "pte": kv.get("pte_fetches", 0),
            # admission
            "attempts": kv.get("bulk_g6c_admit_attempts", 0),
            "accepted": kv.get("bulk_g6c_admit_accepted", 0),
            "refused": kv.get("bulk_g6c_refused", 0),
            "ref_parked": kv.get("bulk_g6c_refuse_parked", 0),
            "ref_full": kv.get("bulk_g6c_refuse_full", 0),
            "readmit": kv.get("bulk_g6c_deferred_readmitted", 0),
            "defer_age_mean": div(kv.get("bulk_g6c_defer_age_sum", 0),
                                  kv.get("bulk_g6c_deferred_readmitted", 1)),
            "defer_age_max": kv.get("bulk_g6c_defer_age_max", 0),
            "defer_depth_mean": div(kv.get("bulk_g6c_defer_depth_sum", 0),
                                    kv.get("bulk_g6c_defer_depth_n", 1)),
            "defer_depth_max": kv.get("bulk_g6c_defer_depth_max", 0),
            "cohorts_opened": kv.get("bulk_g6c_cohorts_opened", 0),
            "admit_idle_pct": pct(kv.get("bulk_g6c_admit_idle_cycles", 0), c),
            "tlb_refuse_parked": kv.get("tlb_refuse_parked", 0),
            "tlb_refuse_full": kv.get("tlb_refuse_full", 0),
            # buffer
            "buf_cap": kv.get("bulk_g6b_pa_capacity", 0), "buf_peak": kv.get("bulk_g6b_ready_peak", 0),
            "buf_avg": div(kv.get("bulk_g6b_ready_occ_sum", 0), c),
            "buf_empty_pct": pct(kv.get("bulk_g6b_ready_h0", 0), c),
            "buf_ge256_pct": pct(sum(kv.get("bulk_g6b_ready_h%d" % i, 0) for i in (9, 10, 11, 12)), c),
            "buf_ge1024_pct": pct(sum(kv.get("bulk_g6b_ready_h%d" % i, 0) for i in (11, 12)), c),
            "buf_ge2048_pct": pct(kv.get("bulk_g6b_ready_h12", 0), c),
            "buf_hist_cycles": ready_n,
            "residence_mean": div(kv.get("bulk_g6b_tr_wait_dram", 0),
                                  kv.get("bulk_g6b_runs_released", 1)),
            "residence_max": kv.get("bulk_g6b_tr_wait_max", 0),
            "bp_pct": pct(kv.get("bulk_g6b_backpressure_cycles", 0), c),
            "no_ready_pct": pct(kv.get("bulk_g6b_no_ready_cycles", 0), c),
            "tr_active_pct": pct(kv.get("bulk_g6b_tr_active_cycles", 0), c),
            "tgt_out_pct": pct(kv.get("bulk_g6b_tgt_out_cycles", 0), c),
            "overlap_pct": pct(kv.get("bulk_g6b_overlap_cycles", 0), c),
            "overlap_of_tgt_pct": pct(kv.get("bulk_g6b_overlap_cycles", 0),
                                      kv.get("bulk_g6b_tgt_out_cycles", 1)),
            "tr_per_release": 1.0,
            # DRAM
            "dram_reads": kv.get("dram_reads", 0),
            "row_hit_pct": pct(kv.get("dram_row_hits", 0),
                               kv.get("dram_row_hits", 0) + kv.get("dram_row_conflicts", 0)),
            "rows_used": kv.get("dram_rows_used", 0),
            "bank0_busy": kv.get("dram_bank0_busy", 0), "bank1_busy": kv.get("dram_bank1_busy", 0),
            "bank_util_pct": pct(kv.get("dram_busy_cycles", 0), 2 * c),
            "bytes": kv.get("dram_bytes", 0),
            "svc_mean": div(kv.get("dram_svc_hit_cycles", 0) + kv.get("dram_svc_miss_cycles", 0),
                            kv.get("dram_svc_events", 1)),
            "wait_mean": div(kv.get("dram_wait_sum", 0), kv.get("dram_svc_events", 1)),
            "q_occ_mean": div(kv.get("dram_q_occ_sum", 0), c),
            "starved_pct": pct(kv.get("dram_starved_cycles", 0), c),
            "dram_floor": max(kv.get("dram_bank0_busy", 0), kv.get("dram_bank1_busy", 0)),
            "dram_headroom_pct": pct(max(kv.get("dram_bank0_busy", 0), kv.get("dram_bank1_busy", 0)), c),
        })

    # ---- baseline reproducibility (Orkut A0 must be the published G6C cycle) ----
    if "A0" in data:
        if data["A0"][0]["cycles"] != BASE["orkut"]:
            problems.append("A0: %d != published G6C baseline %d"
                            % (data["A0"][0]["cycles"], BASE["orkut"]))

    if rows:
        with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            for r in rows:
                w.writerow({k: (round(v, 4) if isinstance(v, float) else v) for k, v in r.items()})

    print("== end-to-end")
    print("%-6s %-12s %5s %5s %8s %13s %9s %8s %8s %8s %8s"
          % ("cell", "workload", "W", "coh", "mshr", "cycles", "vs A0", "xM0", "vs P0", "vs P2", "vs G6B"))
    for r in rows:
        print("%-6s %-12s %5d %5d %8d %13d %8.2f%% %8.3f %7.1f%% %7.1f%% %7.1f%%"
              % (r["cell"], r["wl"], r["walkers"], r["cohorts"], r["dtlb_mshr"], r["cycles"],
                 r["vs_A0_pct"] if r["wl"] == "orkut" else r["vs_base_pct"],
                 r["speedup_M0"], r["vs_P0_pct"], r["vs_P2_pct"], r["vs_G6B_pct"]))

    print("\n== translation: walkers, walkers actually in use, and the floor")
    print("%-6s %5s %10s %9s %8s %8s %9s %10s %9s %8s"
          % ("cell", "W", "walks", "busy", "util%", "avgAct", "idleW", "floor", "gap%", "svc/walk"))
    for r in rows:
        print("%-6s %5d %10d %9d %7.2f%% %8.3f %9d %10.0f %8.2f%% %8.1f"
              % (r["cell"], r["walkers"], r["walks"], r["walker_busy"], r["walker_util_pct"],
                 r["active_walkers_avg"], r["walker_idle_cycles"], r["walk_floor"], r["gap_pct"],
                 r["walk_service_cycles"]))

    print("\n== concurrency: MSHR occupancy, pages in flight, admission")
    print("%-6s %6s %8s %7s %9s %8s %7s %8s %9s %9s %8s"
          % ("cell", "mshr", "dMshrAv", "dAtCap%", "l2MshrAv", "l2Max", "l2Cap%", "l2Stall",
             "attempts", "accepted", "deferAge"))
    for r in rows:
        print("%-6s %6d %8.2f %6.2f%% %9.2f %8d %6.2f%% %8d %9d %9d %8.0f"
              % (r["cell"], r["dtlb_mshr"], r["tlb0_mshr_avg"], r["tlb0_mshr_at_cap_pct"],
                 r["l2tlb_occ_avg"], r["l2tlb_max_occ"], r["l2tlb_at_cap_pct"],
                 r["l2tlb_walk_stalls"], r["attempts"], r["accepted"], r["defer_age_mean"]))

    print("\n== ready buffer and DRAM")
    print("%-6s %8s %8s %8s %9s %8s %8s %8s %8s %9s %8s"
          % ("cell", "bufAvg", "peak", "empty%", ">=1024%", "resid", "noRdy%", "ovl%",
             "rowHit%", "bankBusy", "svc/rd"))
    for r in rows:
        print("%-6s %8.1f %8d %7.2f%% %8.2f%% %8.1f %7.2f%% %7.2f%% %7.2f%% %9d %8.2f"
              % (r["cell"], r["buf_avg"], r["buf_peak"], r["buf_empty_pct"], r["buf_ge1024_pct"],
                 r["residence_mean"], r["no_ready_pct"], r["overlap_pct"], r["row_hit_pct"],
                 r["dram_floor"], r["svc_mean"]))

    print("\n== DRAM bound: the busier bank's busy cycles is the earliest cycle count any")
    print("   translation improvement can reach")
    for r in rows:
        print("   %-6s bank0=%11d bank1=%11d headroom=%5.1f%% of %d cycles"
              % (r["cell"], r["bank0_busy"], r["bank1_busy"], r["dram_headroom_pct"], r["cycles"]))

    print()
    if problems:
        print("%d problem(s):" % len(problems))
        for p in problems:
            print("  " + p)
        return 1
    print("all %d cells complete; all checks passed" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
