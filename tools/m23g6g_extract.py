#!/usr/bin/env python3
"""M23-G6G — physical DRAM row organization: cell extractor.

Reads `build/wm/m23g6g/g6g_<cell>_<workload>.txt` and prints the comparison used
in docs/m23-g6g-physical-row-organization.md, plus summary.csv.

Definitions (the G0 cell of each workload is the control):

  reads/act  dram_reads / dram_row_acts_sum, i.e. how many reads one activation
             serves -- the quantity row organization is supposed to raise.
  hit%       dram_row_hits / dram_reads.
  sz         mean runs per *cell* in the ready buffer at the moment a group opens
             (bulk_g6g_group_size_sum / bulk_g6g_groups_opened).
  drain      mean runs actually released per group
             (bulk_g6g_group_drain_sum / bulk_g6g_groups_closed). G0 has no
             groups; its own cell-run length is `runs/open`
             (bulk_g6b_release_pops / bulk_sched_opens), which is the direct
             like-for-like comparison with `drain`.
  cont%      share of consecutive releases that stayed in the same cell.
             G0 prints 0 by construction (it has no group state); its contiguity
             is measured by `runs/open` instead: consecutive releases within one
             cell run are by definition same-cell.
  util%      dram_busy_cycles / (banks * cycles): DRAM bank utilisation.
"""
import os
import sys

WORKLOADS = [
    ("orkut", "Orkut BFS (S1: 8/8/8)", 2),
    ("livejournal", "LiveJournal BFS (4/8/8)", 2),
    ("ogbntrain", "ogbn-products train (4/8/8)", 2),
]
CELLS = ["G0", "G1", "G2"]
GRP = {"G0": 0, "G1": 1, "G2": 2}
DIR = "build/wm/m23g6g"


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


def row(cell, kv, base, banks):
    c = kv["cycles"]
    reads = kv["dram_reads"]
    d = kv.get("dram_diag_decisions", 0) or 1
    opened = kv.get("bulk_g6g_groups_opened", 0)
    closed = kv.get("bulk_g6g_groups_closed", 0)
    pops = kv.get("bulk_g6b_release_pops", 0)
    opens = kv.get("bulk_sched_opens", 0) or 1
    same = kv.get("bulk_g6g_same_cell", 0)
    sw = kv.get("bulk_g6g_switch_cell", 0)
    runs = kv.get("bulk_g6b_runs_released", 0) or 1
    return dict(
        cell=cell, grp=GRP[cell], cycles=c,
        dcyc=(100.0 * (c - base["cycles"]) / base["cycles"]) if base else 0.0,
        reads=reads, hit=100.0 * kv["dram_row_hits"] / reads,
        rpa=reads / kv["dram_row_acts_sum"], acts=kv["dram_row_acts_sum"],
        svc_hit=kv["dram_svc_hit_cycles"] / kv["dram_svc_events"],
        svc_miss=kv["dram_svc_miss_cycles"] / kv["dram_svc_events"],
        util=100.0 * kv["dram_busy_cycles"] / (banks * c),
        starved=100.0 * kv["dram_starved_cycles"] / c,
        qmean=kv["dram_q_occ_sum"] / d,
        qcells=kv["dram_vis_rows_sum"] / d,
        qhitC=kv["dram_hit_cand_sum"] / d,
        sz=(kv["bulk_g6g_group_size_sum"] / opened) if opened else 0.0,
        szmax=kv.get("bulk_g6g_group_size_max", 0),
        h16=kv.get("bulk_g6g_group_h5", 0),
        drain=(kv["bulk_g6g_group_drain_sum"] / closed) if closed else 0.0,
        run_per_open=pops / opens,
        cont=(100.0 * same / (same + sw)) if (same + sw) else 0.0,
        revisit=(100.0 * kv.get("bulk_g6g_revisit", 0) / opened) if opened else 0.0,
        bankalt=(100.0 * kv.get("bulk_g6g_bank_alt", 0) / runs),
        ready=kv["bulk_g6b_ready_occ_sum"] / c,
        wait=kv["bulk_g6b_tr_wait_dram"] / runs,
        waitmax=kv["bulk_g6b_tr_wait_max"],
        blocked=100.0 * kv["bulk_g6b_release_blocked_cycles"] / c,
        l2hit=100.0 * kv["cc3_l2_hits"] / kv["cc3_l2_reads"],
        l2r=kv["cc3_l2_reads"], l2m=kv["cc3_l2_misses"],
        mshr=kv["l2_mshr_max"], qmax=kv["dram_max_q"], qfull=kv["dram_q_full"],
        missed=kv["dram_missed_hit"],
        gen=kv["bulk_tgt_req_gen"], iss=kv["bulk_tgt_req_issued"],
        done=kv["bulk_tgt_req_done"], c3i=kv["cc3_issue"], c3d=kv["cc3_done"],
        mismatch=kv.get("bulk_g6g_sel_mismatch", 0),
        scans=kv.get("bulk_g6g_scan_calls", 0),
        scan_ent=(kv["bulk_g6g_scan_entries"] / kv["bulk_g6g_scan_calls"])
        if kv.get("bulk_g6g_scan_calls") else 0.0,
    )


