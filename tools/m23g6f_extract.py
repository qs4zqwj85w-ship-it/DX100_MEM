#!/usr/bin/env python3
"""M23-G6F — full-buffer candidate visibility: cell extractor.

Reads `build/wm/m23g6f/g6f_<cell>_<workload>.txt` and prints the comparison used
in docs/m23-g6f-full-buffer-visibility.md, plus summary.csv.

Definitions that matter (per-workload F0 cells are the control):

  qmean    dram_q_occ_sum / dram_diag_decisions
           = mean requests in the DRAM queue at a scheduling decision. This is
             the population FR-FCFS actually chooses from in every cell.
  qcells   dram_vis_rows_sum / decisions = distinct (bank,row) cells in that queue.
  qhitC    dram_hit_cand_sum / decisions = queued requests sitting on an open row.
  b_ent    bulk_g6f_scan_entries / bulk_g6f_scan_calls
           = candidates compared per release decision (1 in F0 by construction:
             the release reads only the heap's minimum).
  b_cells  bulk_g6f_vis_cells_sum / scan_calls = distinct (bank,row) cells among
           the compared candidates.
  b_hitC   bulk_g6f_vis_hit_sum / scan_calls = compared candidates that sit on an
           already-open row -- the only ones visibility can turn into a hit now.
  pick%    bulk_g6f_pick_hit / scan_calls = decisions whose chosen candidate was
           such a candidate.
  dens     b_ent / b_cells = requests per candidate cell (buffer-side row density).
"""
import os
import sys

WORKLOADS = [
    ("orkut", "Orkut BFS (S1: 8/8/8)"),
    ("livejournal", "LiveJournal BFS (4/8/8)"),
    ("ogbntrain", "ogbn-products train (4/8/8)"),
]
CELLS = ["F0", "F1", "F2"]
VIS = {"F0": 0, "F1": 1024, "F2": 4096}
DIR = "build/wm/m23g6f"


def parse(path):
    kv = {}
    for line in open(path, errors="replace"):
        k, _, v = line.strip().partition("=")
        if v:
            try:
                kv[k] = int(v)
            except ValueError:
                pass
    return kv


def row(cell, kv, base):
    d = kv["dram_diag_decisions"]
    c = kv["cycles"]
    scans = kv.get("bulk_g6f_scan_calls", 0)
    return dict(
        cell=cell, vis=VIS[cell], cycles=c,
        dcyc=(100.0 * (c - base["cycles"]) / base["cycles"]) if base else 0.0,
        reads=kv["dram_reads"], hit=100.0 * kv["dram_row_hits"] / kv["dram_reads"],
        rpa=kv["dram_reads"] / kv["dram_row_acts_sum"], acts=kv["dram_row_acts_sum"],
        qmean=kv["dram_q_occ_sum"] / d, qcells=kv["dram_vis_rows_sum"] / d,
        qhitC=kv["dram_hit_cand_sum"] / d,
        b_ent=(kv["bulk_g6f_scan_entries"] / scans) if scans else 1.0,
        b_cells=(kv["bulk_g6f_vis_cells_sum"] / scans) if scans else 0.0,
        b_hitC=(kv["bulk_g6f_vis_hit_sum"] / scans) if scans else 0.0,
        pick=(100.0 * kv["bulk_g6f_pick_hit"] / scans) if scans else 0.0,
        dens=((kv["bulk_g6f_scan_entries"] / kv["bulk_g6f_vis_cells_sum"])
              if scans and kv["bulk_g6f_vis_cells_sum"] else 0.0),
        svc_hit=kv["dram_svc_hit_cycles"] / kv["dram_svc_events"],
        svc_miss=kv["dram_svc_miss_cycles"] / kv["dram_svc_events"],
        mshr=kv["l2_mshr_max"], allocs=kv["l2_mshr_allocs"],
        blocked=100.0 * kv["bulk_g6b_release_blocked_cycles"] / c,
        ready=kv["bulk_g6b_ready_occ_sum"] / c,
        pops=kv["bulk_g6b_release_pops"],
        case_b=kv["dram_stall_case_b"], case_c=kv["dram_stall_case_c"],
        missed=kv["dram_missed_hit"],
    )


def main():
    out_rows = []
    for wl, desc in WORKLOADS:
        cells = {}
        for c in CELLS:
            p = os.path.join(DIR, "g6f_%s_%s.txt" % (c, wl))
            if not os.path.exists(p) or os.path.getsize(p) == 0:
                print("missing %s" % p, file=sys.stderr)
                return 1
            cells[c] = parse(p)
        base = cells["F0"]
        rows = [row(c, cells[c], base) for c in CELLS]
        out_rows += [(wl, r) for r in rows]

        print("== %s  (%s)" % (wl, desc))
        print("cell vis      cycles   dCyc%   reads   hit%  reads/act    acts  qmean qcells qhitC "
              "| b_ent b_cells b_hitC  pick%  dens | svcH svcM mshr blk%")
        for r in rows:
            print("%-4s %-5d %10d %+7.3f %7d %6.2f %9.4f %7d %6.2f %6.1f %5.2f "
                  "| %5.0f %7.1f %6.3f %6.3f %5.2f | %4.2f %4.2f %4d %4.1f"
                  % (r["cell"], r["vis"], r["cycles"], r["dcyc"], r["reads"], r["hit"],
                     r["rpa"], r["acts"], r["qmean"], r["qcells"], r["qhitC"],
                     r["b_ent"], r["b_cells"], r["b_hitC"], r["pick"], r["dens"],
                     r["svc_hit"], r["svc_miss"], r["mshr"], r["blocked"]))
        print()

    out = os.path.join(DIR, "summary.csv")
    with open(out, "w") as f:
        f.write("workload,cell,sched_vis,cycles,dcyc_pct,dram_reads,row_hit_pct,reads_per_act,"
                "activations,qmean,qcells,qhitC,buf_entries,buf_cells,buf_hitC,pick_hit_pct,"
                "buf_density,svc_hit,svc_miss,l2_mshr_max,l2_mshr_allocs,release_blocked_pct,"
                "ready_avg,release_pops,case_b,case_c,missed_hit\n")
        for wl, r in out_rows:
            f.write("%s,%s,%d,%d,%.4f,%d,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%.1f,%.1f,%.4f,%.4f,"
                    "%.4f,%.4f,%.4f,%d,%d,%.4f,%.1f,%d,%d,%d,%d\n"
                    % (wl, r["cell"], r["vis"], r["cycles"], r["dcyc"], r["reads"], r["hit"],
                       r["rpa"], r["acts"], r["qmean"], r["qcells"], r["qhitC"], r["b_ent"],
                       r["b_cells"], r["b_hitC"], r["pick"], r["dens"], r["svc_hit"],
                       r["svc_miss"], r["mshr"], r["allocs"], r["blocked"], r["ready"],
                       r["pops"], r["case_b"], r["case_c"], r["missed"]))
    print("wrote %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
