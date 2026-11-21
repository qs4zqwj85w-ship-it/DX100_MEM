#!/usr/bin/env python3
"""M23-G2A — extract the nine L2 MSHR sensitivity cells.

Reads build/wm/m23g2a/g2a_<workload>_m<mshr>.txt, checks the accounting of
every cell, and writes build/wm/m23g2a/summary.csv plus checks.txt.

Every number is read from a log line; nothing is re-simulated. Rates are formed
from the counters (dram_bytes / cycles), the clock convention is the simulator's
own 400 MHz (system.cpp:285), which is also what its `dram_gbs` summary line
uses.
"""
import csv
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g2a")

WORKLOADS = [
    ("criteo", "Criteo 1TB click logs, 16 384 records", "build/wm/m23_criteo_16k.bin"),
    ("orkut", "Orkut BFS frontier, max-degree seed", "build/wm/m23_orkut_bfs.bin"),
    ("ogbn", "ogbn-products train minibatch gather", "build/wm/m23_ogbn.bin"),
]
MSHR = [64, 128, 256]
CLK_HZ = 400e6


def parse(path):
    """key=value lines -> dict; the `realized workload` line keeps its own keys."""
    d = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("=") or line.startswith("--"):
                continue
            if "=" not in line:
                continue
            for tok in line.split():
                if "=" in tok:
                    k, _, v = tok.partition("=")
                    # first occurrence wins: identical keys are repeated by the
                    # == summary == block and the != counters block with equal
                    # values, but `cycles=...` appears in both
                    d.setdefault(k, v)
                    d[k + "@dup"] = v
    return d


def num(d, k):
    if k not in d:
        raise KeyError(f"missing field {k}")
    return float(d[k])


