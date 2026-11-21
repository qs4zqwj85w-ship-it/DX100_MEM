#!/usr/bin/env python3
"""M25: extract the diagnostic table the efficiency audit needs from *existing*
M24-F logs.  No simulator run, no configuration change.

Inputs (all predate this milestone):
  build/wm/m24f/<trace>__{bulk,simt}.txt   M24-F runs
  build/wm/m24f/summary.csv                M24-F's published per-arm table
  build/wm/m24f/structure.tsv              offline trace structure (tools/m24f_stats.py)

Outputs (build/wm/m25/):
  diag.csv    one row per workload: both arms' counters, deltas, and the Bulk
              engine's cycle budget decomposed into its own stages
  budget.txt  the same, printed per trace, with the arithmetic the audit quotes
  checks.txt  invariants that must hold for the table to be usable
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
F = os.path.join(ROOT, "build", "wm", "m24f")
OUT = os.path.join(ROOT, "build", "wm", "m25")
CHECKS = []


def chk(ok, what, detail=""):
    CHECKS.append((bool(ok), what, detail))
    if not ok:
        print(f"FAIL {what} {detail}")


def kv(path):
    d = {}
    with open(path) as f:
        for line in f:
            for tok in line.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    try:
                        d[k] = float(v)
                    except ValueError:
                        d[k] = v
    return d


def num(d, k, default=0.0):
    v = d.get(k, default)
    return v if isinstance(v, float) else default


def main():
    os.makedirs(OUT, exist_ok=True)
    pub = {}
    with open(os.path.join(F, "summary.csv")) as f:
        for r in csv.DictReader(f):
            pub[(r["trace"], r["arm"])] = r
    st = {}
    with open(os.path.join(F, "structure.tsv")) as f:
        for r in csv.DictReader(f, delimiter="\t"):
            st[r["trace"]] = r

    traces = sorted({k[0] for k in pub})
    rows = []
    for t in traces:
        if (t, "bulk") not in pub or (t, "simt") not in pub:
            continue
        b = kv(os.path.join(F, f"{t}__bulk.txt"))
        s = kv(os.path.join(F, f"{t}__simt.txt"))
        pb, ps = pub[(t, "bulk")], pub[(t, "simt")]
        # Canonical values are M24-F's published columns; the logs supply the
        # engine-internal counters M24-F did not tabulate.
        cyc_b, cyc_s = float(pb["cycles"]), float(ps["cycles"])
        dr_b, dr_s = float(pb["dram_reads"]), float(ps["dram_reads"])
        chk(abs(num(b, "cycles") - cyc_b) < 0.5, f"{t}: bulk cycles match summary.csv")
        chk(abs(num(s, "cycles") - cyc_s) < 0.5, f"{t}: simt cycles match summary.csv")
        chk(abs(num(b, "dram_arrivals") - dr_b) < 0.5, f"{t}: bulk reads match summary.csv")
        chk(abs(num(s, "dram_arrivals") - dr_s) < 0.5, f"{t}: simt reads match summary.csv")
        g = st.get(t, {})
        idx_lines = num(b, "bulk_idx_lines_req")
        elems = num(b, "bulk_elems_total")
        l2_b, l2_s = float(pb["l2_reads"]), float(ps["l2_reads"])
        rows.append({
            "trace": t,
            "speedup": cyc_s / cyc_b,
            "cycles_bulk": cyc_b, "cycles_simt": cyc_s,
            "walks_bulk": float(pb["walks"]), "walks_simt": float(ps["walks"]),
            "dram_reads_bulk": dr_b, "dram_reads_simt": dr_s,
            "dram_reads_delta": dr_b - dr_s,
            "idx_lines": idx_lines,
            "idx_over_delta": (idx_lines / (dr_b - dr_s)) if dr_b != dr_s else 0.0,
            "row_hit_bulk": 100.0 * float(pb["row_hits"]) / dr_b,
            "row_hit_simt": 100.0 * float(ps["row_hits"]) / dr_s,
            "acts_bulk": float(pb["row_acts"]), "acts_simt": float(ps["row_acts"]),
            "l2_reads_bulk": l2_b, "l2_reads_simt": l2_s,
            "l2_miss_bulk": l2_b - dr_b, "l2_miss_simt": l2_s - dr_s,
            "l2_hit_bulk": 100.0 * (l2_b - b.get("l2_misses", 0)) / l2_b if l2_b else 0.0,
            "l2_hit_simt": 100.0 * (l2_s - s.get("l2_misses", 0)) / l2_s if l2_s else 0.0,
            "bank_busy_bulk": float(pb["bank_busy"]), "bank_busy_simt": float(ps["bank_busy"]),
            "cyc_per_read_bulk": cyc_b / dr_b, "cyc_per_read_simt": cyc_s / dr_s,
            "bytes_per_read_bulk": float(pb["bank_busy"]) / dr_b,
            "elements": float(pb["elements"]),
            "batches": num(b, "bulk_batches"),
            "first_done": num(b, "bulk_first_done_cycle"),
            "last_done": num(b, "bulk_last_done_cycle"),
            "org_cycles": num(b, "bulk_org_cycles"),
            "fanout_cycles": num(b, "bulk_fanout_cycles"),
            "sched_cycles": num(b, "bulk_sched_cycles"),
            "rel_slot_stall": num(b, "bulk_g6b_release_slot_stall"),
            "rel_blocked": num(b, "bulk_g6b_release_blocked_cycles"),
            "no_ready": num(b, "bulk_g6b_no_ready_cycles"),
            "backpressure": num(b, "bulk_g6b_full_cycles"),
            "tr_active": num(b, "bulk_g6b_tr_active_cycles"),
            "overlap": num(b, "bulk_g6b_overlap_cycles"),
            "ready_peak": num(b, "bulk_g6b_ready_peak"),
            "ready_mean": num(b, "bulk_g6b_ready_occ_sum") / cyc_b,
            "defer_events": num(b, "bulk_g6c_defer_events"),
            "cohorts_opened": num(b, "bulk_g6c_cohorts_opened"),
            "idx_done": num(b, "bulk_idx_lines_done"),
            # Per-client attribution (cache_diag = 1 in every M24-F cell):
            # 1 = index stream, 2 = PTE, 3 = engine target, 0 = everything else.
            "idx_dram": num(b, "cc1_l2_misses"),
            "idx_l2_hits": num(b, "cc1_l2_hits"),
            "tgt_dram": num(b, "cc3_l2_misses"),
            "tgt_l2_hits": num(b, "cc3_l2_hits"),
            "other_dram": num(b, "cc0_l2_misses"),
            "simt_idx_l1_hits": num(s, "cc1_l1_hits"),
            "simt_idx_l1_reads": num(s, "cc1_l1_reads"),
            "tgt_req_gen": num(b, "bulk_tgt_req_gen"),
            "win_bytes": num(b, "bulk_win_bytes"),
            "simt_req_gen": num(s, "requests_generated"),
            "simt_instr": num(s, "instr_issued"),
            "simt_l1_hits": num(s, "l1_hits"),
            "simt_l1_reads": num(s, "l1_reads"),
            "simt_l1_hit": 100.0 * num(s, "l1_hits") / num(s, "l1_reads") if num(s, "l1_reads") else 0.0,
            "simt_mshr_joins": num(s, "l2_mshr_joins"),
            "simt_unissued_max": num(s, "unissued_max"),
            "simt_l2_mshr_max": num(s, "l2_mshr_max"),
            "tgt_acc": float(g.get("tgt_acc", 0) or 0),
            "accesses": float(g.get("accesses", 0) or 0),
            "tgt_lines": float(g.get("tgt_lines", 0) or 0),
            "tgt_pages": float(g.get("tgt_pages", 0) or 0),
            "ws_kib": float(g.get("ws_kib", 0) or 0),
            "dup_line": float(g.get("dup_line", 0) or 0),
            "seq_frac": float(g.get("seq_frac", 0) or 0),
            "reuse16k": float(g.get("reuse16k", 0) or 0),
            "idx_share": float(g.get("idx_share", 0) or 0),
            "tlb_pressure": float(g.get("tlb_pressure", 0) or 0),
            "cache_pressure": float(g.get("cache_pressure", 0) or 0),
        })

    with open(os.path.join(OUT, "diag.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        for r in rows:
            w.writerow({k: (round(v, 4) if isinstance(v, float) else v) for k, v in r.items()})

    chk(len(rows) == 31, "31 traces have both arms", str(len(rows)))
    for r in rows:
        chk(0.5 < r["speedup"] < 4.0, f"{r['trace']}: both arms completed inside the guard")
        # orkut_tail is a legal degenerate cell: the engine is armed with zero
        # elements, so it has no batches, no index lines and no target requests.
        if r["batches"] > 0:
            chk(r["elements"] > 0, f"{r['trace']}: element count present")
            chk(r["idx_lines"] > 0, f"{r['trace']}: index volume recorded")
        else:
            chk(r["elements"] == 0, f"{r['trace']}: a zero-batch cell does no work",
                f"{r['elements']:.0f}")
        # Trace family: accesses == targets means the trace carries no index
        # accesses (the M23 graph family); a strict superset means it does (the
        # M18/M21 family).  Never fewer -- that would be a loader bug and it
        # would invalidate the index comparison in the audit.
        chk(r["accesses"] >= r["tgt_acc"],
            f"{r['trace']}: trace covers its targets",
            f"{r['accesses']:.0f} vs {r['tgt_acc']:.0f}")

    loses = [r for r in rows if r["speedup"] < 1.02]
    wins = [r for r in rows if r["speedup"] >= 1.10]
    ties = [r for r in rows if 1.02 <= r["speedup"] < 1.10]
    with open(os.path.join(OUT, "budget.txt"), "w") as f:
        f.write("== per-trace DRAM service and index volume (M24-F logs) ==\n")
        f.write(f"{'trace':16s} {'sp':>6s} {'cyc_b':>9s} {'cyc_s':>9s} {'rd_b':>8s} {'rd_s':>8s} "
                f"{'d':>7s} {'idx':>6s} {'d/idx':>6s} {'c/rd_b':>7s} {'c/rd_s':>7s} "
                f"{'hit_b':>6s} {'hit_s':>6s} {'l2h_b':>6s} {'l2h_s':>6s}\n")
        for r in sorted(rows, key=lambda x: x["speedup"]):
            f.write(f"{r['trace']:16s} {r['speedup']:6.3f} {r['cycles_bulk']:9.0f} "
                    f"{r['cycles_simt']:9.0f} {r['dram_reads_bulk']:8.0f} "
                    f"{r['dram_reads_simt']:8.0f} {r['dram_reads_delta']:7.0f} "
                    f"{r['idx_lines']:6.0f} {r['idx_over_delta']:6.2f} "
                    f"{r['cyc_per_read_bulk']:7.2f} {r['cyc_per_read_simt']:7.2f} "
                    f"{r['row_hit_bulk']:6.1f} {r['row_hit_simt']:6.1f} "
                    f"{r['l2_hit_bulk']:6.1f} {r['l2_hit_simt']:6.1f}\n")

        f.write("\n== Bulk cycle budget where Bulk does not win (speedup < 1.02) ==\n")
        f.write("cycles of the reported run; start = first element done, "
                "work = last - first, drain = run - last\n")
        f.write(f"{'trace':16s} {'cycles':>9s} {'start':>7s} {'work':>9s} {'drain':>7s} "
                f"{'slotstall':>9s} {'noready':>8s} {'org':>7s} {'fanout':>7s} "
                f"{'bpress':>7s} {'rdm':>6s} {'rdpk':>5s}\n")
        for r in sorted(loses, key=lambda x: x["speedup"]):
            f.write(f"{r['trace']:16s} {r['cycles_bulk']:9.0f} {r['first_done']:7.0f} "
                    f"{r['last_done'] - r['first_done']:9.0f} "
                    f"{r['cycles_bulk'] - r['last_done']:7.0f} "
                    f"{r['rel_slot_stall']:9.0f} {r['no_ready']:8.0f} "
                    f"{r['org_cycles']:7.0f} {r['fanout_cycles']:7.0f} "
                    f"{r['backpressure']:7.0f} {r['ready_mean']:6.0f} {r['ready_peak']:5.0f}\n")

        f.write("\n== the index arithmetic ==\n")
        for r in sorted(rows, key=lambda x: x["speedup"]):
            if r["dram_reads_delta"] > 0:
                f.write(f"{r['trace']:16s} index lines {r['idx_lines']:7.0f}  read delta "
                        f"{r['dram_reads_delta']:7.0f}  delta - index = "
                        f"{r['dram_reads_delta'] - r['idx_lines']:7.0f}  index covers "
                        f"{100.0 * r['idx_lines'] / r['dram_reads_delta']:5.1f}%\n")

        f.write("\n== SIMT-side work on the losing traces ==\n")
        f.write(f"{'trace':16s} {'instr':>8s} {'requests':>9s} {'req/instr':>9s} "
                f"{'l1hit%':>7s} {'mshr_join':>9s} {'unissued_max':>12s}\n")
        for r in sorted(loses, key=lambda x: x["speedup"]):
            f.write(f"{r['trace']:16s} {r['simt_instr']:8.0f} {r['simt_req_gen']:9.0f} "
                    f"{r['simt_req_gen'] / max(1.0, r['simt_instr']):9.2f} "
                    f"{r['simt_l1_hit']:7.1f} {r['simt_mshr_joins']:9.0f} "
                    f"{r['simt_unissued_max']:12.0f}\n")

    ok = sum(1 for c in CHECKS if c[0])
    with open(os.path.join(OUT, "checks.txt"), "w") as f:
        f.write(f"M25 extraction: {len(CHECKS)} checks, {len(CHECKS) - ok} failures\n")
        for good, what, detail in CHECKS:
            f.write(f"{'ok  ' if good else 'FAIL'} {what} {detail}\n")
    print(f"{len(CHECKS)} checks: {len(CHECKS) - ok} failures")
    print(f"wrote {OUT}/diag.csv, budget.txt, checks.txt  ({len(rows)} traces; "
          f"{len(wins)} win >=1.10x, {len(ties)} marginal, {len(loses)} <1.02x)")
    return 0 if ok == len(CHECKS) else 1


if __name__ == "__main__":
    sys.exit(main())
