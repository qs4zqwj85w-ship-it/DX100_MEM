#!/usr/bin/env python3
"""M24-A — four-way page / DRAM organization: tables, factorial, identities.

  python3 tools/m24a_extract.py

Reads build/wm/m24a/m24_<cell>_M<map>_<workload>.txt for cell in A B C D and
map in 0..3, writes summary.csv and prints:

  * the four-way table per workload and mapping (cycles, DRAM, translation,
    pool and organizer counters);
  * the factorial: page-only, DRAM-only, combined, both incremental effects and
    Interaction = T_D - T_B - T_C + T_A, with its sign read as "lower is
    better" (negative interaction = the two mechanisms together save more than
    the sum of their separate savings);
  * the identities every cell must satisfy and the anchors that must reproduce.
"""
import csv
import os
import sys

DIR = "build/wm/m24a"
WLS = ["orkut", "livejournal", "ogbntrain"]
MAPS = [0, 1, 2, 3]
CELLS = [("A", 7, 0), ("B", 8, 0), ("C", 7, 1), ("D", 8, 1)]
CLIENT = {0: "simt", 1: "engine", 2: "pte"}


def parse(p):
    kv = {}
    for line in open(p, errors="replace"):
        k, _, v = line.strip().partition("=")
        if not v:
            continue
        try:
            kv[k] = int(v)
        except ValueError:
            pass
    return kv


def sp(v):
    s = "%d" % round(v)
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    out = ""
    while len(s) > 3:
        out = " " + s[-3:] + out
        s = s[:-3]
    return ("-" if neg else "") + s + out


def pct(a, b):
    return 100.0 * (b - a) / a if a else 0.0


