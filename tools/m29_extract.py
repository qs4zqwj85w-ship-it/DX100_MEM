#!/usr/bin/env python3
"""M29 — extract paired results from build/wm/m29 logs into CSV + invariant checks.

Reads every <id>__<arm><suffix>.txt, pairs the arms, and writes
build/wm/m29/results.csv plus a validation summary on stdout.

Invariants checked per pair (both arms must agree):
  trace_accesses, accesses            -- same work
  conservation_ok == 1                -- the model's own conservation gate
  dispatch_partition_ok == 1, dispatch_unaccounted == 0 (Bulk arm)
  bulk_elems + simt_elems == trace_accesses (Bulk arm)
  dram_row_hits + dram_row_conflicts + 2 == dram_reads (both arms)
  cycles > 0 and below the 500M guard
"""
import csv, glob, os, re, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = os.path.join(ROOT, "build", "wm", "m29")

FIELDS = [
    "cycles", "accesses", "trace_accesses", "trace_instr", "trace_lines", "trace_pages",
    "instr_issued", "completed", "completion_hash", "conservation_ok", "conservation_violations",
    # translation
    "walks", "pte_fetches", "walker_busy", "walker_max", "walk_latency", "walk_wc_hits",
    "tlb0_hits", "tlb0_misses", "tlb0_mshr_stalls", "tlb0_mshr_max", "tlb0_mshr_occ",
    "l2tlb_hits", "l2tlb_misses", "l2tlb_dedups", "l2tlb_walk_stalls", "l2tlb_max_occ",
    "tlb_refuse_full", "tlb_refuse_parked",
    # cache
    "l1_reads", "l1_hits", "l1_misses", "l1_mshr_max", "l1_stall_cycles", "l1_evict",
    "l2_reads", "l2_hits", "l2_misses", "l2_mshr_max", "l2_stall_cycles", "l2_evict",
    # dram
    "dram_reads", "dram_arrivals", "dram_row_hits", "dram_row_conflicts", "dram_row_acts_sum",
    "dram_row_acts_max", "dram_rows_used", "dram_busy_cycles", "dram_starved_cycles",
    "dram_blocked_cycles", "dram_max_q", "dram_bytes", "dram_diag_decisions",
    # latency
    "lat_sum", "lat_tlb_sum", "lat_cache_sum", "lat_mean_x100", "lat_tlb_mean", "lat_p50", "lat_p95",
    # outstanding
    "inflight_max", "inflight_occ", "peak_total_outstanding", "unissued_cycles", "unissued_max",
    # dispatch
    "dispatch_mode", "dispatch_partition_ok", "dispatch_unaccounted",
    "simt_elems", "bulk_elems", "bulk_elems_done", "bulk_elems_total", "simt_share_x1000",
    "bulk_instr", "simt_instr", "simt_completed", "simt_issue_stalls", "simt_peak_slot",
    # bulk structures
    "bulk_batches", "bulk_windows", "bulk_g6b_windows", "bulk_g6c_windows",
    "bulk_g6b_ready_peak", "bulk_g6b_ready_occ_sum", "bulk_g6b_backpressure_cycles",
    "bulk_g6b_runs_released", "bulk_g6b_tr_requests", "bulk_g6b_tr_wait_dram", "bulk_g6b_tr_wait_max",
    "bulk_g6c_admit_attempts", "bulk_g6c_admit_ok", "bulk_g6c_refused", "bulk_g6c_defer_events",
    "bulk_g6c_defer_depth_max", "bulk_g6c_dup_submit_blocked", "bulk_g6c_cohorts_opened",
    "bulk_idx_lines_req", "bulk_idx_lines_done", "bulk_idx_line_bytes", "bulk_idx_bytes",
    "bulk_win_entries", "bulk_win_occ_max", "bulk_win_size_mean", "bulk_win_bytes",
    "bulk_win_vec_bytes", "bulk_dedup_merged", "bulk_g6b_pa_bytes", "bulk_win_recs",
    "bulk_pe", "bulk_org_policy",
]


def parse(path):
    d = {}
    with open(path, errors="replace") as f:
        for line in f:
            # several lines carry several `key=value` tokens (`cycles=N lines_per_cycle=..`),
            # so harvest every token and keep the first occurrence of each key
            for m in re.finditer(r"([a-z0-9_]+)=(\S*)", line):
                d.setdefault(m.group(1), m.group(2).rstrip(",;"))
    return d


