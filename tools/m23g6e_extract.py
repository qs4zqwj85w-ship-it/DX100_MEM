#!/usr/bin/env python3
"""M23-G6E — DRAM scheduling visibility audit: cell extractor.

Reads the `key=value` logs produced by `build/wm/m23g6e_run.sh` and prints the
audit table used in docs/m23-g6e-dram-visibility-audit.md, plus a summary.csv.

Definitions (all ratios are per *decision*, i.e. per cycle in which the DRAM
queue was non-empty, `dram_diag_decisions`):

  q_mean_dec      dram_q_occ_sum / decisions
                  = mean number of requests in the DRAM queue at a decision.
  cells_dec       dram_vis_rows_sum / decisions
                  = mean number of DISTINCT (bank,row) cells present in the
                    queue at a decision. Not "available requests": two requests
                    to the same open row are one cell, and demand for a cell is a
                    different thing from an issue opportunity.
  banks_dec       dram_vis_banks_sum / decisions
                  = mean number of distinct banks represented in the queue.
  hitcand_dec     dram_hit_cand_sum / decisions
                  = mean number of queued requests whose (bank,row) equals that
                    bank's currently open row (in either bank).
  entries_per_cell q_mean_dec / cells_dec
                  = mean requests sharing a queue-resident cell. The DRAM
                    analogue of "run length": it bounds how many of the visible
                    requests can be served before the next activation.
  hit_share       dram_hit_cand_sum / dram_q_occ_sum
                  = fraction of queued requests that are row hits.
  visible_pct     q_mean_dec / ready_avg * 100
                  = fraction of translated-but-unissued runs that the DRAM
                    controller can see.
  residence_cyc   ready_avg / (pops / cycles)
                  = Little's-law residence time of a run in the ready buffer.
"""
import os
import sys

CELLS = [
    ("A0",    "G6C baseline   2/2/4",  "orkut"),
    ("S1",    "G6D winner     8/8/8",  "orkut"),
    ("DLJ",   "G6D winner     4/8/8",  "livejournal"),
    ("DOGBN", "G6D winner     4/8/8",  "ogbn-products"),
]
DIR = "build/wm/m23g6e"


def parse(path):
    kv = {}
    for line in open(path, errors="replace"):
        line = line.strip()
        if "=" not in line or line.startswith("#"):
            continue
        k, _, v = line.partition("=")
        if not all(ch.isalnum() or ch == "_" for ch in k):
            continue
        try:
            kv[k] = int(v)
        except ValueError:
            pass
    return kv


def bucket_pct(hist, q):
    """Percentile of a log2-bucket histogram: returns the inclusive upper edge."""
    tot, run = sum(hist), 0
    for b, h in enumerate(hist):
        run += h
        if run >= q * tot:
            return (1 << (b + 1)) - 1 if b else 1
    return (1 << len(hist)) - 1


