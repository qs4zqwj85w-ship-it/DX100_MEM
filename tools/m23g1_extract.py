#!/usr/bin/env python3
"""M23-G1 — extract the request-generation logs into one CSV.

Reads build/wm/m23g1/*.txt (written by build/wm/m23g1_run.sh) and emits
build/wm/m23g1/summary.csv with, per cell: the configuration actually applied
(parsed from the simulator's own configuration echo, not from the file name) and
every rate the report uses. Nothing is recomputed that the simulator reports;
the derived columns are labelled in the header comment below.
"""
import csv
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))      # DX100_MEM/tools
ROOT = os.path.dirname(HERE)                           # DX100_MEM
LOGDIR = os.path.join(ROOT, "build", "wm", "m23g1")
OUT = os.path.join(LOGDIR, "summary.csv")

# derived columns:
#   *_per_cycle  = corresponding counter / cycles
#   row_hit_rate = dram_row_hits / dram_reads
#   inflight_avg = inflight_occ / cycles      (mean outstanding requests)
#   q_wait       = dram_wait_sum / dram_reads (mean cycles in the DRAM queue)
CFG_FIELDS = ["R", "warps", "lanes", "l1_mshr", "l2_mshr", "lsu_pending",
              "ideal_translation", "dtlb_mshr", "dtlb_parked", "l2tlb_mshr"]
CNT_FIELDS = [
    "cycles", "trace_accesses", "trace_instr", "trace_lines",
    "instr_issued", "requests_generated", "issued", "completed",
    "slot_allocs", "unissued_max", "simt_peak_slot", "simt_staging_max",
    "simt_slot_full_cycles", "simt_issue_stalls",
    "l1_reads", "l1_hits", "l1_mshr_max", "l1_stall_cycles",
    "l2_reads", "l2_hits", "l2_mshr_max", "l2_stall_cycles",
    "tlb0_reads", "tlb0_hits", "tlb0_misses", "tlb0_mshr_max",
    "walks", "pte_fetches", "replays",
    "dram_reads", "dram_bytes", "dram_row_hits", "dram_row_conflicts",
    "dram_max_q", "dram_q_occ_sum", "dram_wait_sum", "dram_wait_max",
    "dram_svc_events", "dram_elig_sum", "dram_blocked_cycles",
    "dram_starved_cycles", "dram_busy_cycles",
    "inflight_occ", "inflight_max", "lat_mean_x100",
]


def parse_cfg(lines):
    """Pull the parameters we report out of Config::to_string()'s echo."""
    cfg = {k: "" for k in CFG_FIELDS}
    for line in lines:
        if line.startswith("cores="):
            for key, pat in (("R", r"\bR=(\d+)"), ("warps", r"\bwarps=(\d+)"),
                             ("lanes", r"\blanes=(\d+)")):
                m = re.search(pat, line)
                if m:
                    cfg[key] = m.group(1)
        elif line.startswith("l1="):
            m = re.search(r"mshr=(\d+)", line)
            if m:
                cfg["l1_mshr"] = m.group(1)
        elif line.startswith("l2="):
            m = re.search(r"mshr=(\d+)", line)
            if m:
                cfg["l2_mshr"] = m.group(1)
        elif line.startswith("dtlb="):
            m = re.search(r"mshr=(\d+) parked=(\d+)", line)
            if m:
                cfg["dtlb_mshr"], cfg["dtlb_parked"] = m.group(1), m.group(2)
        elif line.startswith("l2tlb="):
            m = re.search(r"mshr=(\d+)", line)
            if m:
                cfg["l2tlb_mshr"] = m.group(1)
        elif line.startswith("walkers="):
            cfg["ideal_translation"] = 1 if "ideal_translation" in line else 0
        elif line.startswith("dram="):
            m = re.search(r"lsu_pending=(\d+)", line)
            if m:
                cfg["lsu_pending"] = m.group(1)
    return cfg


def main():
    rows = []
    for path in sorted(glob.glob(os.path.join(LOGDIR, "g1_*.txt"))):
        text = open(path).read().splitlines()
        cfg = parse_cfg(text)
        cnt = {}
        for line in text:
            m = re.match(r"^([a-z0-9_]+)=(\d+)$", line.strip())
            if m:
                cnt[m.group(1)] = int(m.group(2))
        name = os.path.basename(path)[:-4]
        pat = re.match(r"g1_([A-F])_", name)
        cyc = cnt.get("cycles", 0) or 1

        def per(key):
            return cnt.get(key, 0) / cyc

        row = {
            "cell": name,
            "pattern": pat.group(1) if pat else "",
        }
        row.update(cfg)
        for f in CNT_FIELDS:
            row[f] = cnt.get(f, "")
        row["instr_per_cycle"] = per("instr_issued")
        row["accesses_per_cycle"] = per("requests_generated")
        row["issued_per_cycle"] = per("issued")
        row["l1_per_cycle"] = per("l1_reads")
        row["l2_per_cycle"] = per("l2_reads")
        row["dram_per_cycle"] = per("dram_reads")
        row["bytes_per_cycle"] = per("dram_bytes")
        row["inflight_avg"] = per("inflight_occ")
        row["row_hit_rate"] = (cnt.get("dram_row_hits", 0) / cnt["dram_reads"]
                               if cnt.get("dram_reads") else 0.0)
        row["q_wait"] = (cnt.get("dram_wait_sum", 0) / cnt["dram_reads"]
                         if cnt.get("dram_reads") else 0.0)
        row["lat_mean"] = cnt.get("lat_mean_x100", 0) / 100.0
        row["bytes_per_dram_read"] = (cnt["dram_bytes"] / cnt["dram_reads"]
                                      if cnt.get("dram_reads") else 0.0)
        row["cand_per_decision"] = (cnt["dram_elig_sum"] / cnt["dram_svc_events"]
                                    if cnt.get("dram_svc_events") else 0.0)
        row["bank_util"] = (cnt.get("dram_busy_cycles", 0) / (2 * cyc))
        row["queue_avg"] = cnt.get("dram_q_occ_sum", 0) / cyc
        rows.append(row)

    if not rows:
        print("no logs found in", LOGDIR, file=sys.stderr)
        return 1

    cols = (["cell", "pattern"] + CFG_FIELDS + CNT_FIELDS +
            ["instr_per_cycle", "accesses_per_cycle", "issued_per_cycle",
             "l1_per_cycle", "l2_per_cycle", "dram_per_cycle", "bytes_per_cycle",
             "inflight_avg", "row_hit_rate", "q_wait", "lat_mean",
             "bytes_per_dram_read", "cand_per_decision", "bank_util", "queue_avg"])
    with open(OUT, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in cols})
    print(f"wrote {OUT}: {len(rows)} cells")
    return 0


if __name__ == "__main__":
    sys.exit(main())
