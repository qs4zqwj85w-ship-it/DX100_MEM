#!/usr/bin/env python3
"""M23-G3 — emit the report tables as Markdown straight from summary.csv.

Tables are generated, never transcribed, so a number in the report cannot drift
from the cell log it came from. Cells that failed an accounting check are printed
as DNF with their failure, not as a metric.
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g3")
WLS = ["orkut", "ogbn_uniform", "criteo"]
WLN = {"orkut": "Orkut BFS", "ogbn_uniform": "ogbn-products uniform", "criteo": "Criteo 16K"}
POLS = ["P0", "P1", "P2", "P3", "P1-"]
# window-local ceiling from tools/m23g3_ceiling.py (analysis bound, not simulated)
CEIL = {("orkut", 8192): 56.12, ("orkut", 16384): 65.41, ("orkut", 32768): 73.64,
        ("ogbn_uniform", 8192): 36.52, ("ogbn_uniform", 16384): 56.26, ("ogbn_uniform", 32768): 74.81,
        ("criteo", 8192): 19.53, ("criteo", 16384): 30.13, ("criteo", 32768): 43.76}
OFF = {"orkut": 91.84, "ogbn_uniform": 98.85, "criteo": 83.91}


def load():
    with open(os.path.join(OUT, "summary.csv")) as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        for k in ("window", "cycles", "dram_commands", "row_hits", "row_conflicts",
                  "elems_done", "dedup_merged", "tgt_req_gen", "bulk_org_cycles",
                  "org_visits", "l2_hits", "win_util_pct", "mean_service",
                  "bytes_per_cycle", "gbs", "bank_busy_pct", "q_avg", "wait_avg",
                  "win_size_mean", "sched_defer", "walks", "dtlb_misses"):
            r[k] = float(r[k]) if r[k] not in ("", None) else None
        r["ok"] = r["checks_ok"] == "yes"
        for k in ("row_hit_pct", "speedup_vs_simt", "org_pct_of_cycles"):
            r[k] = float(r[k]) if r.get(k) not in (None, "", "None") else None
    return rows


def fmt(v, spec="{:,.0f}"):
    return "DNF" if v is None else spec.format(v)


def table_perf(rows, cfg):
    out = ["| Workload | Policy | W | Cycles | vs SIMT | Row hit | Conflicts | DRAM cmds "
           "| Service | B/cyc | Org cost | Win util |",
           "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for wl in WLS:
        for pol in POLS:
            for r in rows:
                if r["config"] != cfg or r["workload"] != wl or r["policy"] != pol:
                    continue
                if not r["ok"]:
                    out.append(f"| {WLN[wl]} | {pol} | {int(r['window'])//1024}K | DNF | — | — | — "
                               f"| — | — | — | — | — |")
                    continue
                out.append(
                    f"| {WLN[wl]} | {pol} | {int(r['window'])//1024}K | {r['cycles']:,.0f} "
                    f"| {r['speedup_vs_simt']:.3f}× | {r['row_hit_pct']:.2f}% "
                    f"| {r['row_conflicts']:,.0f} | {r['dram_commands']:,.0f} "
                    f"| {r['mean_service']:.2f} | {r['bytes_per_cycle']:.3f} "
                    f"| {r['org_pct_of_cycles']:.1f}% | {r['win_util_pct']:.1f}% |")
    return "\n".join(out)


def table_attr(rows):
    out = ["| Config | Workload | W | Policy | Δ cycles | Δ row hit | Δ conflicts "
           "| Δ DRAM cmds | Δ service | Δ L2 hits | Org visits |",
           "|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|"]
    with open(os.path.join(OUT, "attribution.csv")) as f:
        for a in csv.DictReader(f):
            if a["policy"] == "P1-" and a["config"] != "ideal":
                pass
            out.append(
                f"| {a['config']} | {WLN[a['workload']]} | {int(a['window'])//1024}K | {a['policy']} "
                f"| {float(a['d_cycles_pct']):+.2f}% | {float(a['d_row_hit_pts']):+.2f} pt "
                f"| {float(a['d_row_conflicts']):+,.0f} | {float(a['d_dram_commands']):+,.0f} "
                f"| {float(a['d_mean_service']):+.3f} | {float(a['d_l2_hits']):+,.0f} "
                f"| {float(a['d_org_visits']):+,.0f} |")
    return "\n".join(out)


def table_ceiling(rows):
    out = ["| Workload | W | Window ceiling | Whole-trace ceiling | P0 row hit | P2 row hit "
           "| P2 / ceiling |", "|---|---:|---:|---:|---:|---:|---:|"]
    for wl in WLS:
        for W in (8192, 16384, 32768):
            p0 = next((r for r in rows if r["config"] == "ideal" and r["workload"] == wl
                       and r["window"] == W and r["policy"] == "P0"), None)
            p2 = next((r for r in rows if r["config"] == "ideal" and r["workload"] == wl
                       and r["window"] == W and r["policy"] == "P2"), None)
            if not p0 or not p2 or not p0["ok"]:
                continue
            c = CEIL[(wl, W)]
            out.append(f"| {WLN[wl]} | {W//1024}K | {c:.2f}% | {OFF[wl]:.2f}% "
                       f"| {p0['row_hit_pct']:.2f}% | {p2['row_hit_pct']:.2f}% "
                       f"| {100.0 * p2['row_hit_pct'] / c:.0f}% |")
    return "\n".join(out)


def table_corr(rows):
    out = ["| Config | Workload | Policy | Elems | Target requests | Merged | L2 reads "
           "| L2 hits | DRAM cmds |", "|---|---|---|---:|---:|---:|---:|---:|---:|"]
    for cfg in ("ideal", "published"):
        for wl in WLS:
            for pol in ("P0", "P1-"):
                r = next((x for x in rows if x["config"] == cfg and x["workload"] == wl
                          and x["window"] == 16384 and x["policy"] == pol), None)
                if not r or not r["ok"]:
                    continue
                out.append(f"| {cfg} | {WLN[wl]} | {pol} | {r['elems_done']:,.0f} "
                           f"| {r['tgt_req_gen']:,.0f} | {r['dedup_merged']:,.0f} "
                           f"| {float(r['l2_reads']):,.0f} | {float(r['l2_hits']):,.0f} "
                           f"| {r['dram_commands']:,.0f} |")
    return "\n".join(out)


def main():
    rows = load()
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    if which in ("all", "pub"):
        print("<!-- published configuration -->")
        print(table_perf(rows, "published"))
        print()
    if which in ("all", "ideal"):
        print("<!-- ideal translation -->")
        print(table_perf(rows, "ideal"))
        print()
    if which in ("all", "attr"):
        print(table_attr(rows))
        print()
    if which in ("all", "ceiling"):
        print(table_ceiling(rows))
        print()
    if which in ("all", "corr"):
        print(table_corr(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
