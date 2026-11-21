#!/usr/bin/env python3
"""M23-G6H — complete-eligible-window audit: cell extractor.

Reads `build/wm/m23g6h/g6h_<cell>_<workload>.txt` (G0r, P2F) and the frozen
M23-G6G arm-1 logs (`build/wm/m23g6g/g6g_G1_<workload>.txt`), prints the
comparison used in docs/m23-g6h-full-window-row-organization.md, and writes
build/wm/m23g6h/summary.csv.

Visibility fields, all from the complete-enumeration pass in `pick_group`:

  cap        configured ready-buffer capacity (bulk_g6b_capacity)
  pop        entries examined per organization decision, mean / max
             (bulk_g6h_exam_sum / decisions, bulk_g6h_exam_max)
  elig       live translated entries per decision, mean / max
  stale      released heap entries the scan skipped, mean
  untr       window runs still in translation per decision, mean / max
  grp        distinct (bank,row) cells visible per decision, mean / max
  req/grp    eligible entries per visible cell (elig_sum / groups_sum)
  defer%     eligible entries left for a later decision (deferred_sum / elig_sum)
  inv        bulk_g6h_inv_viol -- must be 0

Definitions of the DRAM fields are the same as tools/m23g6g_extract.py:
  reads/act  dram_reads / dram_row_acts_sum
  r/o        releases per cell run (bulk_g6b_release_pops / bulk_sched_opens)
"""
import os
import sys

WORKLOADS = [
    ("orkut", "Orkut BFS (S1: 8/8/8)", 2),
    ("livejournal", "LiveJournal BFS (4/8/8)", 2),
    ("ogbntrain", "ogbn-products train (4/8/8)", 2),
]
# cell -> (directory, filename prefix, sched_group)
CELLS = [
    ("G0", "build/wm/m23g6h", "g6h_G0r_", 0),
    ("G1", "build/wm/m23g6h", "g6h_G1r_", 1),
    ("P2F", "build/wm/m23g6h", "g6h_P2F_", 3),
]
DIR = "build/wm/m23g6h"
G3_ANCHORS = [
    # (label, reproduced, historical)
    ("orkut w16384 p2", "build/wm/m23g6h/g6h_g3_p2_orkut_w16384.txt",
     "build/wm/m23g3/g3_orkut_w16384_p2.txt"),
]
SKIP_DIFF = ("bulk_g6h_", "sched_group", "bulk_g6g_")
# The G6G arms rerun on the G6H binary must reproduce the frozen G6G logs; the
# only permitted difference is the new G6H counter block.
REPRO = [
    ("G0r", "build/wm/m23g6h/g6h_G0r_%s.txt", "build/wm/m23g6g/g6g_G0_%s.txt"),
    ("G1r", "build/wm/m23g6h/g6h_G1r_%s.txt", "build/wm/m23g6g/g6g_G1_%s.txt"),
]


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