def main():
    data = {}
    missing = []
    for wl in WLS:
        for m in MAPS:
            for cell, _, _ in CELLS:
                p = os.path.join(DIR, "m24_%s_M%d_%s.txt" % (cell, m, wl))
                if not os.path.exists(p) or "cycles" not in parse(p):
                    missing.append((wl, m, cell))
                    continue
                data[(wl, m, cell)] = parse(p)
    if missing:
        print("note: %d cell(s) absent: %s" % (
            len(missing), ", ".join("%s M%d %s" % t for t in missing)), file=sys.stderr)
    # A mapping is analysed only where all four cells exist.
    full = [(wl, m) for wl in WLS for m in MAPS
            if all((wl, m, c) in data for c, _, _ in CELLS)]

    rows, ok = [], True

    print("== four-way cells (page ON = cohorts, DRAM ON = physical release order)")
    hdr = ("wl map cell p d |     cycles   vs-A%  vsSIMT |  reads  hit%  conf  act  rd/act"
           "  wait  qmax |  pool poolmx  vpn  cell  e/cl | orgR orgCyc | tr/cyc  admIdle walker")
    for wl in WLS:
        print("\n-- %s" % wl)
        print(hdr)
        base = data[(wl, 0, "A")]["cycles"]
        simt = data[(wl, 0, "A")].get("dram_client0_reads", 0)
        for m in MAPS:
            for cell, org, dorg in CELLS:
                if (wl, m) not in full:
                    continue
                d = data[(wl, m, cell)]
                c = d["cycles"]
                reads = d["dram_reads"]
                acts = d["dram_row_acts_sum"]
                pool = d["bulk_g6b_ready_occ_sum"] / c
                smp = max(1, d.get("bulk_g6i_samples", 0))
                print("%-11s M%d %s %d %d | %11s %7.2f %7s | %6s %5.2f %5s %5s %7.3f %5s %5s"
                      " | %5.1f %6s %4.1f %5.1f %5.2f | %4s %6s | %6.3f %7.1f%% %5s" % (
                          wl, m, cell, org, dorg, sp(c), pct(base, c), "-",
                          sp(reads), 100.0 * d["dram_row_hits"] / reads,
                          sp(d["dram_row_conflicts"]), sp(acts), reads / acts,
                          sp(d["dram_wait_sum"] / max(1, d["dram_svc_events"])),
                          sp(d["dram_max_q"]),
                          pool, sp(d["bulk_g6b_ready_peak"]),
                          d.get("bulk_g6i_vpn_sum", 0) / smp,
                          d.get("bulk_g6i_cells_sum", 0) / smp,
                          pool / max(1e-9, d.get("bulk_g6i_cells_sum", 1) / smp),
                          sp(d["bulk_org_runs"]), sp(d["bulk_org_cycles"]),
                          d["bulk_g6b_tr_completed"] / c,
                          100.0 * d.get("bulk_g6c_admit_idle_cycles", 0) / c,
                          d.get("walker_busy", 0)))
                rows.append({
                    "workload": wl, "map": "M%d" % m, "cell": cell,
                    "page_org": org, "dram_org": dorg,
                    "cycles": c,
                    "dcyc_vs_A_M0_pct": round(pct(base, c), 4),
                    "dram_reads": reads, "row_hit_pct": round(100.0 * d["dram_row_hits"] / reads, 4),
                    "conflicts": d["dram_row_conflicts"], "activations": acts,
                    "reads_per_act": round(reads / acts, 4),
                    "wait_mean": round(d["dram_wait_sum"] / max(1, d["dram_svc_events"]), 2),
                    "q_max": d["dram_max_q"], "bytes": d["dram_bytes"],
                    "pool_mean": round(pool, 2), "pool_peak": d["bulk_g6b_ready_peak"],
                    "pages_mean": round(d.get("bulk_g6i_vpn_sum", 0) / smp, 2),
                    "cells_mean": round(d.get("bulk_g6i_cells_sum", 0) / smp, 2),
                    "org_runs": d["bulk_org_runs"], "org_cycles": d["bulk_org_cycles"],
                    "rel_inv": d.get("bulk_m24_rel_inversions", 0),
                    "rel_pairs": d.get("bulk_m24_rel_pairs", 0),
                    "walks": d.get("walks", 0),
                    "tlb_hit_pct": round(100.0 * d.get("tlb0_hits", 0) /
                                         max(1, d.get("tlb0_reads", 0)), 4),
                    "l2tlb_hit_pct": round(100.0 * d.get("l2tlb_hits", 0) /
                                           max(1, d.get("l2tlb_reads", 0)), 4),
                    "adm_accepted": d.get("bulk_g6c_admit_accepted", 0),
                    "tr_reqs": d["bulk_g6b_tr_requests"],
                    "cohorts": d.get("bulk_g6c_cohorts_opened", 0),
                })

    # ---- factorial ------------------------------------------------------
    print("\n== factorial (cycles; savings are (A - X) / A, lower time is better)")
    print("wl map |        A        B        C        D | page%  dram%  comb% | incDram incPage | inter"
          "     | type")
    checks = []
    for wl in WLS:
        for m in MAPS:
            if (wl, m) not in full:
                continue
            A = data[(wl, m, "A")]["cycles"]
            B = data[(wl, m, "B")]["cycles"]
            C = data[(wl, m, "C")]["cycles"]
            D = data[(wl, m, "D")]["cycles"]
            inter = D - B - C + A
            page = 100.0 * (A - B) / A
            dram = 100.0 * (A - C) / A
            comb = 100.0 * (A - D) / A
            incd = 100.0 * (B - D) / B
            incp = 100.0 * (C - D) / C
            thr = 0.0005 * A   # 0.05 % of runtime: "no interaction" band
            kind = ("additive" if abs(inter) <= thr
                    else ("complementary" if inter < 0 else "antagonistic"))
            print("%-11s M%d | %8s %8s %8s %8s | %5.2f %6.2f %6.2f | %6.2f%% %6.2f%% | %9s %s" % (
                wl, m, sp(A), sp(B), sp(C), sp(D), page, dram, comb, incd, incp,
                sp(inter), kind))
            checks.append(("factorial", wl, "M%d" % m, A, B, C, D, inter))

    # ---- identities -----------------------------------------------------
    print("\n== checks")
    for wl in WLS:
        for m in MAPS:
            for cell, org, dorg in CELLS:
                if (wl, m, cell) not in data:
                    continue
                tag = "%s M%d %s" % (wl, m, cell)
                d = data[(wl, m, cell)]
                c = d["cycles"]
                problems = []
                if d["bulk_g6b_tr_requests"] != d["bulk_g6b_tr_completed"]:
                    problems.append("translated != completed")
                if d["bulk_g6b_runs_released"] != d["bulk_g6b_tr_completed"]:
                    problems.append("released != completed")
                if d["bulk_g6b_ready_peak"] > d["bulk_g6b_pa_capacity"]:
                    problems.append("pool over capacity")
                if d["conservation_ok"] != 1 or d["conservation_violations"]:
                    problems.append("conservation")
                if d["bulk_elems_done"] != d["bulk_elems_total"]:
                    problems.append("elements lost")
                if dorg and d["bulk_org_runs"] == 0:
                    problems.append("DRAM arm without organizer pass")
                if not dorg and d["bulk_org_runs"] != 0:
                    problems.append("neutral arm ran the organizer")
                if org == 8 and d.get("bulk_g6c_admit_accepted", 0) == 0:
                    problems.append("page arm admitted nothing")
                if org == 7 and d.get("bulk_g6c_admit_accepted", 0) != 0:
                    problems.append("in-order arm used the cohort path")
                if problems:
                    ok = False
                print("%-20s %-14s %s" % (tag, "PASS" if not problems else "FAIL",
                                          ", ".join(problems)))

    # ---- anchors --------------------------------------------------------
    print("\n== anchors")
    h = "build/wm/m23g6h/g6h_G0r_%s.txt"
    for wl in WLS:
        p = h % wl
        if not os.path.exists(p):
            continue
        ref = parse(p)
        d = data[(wl, 0, "D")]
        keys = sorted(k for k in ref if k in d)
        diff = [k for k in keys if ref[k] != d[k]]
        print("D M0 %-11s vs G6H G0: %d common keys, %d differ%s" % (
            wl, len(keys), len(diff), (" -> " + ", ".join(diff[:6])) if diff else ""))
        if diff:
            ok = False
    for wl in WLS:
        p = "build/wm/m23g6i/g6i_A1_%s.txt" % wl
        if not os.path.exists(p):
            continue
        ref = parse(p)
        d = data[(wl, 0, "D")]
        keys = sorted(k for k in ref if k in d)
        diff = [k for k in keys if ref[k] != d[k]]
        print("D M0 %-11s vs G6I A1: %d common keys, %d differ%s" % (
            wl, len(keys), len(diff), (" -> " + ", ".join(diff[:6])) if diff else ""))
        if diff:
            ok = False
    # p7/p8 reproducibility: cell C's order is org 7 with the shipped organizer.
    for wl in WLS:
        p = "build/wm/m23g6b/g6b_p7_%s.txt" % wl
        if not os.path.exists(p):
            continue
        print("note: historical G6B p7 %s exists (separate config set)" % wl)

    out = os.path.join(DIR, "summary.csv")
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print("\nwrote %s (%d cells)" % (out, len(rows)))
    print("%s" % ("ALL CHECKS PASSED" if ok else "FAILURES PRESENT"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
