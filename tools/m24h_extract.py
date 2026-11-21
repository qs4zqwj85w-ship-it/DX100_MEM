#!/usr/bin/env python3
"""M24-H — extract the audit's measurements.

Inputs, all read-only:
  build/wm/m24f/<trace>__{bulk,simt}.txt   production pair (M24-F), 4 traces
  build/wm/m24h/log/org<o>__<trace>__bulk.txt   storage ladder, org 0/6/7
  build/wm/m24e/summary.csv                mechanism ablation (M24-E), M23 panel
  build/wm/m24h/totals.csv                 storage model (m24h_structures.py)

Outputs:
  cells.csv    one row per cell, every quantity the report quotes
  ladder.csv   cycles and storage per organisation, per trace
  traffic.csv  Bulk vs SIMT memory-system traffic per trace
  checks.txt   invariants (one line per check, FAIL-able)
  report.txt   the markdown tables the report uses

usage: python3 tools/m24h_extract.py
"""
import csv
import re
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m24h")
M24F = os.path.join(ROOT, "build/wm/m24f")
M24E = os.path.join(ROOT, "build/wm/m24e")
LOGD = os.path.join(OUT, "log")

PRIMARY = ["w4_duplow", "criteo_2k", "n1_seq8m", "n3_clu4m"]
M23 = ["orkut", "livejournal", "ogbn"]

FIELDS = ["cycles", "bulk_elems_done", "bulk_elems_total", "conservation_ok",
          "dram_reads", "dram_bytes", "dram_row_hits", "dram_row_acts_sum",
          "dram_row_conflicts", "l1_reads", "l1_hits", "l1_misses",
          "l2_reads", "l2_hits", "l2_misses", "walks", "pte_fetches",
          "walker_busy", "replays", "instr_issued",
          "bulk_idx_lines_req", "bulk_idx_line_bytes", "bulk_tgt_line_bytes",
          "bulk_desc_line_bytes", "bulk_dedup_merged", "bulk_fanout_elems",
          "bulk_fanout_max_backlog", "bulk_g6b_ready_peak", "bulk_g6b_ready_occ_sum",
          "bulk_g6b_pa_capacity", "bulk_g6b_pa_bytes", "bulk_win_vec_bytes",
          "bulk_win_bytes", "bulk_idx_bytes", "bulk_sched_bytes",
          "bulk_unissued_max", "bulk_win_occ_max", "bulk_lines_issued",
          "bulk_g6b_release_pops", "bulk_g6c_admit_attempts", "bulk_sched_opens",
          "bulk_idx_out_max", "bulk_org_policy", "bulk_win_entries",
          "bulk_g6b_release_slot_stall", "bulk_slot_stall", "bulk_win_refuse",
          "peak_total_outstanding", "inflight_max"]

CHECKS = []


def check(ok, label):
    CHECKS.append((bool(ok), label))


def read_log(path):
    """Every `key=value` on every line; the first occurrence wins.

    The logs put several pairs on the summary line (`cycles=1511665
    lines_per_cycle=0.041 dram_gbs=0.00`), so the value is everything up to the
    next space, not the rest of the line.
    """
    d = {}
    if not os.path.exists(path):
        return d
    with open(path) as f:
        for line in f:
            for k, v in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)", line):
                d.setdefault(k, v)
    return d


def num(d, k):
    try:
        return float(d.get(k, "0"))
    except ValueError:
        return 0.0