def main():
    rows = []
    for cell, desc, wl in CELLS:
        p = os.path.join(DIR, "g6e_%s.txt" % cell)
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            print("missing %s" % p, file=sys.stderr)
            return 1
        kv = parse(p)
        c, d = kv["cycles"], kv["dram_diag_decisions"]
        q, cells = kv["dram_q_occ_sum"], kv["dram_vis_rows_sum"]
        pops, ready = kv["bulk_g6b_release_pops"], kv["bulk_g6b_ready_occ_sum"] / c
        wait = [kv.get("dram_wait_h%d" % i, 0) for i in range(16)]
        r = dict(
            cell=cell, desc=desc, wl=wl, cycles=c,
            reads=kv["dram_reads"], row_hit=100.0 * kv["dram_row_hits"] / kv["dram_reads"],
            decisions=d, decision_pct=100.0 * d / c,
            q_mean=q / d, q_max=kv["dram_max_q"],
            elig=kv["dram_elig_sum"] / d, cells=cells / d, banks=kv["dram_vis_banks_sum"] / d,
            hitcand=kv["dram_hit_cand_sum"] / d, cells_max=kv["dram_vis_rows_max"],
            epc=(q / d) / (cells / d), hit_share=100.0 * kv["dram_hit_cand_sum"] / q,
            pops=pops, pops_cyc=pops / c, rel_blocked=100.0 * kv["bulk_g6b_release_blocked_cycles"] / c,
            slot_stall=100.0 * kv["bulk_g6b_release_slot_stall"] / c,
            ready=ready, residence=ready / (pops / c),
            visible=100.0 * (q / d) / ready,
            wait_med=bucket_pct(wait, 0.5), wait_p95=bucket_pct(wait, 0.95), wait_max=kv["dram_wait_max"],
            tgt_reads=kv["dram_client3_reads"], tgt_hit=100.0 * kv["dram_client3_hits"] / kv["dram_client3_reads"],
            idx_reads=kv["dram_client1_reads"], idx_hit=100.0 * kv["dram_client1_hits"] / kv["dram_client1_reads"],
            conf=kv["dram_row_conflicts"], case_b=kv["dram_stall_case_b"], case_c=kv["dram_stall_case_c"],
            missed=kv["dram_missed_hit"],
            svc_hit=kv["dram_svc_hit_cycles"] / kv["dram_svc_events"],
            svc_miss=kv["dram_svc_miss_cycles"] / kv["dram_svc_events"],
            starved=100.0 * kv["dram_starved_cycles"] / c, blocked=100.0 * kv["dram_blocked_cycles"] / c,
            issuing=100.0 * kv["dram_svc_events"] / c, l2mshr=kv["l2_mshr_max"],
        )
        rows.append(r)

    hdr = ("cell   cycles      reads    hit%  dec%  qMean elig cells banks hitC e/cell hitSh%  "
           "pops/cyc blocked%  ready  resid  vis%  wMed  w95  tgt%  idx%")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        print("%-6s %9d %8d %6.2f %5.1f %6.2f %4.2f %5.2f %5.2f %5.2f %5.2f %6.1f "
              "%7.3f %7.1f %6.0f %6.0f %5.1f %5d %5d %5.1f %5.1f"
              % (r["cell"], r["cycles"], r["reads"], r["row_hit"], r["decision_pct"], r["q_mean"],
                 r["elig"], r["cells"], r["banks"], r["hitcand"], r["epc"], r["hit_share"],
                 r["pops_cyc"], r["rel_blocked"], r["ready"], r["residence"], r["visible"],
                 r["wait_med"], r["wait_p95"], r["tgt_hit"], r["idx_hit"]))

    print("\nconflict split   (case_b = hit queued but its bank busy, "
          "case_c = no hit in the queue at all)")
    for r in rows:
        b = 100.0 * r["case_b"] / r["conf"] if r["conf"] else 0.0
        print("  %-6s conflicts %8d  b %5.1f%%  c %5.1f%%  missed_hit %d  "
              "svc hit %.2f / miss %.2f cycles"
              % (r["cell"], r["conf"], b, 100.0 - b, r["missed"], r["svc_hit"], r["svc_miss"]))

    print("\nutilization      (share of cycles)")
    for r in rows:
        print("  %-6s issuing %5.1f%%  bank-busy-blocked %5.1f%%  starved %5.1f%%  "
              "l2_mshr_max %d  cells_max %d  q_max %d"
              % (r["cell"], r["issuing"], r["blocked"], r["starved"], r["l2mshr"],
                 r["cells_max"], r["q_max"]))

    out = os.path.join(DIR, "summary.csv")
    with open(out, "w") as f:
        f.write("cell,workload,cycles,dram_reads,row_hit_pct,decisions,decision_pct,"
                "q_mean_dec,q_max,elig_dec,cells_dec,banks_dec,hitcand_dec,cells_entries,"
                "hit_share_pct,cells_max,release_pops,pops_per_cycle,release_blocked_pct,"
                "release_slot_stall_pct,ready_avg,residence_cycles,visible_pct,wait_med_upper,"
                "wait_p95_upper,wait_max,tgt_reads,tgt_hit_pct,idx_reads,idx_hit_pct,"
                "conflicts,case_b,case_c,missed_hit,svc_hit_cycles,svc_miss_cycles,"
                "issuing_pct,blocked_pct,starved_pct,l2_mshr_max\n")
        for r in rows:
            f.write("%s,%s,%d,%d,%.4f,%d,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,"
                    "%d,%.6f,%.4f,%.4f,%.1f,%.1f,%.4f,%d,%d,%d,%d,%.4f,%d,%.4f,"
                    "%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%d\n"
                    % (r["cell"], r["wl"], r["cycles"], r["reads"], r["row_hit"], r["decisions"],
                       r["decision_pct"], r["q_mean"], r["q_max"], r["elig"], r["cells"],
                       r["banks"], r["hitcand"], r["epc"], r["hit_share"], r["cells_max"],
                       r["pops"], r["pops_cyc"], r["rel_blocked"], r["slot_stall"], r["ready"],
                       r["residence"], r["visible"], r["wait_med"], r["wait_p95"], r["wait_max"],
                       r["tgt_reads"], r["tgt_hit"], r["idx_reads"], r["idx_hit"], r["conf"],
                       r["case_b"], r["case_c"], r["missed"], r["svc_hit"], r["svc_miss"],
                       r["issuing"], r["blocked"], r["starved"], r["l2mshr"]))
    print("\nwrote %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