HEAD = ("cell cfg     cycles   dCyc%   reads   hit%  reads/act    acts  util% "
        "qmean qcells qhitC | sz  szmax h16  drain  r/o cont% rev%  balt% "
        "| ready  wait wmax | l2hit%")
FMT = ("%-4s %-3d %10d %+7.3f %7d %6.2f %9.4f %7d %6.2f %6.2f %6.1f %5.2f "
       "| %4.2f %5d %5d %6.2f %4.2f %5.1f %4.1f %5.1f | %5.1f %5.0f %6d | %5.2f")


def main():
    out_rows = []
    for wl, desc, banks in WORKLOADS:
        cells = {}
        for c in CELLS:
            p = os.path.join(DIR, "g6g_%s_%s.txt" % (c, wl))
            if not os.path.exists(p) or os.path.getsize(p) == 0:
                print("missing %s" % p, file=sys.stderr)
                cells = None
                break
            cells[c] = parse(p)
        if not cells:
            continue
        base = cells["G0"]
        rows = [row(c, cells[c], base, banks) for c in CELLS]
        out_rows += [(wl, r) for r in rows]
        print("== %s  (%s)" % (wl, desc))
        print(HEAD)
        for r in rows:
            print(FMT % (r["cell"], r["grp"], r["cycles"], r["dcyc"], r["reads"], r["hit"],
                         r["rpa"], r["acts"], r["util"], r["qmean"], r["qcells"], r["qhitC"],
                         r["sz"], r["szmax"], r["h16"], r["drain"], r["run_per_open"],
                         r["cont"], r["revisit"], r["bankalt"], r["ready"], r["wait"],
                         r["waitmax"], r["l2hit"]))
        print()

    if not out_rows:
        return 1
    out = os.path.join(DIR, "summary.csv")
    with open(out, "w") as f:
        f.write("workload,cell,sched_group,cycles,dcyc_pct,dram_reads,row_hit_pct,"
                "reads_per_act,activations,svc_hit,svc_miss,dram_util_pct,starved_pct,"
                "qmean,qcells,qhitC,group_size_mean,group_size_max,groups_full16,"
                "group_drain_mean,runs_per_cell_run,contiguous_pct,revisit_pct,"
                "bank_alt_pct,ready_avg,tr_wait_mean,tr_wait_max,release_blocked_pct,"
                "l2_hit_pct,l2_reads,l2_mshr_max,dram_max_q,dram_q_full,missed_hit,"
                "tgt_gen,tgt_issued,tgt_done,cc3_issue,cc3_done,sel_mismatch,"
                "scan_calls,scan_entries_mean\n")
        for wl, r in out_rows:
            f.write("%s,%s,%d,%d,%.4f,%d,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%.4f,"
                    "%.4f,%.4f,%.4f,%.4f,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,"
                    "%.1f,%.1f,%d,%.4f,%.4f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.1f\n"
                    % (wl, r["cell"], r["grp"], r["cycles"], r["dcyc"], r["reads"],
                       r["hit"], r["rpa"], r["acts"], r["svc_hit"], r["svc_miss"],
                       r["util"], r["starved"], r["qmean"], r["qcells"], r["qhitC"],
                       r["sz"], r["szmax"], r["h16"], r["drain"], r["run_per_open"],
                       r["cont"], r["revisit"], r["bankalt"], r["ready"], r["wait"],
                       r["waitmax"], r["blocked"], r["l2hit"], r["l2r"], r["mshr"],
                       r["qmax"], r["qfull"], r["missed"], r["gen"], r["iss"],
                       r["done"], r["c3i"], r["c3d"], r["mismatch"], r["scans"],
                       r["scan_ent"]))
    print("wrote %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
