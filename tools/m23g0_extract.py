#!/usr/bin/env python3
"""M23-G0 — extract the DRAM microbenchmark logs into one CSV.

Reads build/wm/m23g0/m23g0_<pattern>_k<K>.txt (written by m23g0_run.sh) and
emits build/wm/m23g0/summary.csv. Values are taken verbatim from the driver's
own counters; this script recomputes nothing except the derived columns it
labels as derived.
"""
import csv
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))      # DX100_MEM/tools
ROOT = os.path.dirname(HERE)                           # DX100_MEM
LOGDIR = os.path.join(ROOT, "build", "wm", "m23g0")
OUT = os.path.join(LOGDIR, "summary.csv")

FIELDS = [
    "cycles", "first_service_cycle", "last_service_cycle", "service_span_cycles",
    "steady_window_cycles", "steady_window_lines", "dram_reads", "dram_bytes",
    "dram_row_hits", "dram_row_conflicts", "dram_cold_activations",
    "dram_busy_cycles", "dram_max_q", "dram_q_occ_sum", "dram_blocked_cycles",
    "dram_starved_cycles", "dram_wait_sum", "dram_wait_max", "dram_rows_used",
    "dram_row_acts_max", "l1_mshr_max", "l2_mshr_max", "l2_reads", "l2_hits",
    "walks", "pte_fetches", "inflight_max",
    "lines_per_cycle", "bytes_per_cycle", "steady_lines_per_cycle",
    "steady_bytes_per_cycle", "bank_utilization", "row_hit_rate",
    "predicted_row_hit_rate", "avg_queue_depth", "avg_dram_queue_wait",
    "avg_request_latency", "gbs_at_400mhz", "guard_tripped",
]

COLUMNS = ["pattern", "outstanding"] + FIELDS


def parse(path):
    kv = {}
    with open(path) as f:
        for line in f:
            m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)=(\S+)$", line.strip())
            if m:
                kv[m.group(1)] = m.group(2)
    return kv


def main():
    rows = []
    for path in sorted(glob.glob(os.path.join(LOGDIR, "m23g0_?_k*.txt"))):
        base = os.path.basename(path)
        m = re.match(r"m23g0_([A-E])_k(\d+)\.txt", base)
        if not m:
            continue
        kv = parse(path)
        row = {"pattern": m.group(1), "outstanding": int(m.group(2)),
               "log": base}
        for f in FIELDS:
            row[f] = kv.get(f, "")
        rows.append(row)

    if not rows:
        print("no logs found in", LOGDIR, file=sys.stderr)
        return 1

    rows.sort(key=lambda r: (r["pattern"], r["outstanding"]))
    with open(OUT, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=COLUMNS + ["log"])
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print(f"wrote {OUT}: {len(rows)} cells")
    return 0


if __name__ == "__main__":
    sys.exit(main())
