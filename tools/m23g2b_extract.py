#!/usr/bin/env python3
"""M23-G2B — extract the four instruction-issue cells.

Reads build/wm/m23g2b/g2b_<workload>_m<mshr>.txt and writes summary.csv plus
checks.txt. Every number comes from a simulator counter; nothing is modelled
here except the two derived rates named "capacity" and "util", which are stated
as ratios of counters.

Front-end facts this file relies on (sim/src/system.cpp, run(Mode::Baseline)):

  * at most ONE instruction is admitted to the coalescer per cycle
    (system.cpp:185), and an instruction is admitted only once the previous
    one's line list has been fully staged (`cur_pos >= cur_lines.size()`);
  * an admitted instruction is coalesced into at most `lanes` distinct lines
    (the latch bound, checked at system.cpp:265), counted in
    `requests_generated`;
  * formed requests may be submitted at up to `warps_per_core` per cycle
    (system.cpp:168-182), counted in `issued` / `requests_accepted`;
  * `slot_full_cycles` counts cycles where a core held formed-but-unplaced
    lines with no free R slot (system.cpp:277-286).

So, for a trace with instructions still pending - which is every cycle before
the last instruction is admitted - a cycle without an admission is a cycle in
which an instruction was eligible and not issued. `eligible_unissued_cycles =
cycles - instr_issued` is therefore exact, and `slot_full_cycles` says how many
of those were R-budget backpressure rather than the coalescer being busy.
"""
import csv
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g2b")

