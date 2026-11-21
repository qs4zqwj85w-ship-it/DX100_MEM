#!/usr/bin/env python3
"""M23-G2C — DRAM queue starvation test: extract and classify the two cells.

Reads build/wm/m23g2c/g2c_<workload>.txt, checks the accounting, writes
summary.csv, checks.txt and queue_hist.csv.

The DRAM decision (sim/src/memory.cpp dram_tick) partitions every cycle into
exactly one of three states:

  starved   queue empty at the decision            -> nothing to serve
  blocked   queue non-empty, no candidate's bank free -> waiting for a bank
  issued    one command started that cycle          -> service

`elig_wait_cycles` is the fourth, structurally impossible state (eligible
candidate present, still nothing issued); the M23-G2C guard counts it so the
partition can be asserted rather than assumed.

The tick at cycle 0 happens before any request exists and the loop breaks with
the cycle counter one past the last ticked cycle, so
starved + blocked + issued == cycles + 1 in every cell; that is checked.
"""
import csv
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g2c")

WORKLOADS = [
    ("orkut", "Orkut BFS frontier (max-degree seed, 131 072-vertex induced subgraph)",
     "build/wm/m23_orkut_bfs.bin"),
    ("ogbn", "ogbn-products train minibatch gather (8 455 targets, 16 of 100 features)",
     "build/wm/m23_ogbn.bin"),
]
BANKS = 2
CLK_HZ = 400e6


def parse(path):
    d = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("=") or line.startswith("--"):
                continue
            for tok in line.split():
                if "=" in tok:
                    k, _, v = tok.partition("=")
                    d.setdefault(k, v)
    return d


def i64(d, k):
    return int(d[k]) if k in d else 0


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def pct(a, b):
    return round(100.0 * a / b, 4) if b else 0.0