def num(d, k):
    v = d.get(k)
    if v is None:
        return None
    v = v.strip()
    try:
        return int(v)
    except ValueError:
        pass
    try:
        return float(v)
    except ValueError:
        return None


def main():
    cells = defaultdict(dict)
    for p in sorted(glob.glob(os.path.join(D, "*__*.txt"))):
        base = os.path.basename(p)[:-4]
        m = re.match(r"^(.*)__(simt|bulk)(.*)$", base)
        if not m:
            continue
        cells[m.group(1)][m.group(2) + m.group(3)] = p

    rows, problems = [], []
    n_pairs = 0
    for cid, arms in sorted(cells.items()):
        s = arms.get("simt")
        b = arms.get("bulk")
        if not (s and b):
            problems.append(f"{cid}: unpaired ({sorted(arms)})")
            continue
        ds, db = parse(s), parse(b)
        row = {"id": cid}
        cs, cb = num(ds, "cycles"), num(db, "cycles")
        row["cycles_simt"], row["cycles_bulk"] = cs, cb
        row["speedup"] = round(cs / cb, 4) if cs and cb else None
        for k in FIELDS:
            row[f"{k}_simt"] = ds.get(k, "")
            row[f"{k}_bulk"] = db.get(k, "")
        rows.append(row)
        n_pairs += 1

        # ---- invariants ------------------------------------------------------
        for arm, d in (("simt", ds), ("bulk", db)):
            if num(d, "conservation_ok") != 1:
                problems.append(f"{cid}/{arm}: conservation_ok={d.get('conservation_ok')} "
                                f"violations={d.get('conservation_violations')}")
            if not (cb and num(d, "cycles") > 0):
                problems.append(f"{cid}/{arm}: no valid cycle count")
            if num(d, "cycles") and num(d, "cycles") >= 500_000_000:
                problems.append(f"{cid}/{arm}: reached the 500M cycle guard")
            h, c, r = (num(d, "dram_row_hits"), num(d, "dram_row_conflicts"), num(d, "dram_reads"))
            if None not in (h, c, r) and h + c + 2 != r:
                problems.append(f"{cid}/{arm}: row identity {h}+{c}+2 != {r}")
            if num(d, "completion_hash") in (None, 0):
                problems.append(f"{cid}/{arm}: missing completion_hash")
        if ds.get("trace_accesses") != db.get("trace_accesses"):
            problems.append(f"{cid}: trace_accesses differ "
                            f"{ds.get('trace_accesses')} vs {db.get('trace_accesses')}")
        if ds.get("accesses") != db.get("accesses"):
            problems.append(f"{cid}: accesses differ {ds.get('accesses')} vs {db.get('accesses')}")
        if num(db, "dispatch_partition_ok") != 1:
            problems.append(f"{cid}/bulk: dispatch_partition_ok={db.get('dispatch_partition_ok')}")
        if num(db, "dispatch_unaccounted") != 0:
            problems.append(f"{cid}/bulk: dispatch_unaccounted={db.get('dispatch_unaccounted')}")
        se, be, ta = num(db, "simt_elems"), num(db, "bulk_elems"), num(db, "trace_accesses")
        if None not in (se, be, ta) and se + be != ta:
            problems.append(f"{cid}/bulk: simt_elems {se} + bulk_elems {be} != trace_accesses {ta}")

    # determinism / suffix pairs
    det = []
    for cid, arms in sorted(cells.items()):
        for arm in ("simt", "bulk"):
            a, r = arms.get(arm), arms.get(arm + "__repeat")
            if a and r:
                ha = parse(a).get("completion_hash")
                hr = parse(r).get("completion_hash")
                ca, cr = parse(a).get("cycles"), parse(r).get("cycles")
                det.append((cid, arm, ca, cr, ha, hr, ca == cr and ha == hr))

    keys = ["id", "cycles_simt", "cycles_bulk", "speedup"] + \
           [f"{k}_{a}" for k in FIELDS for a in ("simt", "bulk")]
    out = os.path.join(D, "results.csv")
    # keep only the columns that carry at least one non-empty value
    keep = [k for k in keys if any(str(r.get(k, "")) != "" for r in rows)]
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keep, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)

    print(f"pairs written: {n_pairs}  ->  {out}  ({len(keep)} columns)")
    print(f"problems: {len(problems)}")
    for p in problems:
        print("  !!", p)
    if det:
        print("determinism re-runs:")
        for cid, arm, ca, cr, ha, hr, ok in det:
            print(f"  {'OK ' if ok else 'DIFF'} {cid}/{arm}: {ca}->{cr} hash {ha}->{hr}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