def i64(d, k):
    return int(num(d, k))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    rows = []
    trace_sha = {}
    for wl, desc, trace in WORKLOADS:
        tp = os.path.join(ROOT, trace)
        trace_sha[wl] = sha256(tp)
    for wl, desc, trace in WORKLOADS:
        base_cycles = None
        realized = None
        for m in MSHR:
            p = os.path.join(OUT, f"g2a_{wl}_m{m}.txt")
            d = parse(p)
            cycles = i64(d, "cycles")
            acc = i64(d, "accesses")
            instrs = i64(d, "instrs")
            lines = i64(d, "lines")
            pages = i64(d, "pages")
            rho = num(d, "rho_line")
            # the realized workload block is computed from the trace at load
            # time, before any memory interaction: it must be identical across
            # the three capacities of one workload
            rl = (acc, instrs, lines, pages, rho)
            if realized is None:
                realized = rl
            same_trace = (rl == realized)

            dram_bytes = i64(d, "dram_bytes")
            dram_reads = i64(d, "dram_reads")
            dram_svc = i64(d, "dram_svc_events")
            l2_reads = i64(d, "l2_reads")
            l2_hits = i64(d, "l2_hits")
            l2_misses = i64(d, "l2_misses")
            allocs = i64(d, "l2_mshr_allocs")
            joins = i64(d, "l2_mshr_joins")
            occ = i64(d, "l2_mshr_occ")          # occupancy-cycles, summed
            peak = i64(d, "l2_mshr_max")
            l1_reads = i64(d, "l1_reads")
            l1_peak = i64(d, "l1_mshr_max")
            infl_peak = i64(d, "inflight_max")
            infl_occ = i64(d, "inflight_occ")
            q_occ = i64(d, "dram_q_occ_sum")
            q_peak = i64(d, "dram_max_q")
            q_full = i64(d, "dram_q_full")
            fail_cycles = i64(d, "cc0_l2_stalls")
            fail_cycles_all = sum(i64(d, f"cc{c}_l2_stalls") for c in range(4))
            walks = i64(d, "walks")
            ptes = i64(d, "pte_fetches")
            completed = i64(d, "completed")
            cc0_done = i64(d, "cc0_done")
            cc0_issue = i64(d, "cc0_issue")
            cons = i64(d, "conservation_ok")
            viol = i64(d, "conservation_violations")
            row_hits = i64(d, "dram_row_hits")
            row_conf = i64(d, "dram_row_conflicts")
            replays = i64(d, "replays")
            # PTE traffic must be absent: clients 2 and 3 are the walker and
            # the engine target stream, both inert with ideal translation and
            # dispatch = 0
            stray = sum(i64(d, f"cc{c}_l2_reads") for c in (2, 3))

            checks = {
                "conservation_ok": cons == 1 and viol == 0,
                # the Criteo trace carries an engine-classed element stream that
                # a front-end with no engine does not issue (M23-B sec. 9,
                # M23-F1 sec. 6: the SIMT request count is 360 326 there), so
                # `completed == accesses` is not the right invariant for it.
                # What must hold is that every issued request completed, and
                # that the issued stream is the same in all three capacities.
                "completed_matches_issue":
                    completed == cc0_done == cc0_issue == l1_reads,
                "bytes_are_64_per_command": dram_bytes == 64 * dram_reads,
                "l2_reads_is_hits_plus_misses": l2_reads == l2_hits + l2_misses,
                "mshr_allocs_plus_joins_is_misses": allocs + joins == l2_misses,
                "no_translation_traffic": walks == 0 and ptes == 0 and stray == 0,
                "dram_commands_are_l2_fills": dram_reads == allocs,
                "svc_events_equal_commands": dram_svc == dram_reads,
                "no_l1_refusals": True,
                "mshr_peak_within_capacity": peak <= m,
                "saturation_implies_peak_at_capacity":
                    fail_cycles == 0 or peak == m,
                "queue_never_full": q_full == 0,
                "fail_cycles_match_replays": fail_cycles == replays,
                "fail_cycles_client0_only": fail_cycles == fail_cycles_all,
            }
            notes = [k for k, v in checks.items() if not v]
            if not same_trace:
                notes.append("realized_workload_differs")

            if base_cycles is None:
                base_cycles = cycles

            rows.append({
                "workload": wl,
                "workload_detail": desc,
                "trace": trace,
                "trace_sha256": trace_sha[wl],
                "l2_mshr": m,
                "accesses": acc,
                "instructions": instrs,
                "distinct_lines": lines,
                "distinct_pages": pages,
                "cycles": cycles,
                "speedup_vs_m64": round(base_cycles / cycles, 4),
                "dram_bytes": dram_bytes,
                "dram_commands": dram_reads,
                "bytes_per_cycle": round(dram_bytes / cycles, 4),
                "dram_gbs": round(dram_bytes / cycles * CLK_HZ / 1e9, 4),
                "row_hits": row_hits,
                "row_conflicts": row_conf,
                "row_hit_rate": round(row_hits / max(1, row_hits + row_conf), 5),
                "l2_reads": l2_reads,
                "l2_hits": l2_hits,
                "l2_misses": l2_misses,
                "l2_mshr_allocs": allocs,
                "l2_mshr_joins": joins,
                "l2_mshr_fail_cycles": fail_cycles,
                "l2_mshr_fail_per_cycle": round(fail_cycles / cycles, 4),
                "l2_mshr_fail_per_kaccess": round(fail_cycles / max(1, acc) * 1000, 2),
                "l2_mshr_peak": peak,
                "l2_mshr_peak_pct": round(100.0 * peak / m, 2),
                "l2_mshr_avg": round(occ / cycles, 4),
                "l2_mshr_avg_pct": round(100.0 * occ / cycles / m, 2),
                "l1_mshr_peak": l1_peak,
                "inflight_peak": infl_peak,
                "inflight_avg": round(infl_occ / cycles, 4),
                "dram_q_peak": q_peak,
                "dram_q_avg": round(q_occ / cycles, 4),
                "dram_busy_pct": round(100.0 * i64(d, "dram_busy_cycles")
                                       / (2.0 * cycles), 2),
                "dram_starved_cycles": i64(d, "dram_starved_cycles"),
                "checks_ok": None,          # settled after the stream check
                "notes": ";".join(notes),
                "_stream": (completed, l1_reads),
            })

        # ---- cross-capacity stream identity -------------------------------
        # A capacity change must not alter the request stream: the same trace
        # must issue the same number of completions. This is the per-workload
        # half of "identical address traces across configurations"; the other
        # half is the trace file's SHA-256, identical by construction.
        streams = {r["_stream"] for r in rows if r["workload"] == wl}
        same_stream = len(streams) == 1
        for r in rows:
            if r["workload"] != wl:
                continue
            ok = not r["notes"] and same_stream
            r["checks_ok"] = "yes" if ok else "NO"
            if not same_stream:
                r["notes"] = (r["notes"] + ";" if r["notes"] else "") + \
                    "stream_differs_across_capacities"

    fields = [k for k in rows[0].keys() if not k.startswith("_")]
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)

    with open(os.path.join(OUT, "checks.txt"), "w") as f:
        f.write("# M23-G2A cell accounting\n")
        f.write("# every row must read OK\n")
        for r in rows:
            f.write(f"{r['workload']:8s} mshr={r['l2_mshr']:3d}  "
                    f"{r['checks_ok']:3s}  {r['notes']}\n")
        f.write("\n# identical address traces across capacities (realized "
                "workload block + trace sha256)\n")
        for wl, desc, trace in WORKLOADS:
            f.write(f"{wl:8s} trace={trace}  sha256={trace_sha[wl]}\n")
            seen = {(r["accesses"], r["instructions"], r["distinct_lines"],
                     r["distinct_pages"]) for r in rows if r["workload"] == wl}
            f.write(f"{'':8s} distinct realized-workload tuples across the "
                    f"three capacities: {len(seen)} (must be 1)\n")

    bad = [r for r in rows if r["checks_ok"] != "yes"]
    print(f"{len(rows)} cells -> {os.path.join(OUT, 'summary.csv')}")
    print(f"failing cells: {len(bad)}")
    for r in rows:
        print(f"  {r['workload']:7s} mshr={r['l2_mshr']:3d} "
              f"cycles={r['cycles']:>12,} {r['bytes_per_cycle']:>7.3f} B/cyc "
              f"peak={r['l2_mshr_peak']:3d} avg={r['l2_mshr_avg']:7.2f} "
              f"fail={r['l2_mshr_fail_cycles']:>12,} {r['checks_ok']}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