def row(cell, grp, kv, base, banks):
    c = kv["cycles"]
    reads = kv["dram_reads"]
    d = kv.get("dram_diag_decisions", 0) or 1
    dec = kv.get("bulk_g6h_decisions", 0)
    pops = kv.get("bulk_g6b_release_pops", 0)
    opens = kv.get("bulk_sched_opens", 0) or 1
    opened = kv.get("bulk_g6g_groups_opened", 0)
    gsum = kv.get("bulk_g6h_groups_sum", 0)
    esum = kv.get("bulk_g6h_elig_sum", 0)
    return dict(
        cell=cell, grp=grp, cycles=c,
        dcyc=(100.0 * (c - base["cycles"]) / base["cycles"]) if base else 0.0,
        reads=reads, hit=100.0 * kv["dram_row_hits"] / reads,
        rpa=reads / kv["dram_row_acts_sum"], acts=kv["dram_row_acts_sum"],
        svc_hit=kv["dram_svc_hit_cycles"] / kv["dram_svc_events"],
        svc_miss=kv["dram_svc_miss_cycles"] / kv["dram_svc_events"],
        util=100.0 * kv["dram_busy_cycles"] / (banks * c),
        qmean=kv["dram_q_occ_sum"] / d,
        cap=kv.get("bulk_g6b_pa_capacity", 0),
        pop=(kv["bulk_g6h_exam_sum"] / dec) if dec else 0.0,
        popmax=kv.get("bulk_g6h_exam_max", 0),
        elig=(esum / dec) if dec else 0.0,
        eligmax=kv.get("bulk_g6h_elig_max", 0),
        stale=(kv.get("bulk_g6h_stale_sum", 0) / dec) if dec else 0.0,
        untr=(kv.get("bulk_g6h_untr_sum", 0) / dec) if dec else 0.0,
        untrmax=kv.get("bulk_g6h_untr_max", 0),
        grpavg=(gsum / dec) if dec else 0.0,
        grpmax=kv.get("bulk_g6h_groups_max", 0),
        rpg=(esum / gsum) if gsum else 0.0,
        defer=(100.0 * kv.get("bulk_g6h_deferred_sum", 0) / esum) if esum else 0.0,
        inv=kv.get("bulk_g6h_inv_viol", 0),
        dec=dec,
        sz=(kv.get("bulk_g6g_group_size_sum", 0) / opened) if opened else 0.0,
        szmax=kv.get("bulk_g6g_group_size_max", 0),
        drain=(kv.get("bulk_g6g_group_drain_sum", 0) /
               kv["bulk_g6g_groups_closed"]) if kv.get("bulk_g6g_groups_closed") else 0.0,
        run_per_open=pops / opens,
        same=(100.0 * kv.get("bulk_g6g_same_cell", 0) /
              max(1, kv.get("bulk_g6g_same_cell", 0) + kv.get("bulk_g6g_switch_cell", 0))),
        bankalt=(100.0 * kv.get("bulk_g6g_bank_alt", 0) /
                 max(1, kv.get("bulk_g6b_runs_released", 0))),
        ready=kv["bulk_g6b_ready_occ_sum"] / c,
        readymax=kv["bulk_g6b_ready_peak"],
        wait=kv["bulk_g6b_tr_wait_dram"] / max(1, kv["bulk_g6b_runs_released"]),
        waitmax=kv["bulk_g6b_tr_wait_max"],
        l2hit=100.0 * kv["cc3_l2_hits"] / kv["cc3_l2_reads"],
        l2r=kv["cc3_l2_reads"], l2m=kv["cc3_l2_misses"],
        mshr=kv["l2_mshr_max"], qmax=kv["dram_max_q"], qfull=kv["dram_q_full"],
        missed=kv["dram_missed_hit"],
        gen=kv["bulk_tgt_req_gen"], iss=kv["bulk_tgt_req_issued"],
        done=kv["bulk_tgt_req_done"], c3i=kv["cc3_issue"], c3d=kv["cc3_done"],
        mismatch=kv.get("bulk_g6g_sel_mismatch", 0),
        cons=kv.get("conservation_ok", 0), viol=kv.get("conservation_violations", -1),
        elems=kv.get("bulk_elems_done", 0), total=kv.get("bulk_elems_total", 0),
        resviol=kv.get("bulk_residency_violations", 0),
        dtlbm=kv.get("dtlb_misses", 0), walks=kv.get("walks", 0),
    )


HEAD = ("cell grp      cycles   dCyc%   reads   hit%  reads/act    acts  util% | "
        "cap  pop popmax elig eMax untr  grp gMax req/gp defer% inv | "
        "r/o sz  drain same% balt% | ready rMax")
FMT = ("%-4s %3d %11d %+7.3f %7d %6.2f %9.4f %7d %6.2f | "
       "%4d %5.1f %6d %4.1f %4d %4.1f %4.1f %4d %6.2f %5.1f %3d | "
       "%4.2f %4.2f %6.2f %5.1f %5.1f | %5.1f %5d")