def main():
    rows, hists = [], []
    for wl, desc, trace in WORKLOADS:
        d = parse(os.path.join(OUT, f"g2c_{wl}.txt"))
        cycles = i64(d, "cycles")
        arrivals = i64(d, "dram_arrivals")
        arrive_cycles = i64(d, "dram_arrive_cycles")
        arrive_multi = i64(d, "dram_arrive_multi_cycles")
        cmds = i64(d, "dram_svc_events")
        starved = i64(d, "dram_starved_cycles")
        blocked = i64(d, "dram_blocked_cycles")
        elig_wait = i64(d, "dram_elig_wait_cycles")
        q_sum = i64(d, "dram_q_occ_sum")
        q_max = i64(d, "dram_max_q")
        wait_sum = i64(d, "dram_wait_sum")
        wait_max = i64(d, "dram_wait_max")
        busy = i64(d, "dram_busy_cycles")
        rows_h = i64(d, "dram_row_hits")
        confs = i64(d, "dram_row_conflicts")
        svc_cyc = i64(d, "dram_svc_hit_cycles") + i64(d, "dram_svc_miss_cycles")
        dram_bytes = i64(d, "dram_bytes")
        slot_full = i64(d, "simt_slot_full_cycles")
        l2_refus = i64(d, "cc0_l2_stalls")
        l2_max = i64(d, "l2_mshr_max")
        l1_max = i64(d, "l1_mshr_max")
        infl_max = i64(d, "inflight_max")
        cons = i64(d, "conservation_ok")
        viol = i64(d, "conservation_violations")
        completed = i64(d, "completed")
        acc = i64(d, "accesses")

        mean_svc = svc_cyc / max(1, cmds)
        capacity = BANKS / mean_svc
        wait_avg = wait_sum / max(1, cmds)
        little = q_sum / max(1, cmds)     # L/lambda == average time in queue

        checks = {
            "conservation_ok": cons == 1 and viol == 0,
            "completed_equals_request_stream": completed == i64(d, "requests_generated"),
            "bytes_are_64_per_command": dram_bytes == 64 * i64(d, "dram_reads"),
            "arrivals_equal_commands": arrivals == cmds == i64(d, "dram_reads"),
            "cycle_partition": starved + blocked + cmds == cycles + 1,
            "no_eligible_but_unissued": elig_wait == 0,
            "arrival_cycles_within_run": arrive_cycles <= cycles + 1,
        }
        notes = [k for k, v in checks.items() if not v]

        rows.append({
            "workload": wl,
            "workload_detail": desc,
            "trace": trace,
            "trace_sha256": sha256(os.path.join(ROOT, trace)),
            "l2_mshr": 64,
            "cycles": cycles,
            "dram_commands": cmds,
            "dram_bytes": dram_bytes,
            "bytes_per_cycle": round(dram_bytes / cycles, 4),
            "arrivals": arrivals,
            "arrivals_per_cycle": round(arrivals / cycles, 6),
            "arrive_cycles": arrive_cycles,
            "arrive_cycles_pct": pct(arrive_cycles, cycles),
            "arrive_multi_cycles": arrive_multi,
            "arrive_multi_pct": pct(arrive_multi, cycles),
            "completions": cmds,
            "completions_per_cycle": round(cmds / cycles, 6),
            "q_avg": round(q_sum / cycles, 3),
            "q_max": q_max,
            "empty_pct": pct(starved, cycles),
            "blocked_pct": pct(blocked, cycles),
            "issued_pct": pct(cmds, cycles),
            "elig_wait_pct": pct(elig_wait, cycles),
            "bank_busy_pct": pct(busy, BANKS * cycles),
            "wait_avg": round(wait_avg, 2),
            "wait_max": wait_max,
            "little_law_wait": round(little, 2),
            "row_hits": rows_h,
            "row_conflicts": confs,
            "row_hit_pct": pct(rows_h, rows_h + confs),
            "mean_service_cycles": round(mean_svc, 3),
            "capacity_cmds_per_cycle": round(capacity, 6),
            "arrivals_vs_capacity_pct": round(100.0 * (arrivals / cycles) / capacity, 2),
            "upstream_slot_full_cycles": slot_full,
            "upstream_slot_full_pct": pct(slot_full, cycles),
            # `cc0_l2_stalls` counts refusal *events*: a refused request is
            # retried every cycle, so it is not a count of distinct cycles and
            # must not be read as a percentage of them (M23-G1/G2A).
            "upstream_l2_refusals": l2_refus,
            "upstream_l2_refusals_per_cycle": round(l2_refus / cycles, 3),
            "l1_mshr_max": l1_max,
            "l2_mshr_max": l2_max,
            "inflight_max": infl_max,
            "checks_ok": "yes" if not notes else "NO",
            "notes": ";".join(notes),
        })

        for b in range(16):
            hists.append({
                "workload": wl,
                "bucket": f"2^{b}" if b < 15 else "2^15+",
                # log2_bucket(0) == log2_bucket(1) == 0 (memory.cpp:30), so the
                # first bucket holds depths 0 and 1; the rest are [2^b, 2^(b+1)).
                "depth_range": ("0..1" if b == 0 else
                                (f"{1 << b}..{(1 << (b + 1)) - 1}" if b < 15
                                 else f"{1 << b}+")),
                "cycles": i64(d, f"dram_q_h{b}"),
                "pct": pct(i64(d, f"dram_q_h{b}"), cycles),
            })

    fields = list(rows[0].keys())
    with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    with open(os.path.join(OUT, "queue_hist.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(hists[0].keys()))
        w.writeheader()
        w.writerows(hists)

    with open(os.path.join(OUT, "checks.txt"), "w") as f:
        f.write("# M23-G2C accounting\n")
        for r in rows:
            f.write(f"{r['workload']:6s} {r['checks_ok']:3s}  {r['notes']}\n")
        f.write("\n# cycle partition (starved + blocked + issued == cycles + 1)\n")
        for r in rows:
            f.write(f"{r['workload']:6s} empty={r['empty_pct']:8.4f}%  "
                    f"blocked={r['blocked_pct']:8.4f}%  issued={r['issued_pct']:8.4f}%  "
                    f"eligible-waiting={r['elig_wait_pct']:.4f}%\n")

    bad = [r for r in rows if r["checks_ok"] != "yes"]
    print(f"{len(rows)} cells -> {os.path.join(OUT, 'summary.csv')}")
    print(f"failing cells: {len(bad)}")
    for r in rows:
        print(f"  {r['workload']:6s} cycles={r['cycles']:>10,} "
              f"arr/cyc={r['arrivals_per_cycle']:.5f} q_avg={r['q_avg']:7.2f} "
              f"empty={r['empty_pct']:7.4f}% blocked={r['blocked_pct']:6.2f}% "
              f"wait={r['wait_avg']:8.1f} "
              f"cap={r['capacity_cmds_per_cycle']:.5f} "
              f"arr/cap={r['arrivals_vs_capacity_pct']:6.2f}% {r['checks_ok']}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