WORKLOADS = [
    ("criteo", "Criteo 1TB click logs, 16 384 records", "build/wm/m23_criteo_16k.bin"),
    ("ogbn", "ogbn-products train minibatch gather", "build/wm/m23_ogbn.bin"),
]
MSHR = [64, 128]
BANKS = 2          # cfg banks = 2, and dram.bank_mask == 3 in every cell
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
    return int(d[k])


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    rows = []
    for wl, desc, trace in WORKLOADS:
        trace_path = os.path.join(ROOT, trace)
        tsha = sha256(trace_path)
        for m in MSHR:
            p = os.path.join(OUT, f"g2b_{wl}_m{m}.txt")
            d = parse(p)

            cycles = i64(d, "cycles")
            instr = i64(d, "instr_issued")
            trace_instr = i64(d, "trace_instr")
            addr = i64(d, "trace_accesses")
            req = i64(d, "requests_generated")
            accepted = i64(d, "requests_accepted")
            l1_miss = i64(d, "l1_misses")
            l2_miss = i64(d, "l2_misses")
            l2_reads = i64(d, "l2_reads")
            l2_hits = i64(d, "l2_hits")
            arrivals = i64(d, "l2_mshr_allocs")
            cmds = i64(d, "dram_reads")
            svc = i64(d, "dram_svc_events")
            svc_cyc = i64(d, "dram_svc_hit_cycles") + i64(d, "dram_svc_miss_cycles")
            busy = i64(d, "dram_busy_cycles")
            dram_bytes = i64(d, "dram_bytes")
            slot_full = i64(d, "simt_slot_full_cycles")
            staging_max = i64(d, "simt_staging_max")
            unissued_max = i64(d, "unissued_max")
            peak_slot = i64(d, "simt_peak_slot")
            peak_out = i64(d, "simt_peak_outstanding")
            end_unissued = i64(d, "requests_unissued_end")
            completed = i64(d, "completed")
            cons = i64(d, "conservation_ok")
            viol = i64(d, "conservation_violations")
            warps = i64(d, "warps_per_core") if "warps_per_core" in d else 4

            elig = cycles - instr                       # eligible-but-unissued cycles
            mean_svc = svc_cyc / max(1, svc)            # bank-cycles per command
            capacity = BANKS / mean_svc if mean_svc else 0.0   # cmds/cycle sustained

            checks = {
                "conservation_ok": cons == 1 and viol == 0,
                "every_trace_instruction_issued": instr == trace_instr,
                "generated_equals_accepted_and_completed": req == accepted == completed,
                "no_unissued_left_at_end": end_unissued == 0,
                "bytes_are_64_per_command": dram_bytes == 64 * cmds,
                "l2_reads_is_hits_plus_misses": l2_reads == l2_hits + l2_miss,
                "every_command_is_a_fill": cmds == arrivals == svc,
                "staging_within_lanes": staging_max <= 4,
                "peak_slot_within_R": peak_slot <= 128,
            }
            notes = [k for k, v in checks.items() if not v]

            rows.append({
                "workload": wl,
                "workload_detail": desc,
                "trace": trace,
                "trace_sha256": tsha,
                "l2_mshr": m,
                "cycles": cycles,
                "instr_issued": instr,
                "instr_per_cycle": round(instr / cycles, 6),
                "instr_issue_util_pct": round(100.0 * instr / cycles, 3),
                "eligible_unissued_cycles": elig,
                "eligible_unissued_pct": round(100.0 * elig / cycles, 3),
                "slot_full_cycles": slot_full,
                "slot_full_of_eligible_pct": round(100.0 * slot_full / max(1, elig), 3),
                "addresses_generated": addr,
                "addresses_per_cycle": round(addr / cycles, 6),
                "requests_generated": req,
                "requests_per_cycle": round(req / cycles, 6),
                "requests_per_instr": round(req / max(1, instr), 4),
                "submit_util_pct_of_warps": round(100.0 * (accepted / cycles) / warps, 3),
                "l1_misses_per_cycle": round(l1_miss / cycles, 6),
                "l2_misses_per_cycle": round(l2_miss / cycles, 6),
                "dram_arrivals_per_cycle": round(arrivals / cycles, 6),
                "dram_completions_per_cycle": round(cmds / cycles, 6),
                "dram_mean_service_cycles": round(mean_svc, 3),
                "dram_capacity_cmds_per_cycle": round(capacity, 6),
                "dram_arrival_vs_capacity_pct": round(100.0 * (arrivals / cycles) / capacity, 2),
                "dram_busy_pct": round(100.0 * busy / (BANKS * cycles), 2),
                "staging_max": staging_max,
                "held_queue_max": unissued_max,
                "peak_slot": peak_slot,
                "peak_outstanding": peak_out,
                "checks_ok": "yes" if not notes else "NO",
                "notes": ";".join(notes),
            })

    fields = list(rows[0].keys())
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)

    with open(os.path.join(OUT, "checks.txt"), "w") as f:
        f.write("# M23-G2B cell accounting\n")
        for r in rows:
            f.write(f"{r['workload']:7s} mshr={r['l2_mshr']:3d}  {r['checks_ok']:3s}  "
                    f"{r['notes']}\n")
        f.write("\n# trace identity\n")
        for r in rows:
            f.write(f"{r['workload']:7s} {r['trace']}  sha256={r['trace_sha256']}\n")

    bad = [r for r in rows if r["checks_ok"] != "yes"]
    print(f"{len(rows)} cells -> {os.path.join(OUT, 'summary.csv')}")
    print(f"failing cells: {len(bad)}")
    hdr = ("wl      mshr  cycles        instr/cyc  elig-unissued  budget-starved"
           "  req/cyc  L2miss/cyc  DRAM/cyc  capacity  busy")
    print(hdr)
    for r in rows:
        print(f"{r['workload']:7s} {r['l2_mshr']:3d} {r['cycles']:>12,} "
              f"{r['instr_per_cycle']:>10.5f} {r['eligible_unissued_pct']:>12.2f}% "
              f"{r['slot_full_of_eligible_pct']:>12.2f}% {r['requests_per_cycle']:>8.4f} "
              f"{r['l2_misses_per_cycle']:>11.5f} {r['dram_arrivals_per_cycle']:>9.5f} "
              f"{r['dram_capacity_cmds_per_cycle']:>8.5f} {r['dram_busy_pct']:>5.1f}%")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