def main():
    os.makedirs(OUT, exist_ok=True)
    cells = []

    def add(source, cell, trace, org, arm, path):
        d = read_log(path)
        if not d:
            print("MISSING %s" % path, file=sys.stderr)
            return None
        row = {"source": source, "cell": cell, "trace": trace, "org": org, "arm": arm}
        for k in FIELDS:
            row[k] = d.get(k, "")
        row["file"] = os.path.relpath(path, ROOT)
        cells.append(row)
        return row

    prod = {}
    for tr in PRIMARY:
        prod[(tr, "bulk")] = add("m24f", tr + "__bulk", tr, 8, "bulk",
                                 os.path.join(M24F, tr + "__bulk.txt"))
        prod[(tr, "simt")] = add("m24f", tr + "__simt", tr, -1, "simt",
                                 os.path.join(M24F, tr + "__simt.txt"))

    lad = {}
    for o in (0, 6, 7):
        for tr in PRIMARY:
            lad[(o, tr)] = add("m24h", "org%d__%s__bulk" % (o, tr), tr, o, "bulk",
                               os.path.join(LOGD, "org%d__%s__bulk.txt" % (o, tr)))
    for tr in M23:
        lad[(0, tr)] = add("m24h", "org0__%s__bulk" % tr, tr, 0, "bulk",
                           os.path.join(LOGD, "org0__%s__bulk.txt" % tr))
        lad[(8, tr)] = add("m24h", "prod__%s__bulk" % tr, tr, 8, "bulk",
                           os.path.join(LOGD, "prod__%s__bulk.txt" % tr))

    # ---- m24e ablation cells (production org 8 and its ablations) -----------
    ab = {}
    if os.path.exists(os.path.join(M24E, "summary.csv")):
        with open(os.path.join(M24E, "summary.csv")) as f:
            for r in csv.DictReader(f):
                ab[(r["ablation"], r["wl"])] = r

    # ---- invariants ---------------------------------------------------------
    for row in cells:
        tag = row["cell"] + "/" + row["arm"]
        if row["arm"] == "simt":
            check(row["conservation_ok"] == "1", "conservation %s" % tag)
            check(num(row, "cycles") > 1000, "cycles>0 %s" % tag)
            continue
        check(row["conservation_ok"] == "1", "conservation %s" % tag)
        check(num(row, "bulk_elems_done") == num(row, "bulk_elems_total"),
              "all elements done %s" % tag)
        check(num(row, "bulk_elems_total") > 0, "elements>0 %s" % tag)
        check(num(row, "bulk_win_vec_bytes") > 0, "fused window active %s" % tag)
        check(num(row, "bulk_org_policy") == float(row["org"]),
              "org policy = %s %s" % (row["org"], tag))
        want_pa = 4096 if int(row["org"]) >= 7 else 0
        check(num(row, "bulk_g6b_pa_capacity") == want_pa,
              "PA capacity %d %s" % (want_pa, tag))
        # the fused + cohort configuration must actually run the cohort scheduler
        check(num(row, "bulk_sched_opens") > 0, "cohorts opened %s" % tag)
        if int(row["org"]) == 0:
            check(num(row, "bulk_win_bytes") == 279040,
                  "charge unchanged by org (org 0) %s" % tag)

    # every ladder cell of one trace dispatched the same element count
    for tr in PRIMARY:
        tot = {o: num(lad[(o, tr)], "bulk_elems_total") for o in (0, 6, 7)}
        tot[8] = num(prod[(tr, "bulk")], "bulk_elems_total")
        check(len(set(tot.values())) == 1, "same element count across orgs %s" % tr)

    # ---- derived tables -----------------------------------------------------
    def ratio(o, tr):
        p = num(prod[(tr, "bulk")], "cycles")
        c = num(lad[(o, tr)], "cycles")
        return (p / c) if c else 0.0

    ladder_rows = []
    for tr in PRIMARY:
        base = num(prod[(tr, "bulk")], "cycles")
        for o in (0, 6, 7, 8):
            c = base if o == 8 else num(lad[(o, tr)], "cycles")
            if not c or not base:
                continue
            ladder_rows.append({"trace": tr, "org": o, "cycles": "%.0f" % c,
                                "vs_prod": "%.4f" % (c / base),
                                "bulk_vs_simt": "%.4f" % (
                                    num(prod[(tr, "simt")], "cycles") / c)})
    for tr in M23:
        r0, r8 = lad.get((0, tr)), lad.get((8, tr))
        if not r0 or not r8:
            continue
        c0, c8 = num(r0, "cycles"), num(r8, "cycles")
        if not c0 or not c8:
            continue
        ladder_rows.append({"trace": tr, "org": 0, "cycles": "%.0f" % c0,
                            "vs_prod": "%.4f" % (c0 / c8), "bulk_vs_simt": ""})
        # M24-E measured the same production configuration as its `base` cell and
        # its unfused arm as `unfused`: both are anchors for this audit's own runs.
        e = ab.get(("base", tr))
        if e:
            check(abs(float(e["cycles"]) - c8) < 0.5,
                  "production reproduces M24-E base %s" % tr)
        u = ab.get(("unfused", tr))
        if u:
            check(abs(float(u["cycles"]) - c0) < 0.5,
                  "org 0 reproduces M24-E unfused %s (fused is storage-only)" % tr)

    traffic = []
    for tr in PRIMARY:
        b, s = prod[(tr, "bulk")], prod[(tr, "simt")]
        def row(name, key, scale=1.0, nd=0):
            bv, sv = num(b, key) * scale, num(s, key) * scale
            traffic.append({"trace": tr, "metric": name,
                            "bulk": ("%%.%df" % nd) % bv,
                            "simt": ("%%.%df" % nd) % sv,
                            "delta_pct": ("%.1f" % (100.0 * (bv - sv) / sv)) if sv else ""})
        row("cycles", "cycles")
        row("DRAM reads", "dram_reads")
        row("DRAM bytes", "dram_bytes")
        row("DRAM row hits", "dram_row_hits")
        row("DRAM row activations", "dram_row_acts_sum")
        row("L1 reads", "l1_reads")
        row("L1 hits", "l1_hits")
        row("L1 misses", "l1_misses")
        row("L2 reads", "l2_reads")
        row("L2 hits", "l2_hits")
        row("L2 misses", "l2_misses")
        row("walks", "walks")
        row("PTE fetches", "pte_fetches")
        row("walker busy cycles", "walker_busy")

    with open(os.path.join(OUT, "cells.csv"), "w", newline="") as f:
        cols = ["source", "cell", "trace", "org", "arm"] + FIELDS + ["file"]
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in cells:
            w.writerow(r)
    with open(os.path.join(OUT, "ladder.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["trace", "org", "cycles", "vs_prod", "bulk_vs_simt"])
        w.writeheader()
        w.writerows(ladder_rows)
    with open(os.path.join(OUT, "traffic.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["trace", "metric", "bulk", "simt", "delta_pct"])
        w.writeheader()
        w.writerows(traffic)

    fails = [c for c in CHECKS if not c[0]]
    with open(os.path.join(OUT, "checks.txt"), "w") as f:
        for ok, label in CHECKS:
            f.write("%s %s\n" % ("ok  " if ok else "FAIL", label))
        f.write("\n%d checks, %d failures\n" % (len(CHECKS), len(fails)))

    # ---- report -------------------------------------------------------------
    L = []
    L.append("== 1. production pair (M24-F, org 8) ==")
    L.append("%-12s %12s %12s %8s %10s %10s %7s" % (
        "trace", "bulk cyc", "simt cyc", "ratio", "bulk DRAM", "simt DRAM", "rowhit%"))
    for tr in PRIMARY:
        b, s = prod[(tr, "bulk")], prod[(tr, "simt")]
        bc, sc = num(b, "cycles"), num(s, "cycles")
        rh = 100.0 * num(b, "dram_row_hits") / max(1.0, num(b, "dram_reads"))
        L.append("%-12s %12.0f %12.0f %8.3f %10.0f %10.0f %7.1f" % (
            tr, bc, sc, sc / bc, num(b, "dram_reads"), num(s, "dram_reads"), rh))
    L.append("")
    L.append("== 2. storage ladder (Bulk arm only; vs_prod < 1 is *faster*) ==")
    L.append("%-12s %10s %10s %10s %10s   %s" % (
        "trace", "org0", "org6", "org7", "org8", "org8/org0"))
    for tr in PRIMARY:
        row = [num(lad.get((o, tr)) or {}, "cycles") for o in (0, 6, 7)]
        base = num(prod[(tr, "bulk")], "cycles")
        if not row[0] or not base:
            continue
        L.append("%-12s %10.0f %10.0f %10.0f %10.0f   %.3f" % (
            tr, row[0], row[1], row[2], base, base / row[0]))
    L.append("")
    L.append("== 3. M23 panel (org 0 vs production org 8, both re-run here) ==")
    L.append("%-12s %12s %12s %8s" % ("trace", "org0", "org8", "org8/org0"))
    for tr in M23:
        r0, r8 = lad.get((0, tr)), lad.get((8, tr))
        if not r0 or not r8:
            continue
        c0, c8 = num(r0, "cycles"), num(r8, "cycles")
        if not c0 or not c8:
            continue
        L.append("%-12s %12.0f %12.0f %8.3f" % (tr, c0, c8, c8 / c0))
    L.append("")
    L.append("== 4. traffic, Bulk vs SIMT (production) ==")
    L.append("%-12s %-22s %12s %12s %8s" % ("trace", "metric", "bulk", "simt", "delta%"))
    for r in traffic:
        L.append("%-12s %-22s %12s %12s %8s" % (
            r["trace"], r["metric"], r["bulk"], r["simt"], r["delta_pct"]))
    L.append("")
    L.append("== 5. M24-E ablation (M23 panel, production G6D baseline) ==")
    L.append("%-22s %10s %10s %10s %10s" % ("ablation", "orkut", "livejournal", "ogbn", "criteo"))
    for abl in ("cohorts off", "ready pool = 1", "ready pool = 16", "ready pool = 512",
                "DRAM order off", "unfused window", "idx_mode 0"):
        vals = []
        for tr in M23 + ["criteo"]:
            e = ab.get((abl, tr))
            vals.append(e["cycles"] if e else "?")
        L.append("%-22s %10s %10s %10s %10s" % (abl, *vals))
    L.append("")

    with open(os.path.join(OUT, "report.txt"), "w") as f:
        f.write("\n".join(L) + "\n")

    print("cells %d | checks %d, failures %d" % (len(cells), len(CHECKS), len(fails)))
    print("\n".join(L[:14]))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