def main():
    out_rows = []
    ok = True
    for wl, desc, banks in WORKLOADS:
        cells = {}
        for name, d, pre, grp in CELLS:
            p = os.path.join(d, "%s%s.txt" % (pre, wl))
            if not os.path.exists(p) or os.path.getsize(p) == 0:
                print("missing %s" % p, file=sys.stderr)
                cells = None
                break
            cells[name] = parse(p)
        if not cells:
            ok = False
            continue
        base = cells["G0"]
        print("== %s  (%s)" % (wl, desc))
        print(HEAD)
        for name, _, _, grp in CELLS:
            r = row(name, grp, cells[name], base, banks)
            out_rows.append((wl, r))
            print(FMT % (r["cell"], r["grp"], r["cycles"], r["dcyc"], r["reads"],
                         r["hit"], r["rpa"], r["acts"], r["util"], r["cap"],
                         r["pop"], r["popmax"], r["elig"], r["eligmax"],
                         r["untr"], r["grpavg"], r["grpmax"], r["rpg"],
                         r["defer"], r["inv"], r["run_per_open"], r["sz"],
                         r["drain"], r["same"], r["bankalt"], r["ready"],
                         r["readymax"]))
        print()

    if out_rows:
        out = os.path.join(DIR, "summary.csv")
        cols = [(k, ("%d" if isinstance(v, int) else "%.4f"))
                for k, v in out_rows[0][1].items() if not isinstance(v, str)]
        with open(out, "w") as f:
            f.write("workload,cell," + ",".join(c for c, _ in cols) + "\n")
            for wl, r in out_rows:
                f.write(wl + "," + r["cell"] + "," +
                        ",".join(fmt % r[k] for k, fmt in cols) + "\n")
        print("wrote %s" % out)

    # ---- G6G reproduction: every key but the new counter block must match -----
    for label, rep, hist in REPRO:
        for wl, _, _ in WORKLOADS:
            rp, hp = rep % wl, hist % wl
            if not (os.path.exists(rp) and os.path.exists(hp)):
                print("repro %s/%s: missing cell" % (label, wl), file=sys.stderr)
                ok = False
                continue
            a, b = parse(rp), parse(hp)
            keys = sorted(k for k in a if not k.startswith(SKIP_DIFF))
            diff = [(k, b.get(k), a[k]) for k in keys if b.get(k) != a[k]]
            print("== repro %s/%s: %d keys compared, %d differ%s"
                  % (label, wl, len(keys), len(diff),
                     ", only in the frozen log: " + ",".join(x for x in b if x not in a)
                     if any(x not in a for x in b) else ""))
            for k, h, r in diff[:10]:
                print("   %-28s frozen=%s rerun=%s" % (k, h, r))
            if diff:
                ok = False

    # ---- G3 P2 anchor: reproduced vs historical, key by key -------------------
    for label, rep, hist in G3_ANCHORS:
        if not (os.path.exists(rep) and os.path.exists(hist)):
            print("anchor %s: missing cell" % label, file=sys.stderr)
            ok = False
            continue
        a, b = parse(rep), parse(hist)
        # Only keys the historical log actually carries are comparable: the 176
        # counters added after M23-G3 (bulk_g6a_*/g6b_*/g6c_*/g6f_*/g6g_*/g6h_*)
        # do not exist there and must not be counted as differences.
        both = sorted(set(a) & set(b))
        diff = [(k, b[k], a[k]) for k in both if a[k] != b[k]]
        extra = sorted(set(a) - set(b))
        old = sorted(set(b) - set(a))
        print("== G3 anchor %s: %d keys present in both, %d differ; "
              "%d keys only in the rerun, %d only in the historical log"
              % (label, len(both), len(diff), len(extra), len(old)))
        for k, h, r in diff[:20]:
            print("   %-28s historical=%s reproduced=%s" % (k, h, r))
        if diff or old:
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
