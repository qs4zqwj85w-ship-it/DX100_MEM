#!/usr/bin/env python3
"""M23-G2D — extract the request-ordering cells, check them, decompose the change.

Reads build/wm/m23g2d/g2d_<workload>_<mode>.txt and the per-trace JSON records
written by tools/m23g2d_reorder.py; writes summary.csv, checks.txt and
decomposition.csv.

The decomposition uses M23-G2C's measured identity

    cycles  ~=  (hits * t_hit + misses * t_miss) / banks
            ==  dram_commands * mean_service / banks

which held to 99.4 % / 99.2 % there.  Between two modes the cycle difference is
therefore split exactly into

    ordering   :  r_A * (svc_B - svc_A) / banks     (same work, better rows)
    work       :  (r_B - r_A) * svc_B / banks       (fewer commands, new service)

with r = dram_commands and svc = bank-cycles per command.  The work term is not
a DRAM effect: it is the cache absorbing reuse that the reordered stream made
temporally adjacent.  Keeping the two apart is the point of the table.
"""
import csv
import hashlib
import json
import os
import struct
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g2d")
BANKS = 2
CLK_HZ = 400e6

WORKLOADS = [
    ("orkut", "Orkut BFS frontier (max-degree seed, 131 072-vertex induced subgraph)",
     "build/wm/m23_orkut_bfs.bin"),
    ("ogbn", "ogbn-products train minibatch gather (8 455 targets, 16 of 100 features)",
     "build/wm/m23_ogbn.bin"),
]
MODES = [
    ("orig", "A", "original SIMT order (baseline)"),
    ("reformed", "R", "same order, instructions re-formed (control)"),
    ("page", "B", "grouped by virtual page number"),
    ("row", "C", "grouped by modeled DRAM (bank, row)"),
]


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


def trace_info(path, banks=2, rows=4096, row_bytes=1024):
    """Independent fingerprint of a trace file: counts, footprint and the
    sha256 of the *sorted* address list (the multiset identity the brief asks
    for).  Computed from the file, not from any side-car metadata."""
    with open(path, "rb") as f:
        head = f.read(32)
        lanes, elem, n_instr, n_acc = struct.unpack_from("<IIQQ", head, 8)
        f.read(n_instr)
        rec = np.frombuffer(f.read(n_acc * 16), dtype=np.dtype([("addr", "<u8"),
                                                              ("instr", "<u4"),
                                                              ("lane", "<u4")]))
    addr = np.ascontiguousarray(rec["addr"])
    line = addr // 64
    lpr = max(1, row_bytes // 64)
    bidx = line ^ (line >> 6) ^ (line >> 12)
    cell = (bidx % banks).astype(np.int64) * rows + ((line // banks) // lpr) % rows
    return {
        "accesses": int(n_acc),
        "instrs": int(n_instr),
        "uniq_lines": int(np.unique(line).size),
        "uniq_pages": int(np.unique(line // 64).size),
        "uniq_cells": int(np.unique(cell).size),
        "address_multi_sha256": hashlib.sha256(np.sort(addr).tobytes()).hexdigest(),
        "line_multi_sha256": hashlib.sha256(np.sort(line.astype(np.uint64)).tobytes()).hexdigest(),
    }


def main():
    rows, dec, checks = [], [], []
    base = {}
    for wl, desc, trace in WORKLOADS:
        rec_orig = None
        for mode, letter, mdesc in MODES:
            log = os.path.join(OUT, f"g2d_{wl}_{mode}.txt")
            if not os.path.exists(log):
                print(f"missing {log}", file=sys.stderr)
                continue
            d = parse(log)
            trace_used = trace if mode == "orig" else f"build/wm/m23g2d/traces/{wl}_{mode}.bin"
            tj = os.path.join(ROOT, trace_used + ".json")
            meta = json.load(open(tj)) if os.path.exists(tj) else {}
            ti = trace_info(os.path.join(ROOT, trace_used))
            if rec_orig is None:
                rec_orig = ti

            cycles = i64(d, "cycles")
            cmds = i64(d, "dram_reads")
            bsum = i64(d, "dram_bank0_busy") + i64(d, "dram_bank1_busy")
            hits = i64(d, "dram_row_hits")
            conf = i64(d, "dram_row_conflicts")
            # Every command either hits an open row or activates one.  A bank is
            # closed only before its first command, so conflicts are activations
            # that interrupt an open row and cold activations are ~one per bank
            # (memory.cpp dram_tick counts a cold activation in neither counter).
            acts = cmds - hits
            cold = cmds - hits - conf
            rows_used = i64(d, "dram_rows_used")
            svc_cyc = i64(d, "dram_svc_hit_cycles") + i64(d, "dram_svc_miss_cycles")
            mean_svc = svc_cyc / max(1, cmds)
            dram_min = svc_cyc / BANKS
            q_sum = i64(d, "dram_q_occ_sum")
            wait_sum = i64(d, "dram_wait_sum")
            bytes_ = i64(d, "dram_bytes")
            acc = i64(d, "trace_accesses")

            ck = {
                "conservation_ok": i64(d, "conservation_ok") == 1 and i64(d, "conservation_violations") == 0,
                "acc_count_unchanged": ti["accesses"] == rec_orig["accesses"],
                "requests_equal_accesses": i64(d, "requests_generated") == acc == ti["accesses"],
                "address_multiset_identical": ti["address_multi_sha256"] == rec_orig["address_multi_sha256"],
                "line_multiset_identical": ti["line_multi_sha256"] == rec_orig["line_multi_sha256"],
                "footprint_identical": (ti["uniq_lines"], ti["uniq_pages"], ti["uniq_cells"]) ==
                                       (rec_orig["uniq_lines"], rec_orig["uniq_pages"], rec_orig["uniq_cells"]),
                "bytes_are_64_per_command": bytes_ == 64 * cmds,
                "commands_served": i64(d, "dram_svc_events") == cmds == i64(d, "dram_arrivals"),
                "cycle_partition": i64(d, "dram_starved_cycles") + i64(d, "dram_blocked_cycles") + cmds == cycles + 1,
                "no_eligible_but_unissued": i64(d, "dram_elig_wait_cycles") == 0,
                "queue_identity": wait_sum == q_sum,
                "completed_all": i64(d, "completed") == acc,
            }
            notes = [k for k, v in ck.items() if not v]

            r = {
                "workload": wl,
                "mode": letter,
                "mode_name": mode,
                "description": mdesc,
                "trace": trace_used,
                "trace_sha256": sha256(os.path.join(ROOT, trace_used)),
                "address_multi_sha256": ti["address_multi_sha256"][:16],
                "trace_instr": i64(d, "trace_instr"),
                "accesses": acc,
                "cycles": cycles,
                "dram_commands": cmds,
                "dram_bytes": bytes_,
                "bytes_per_cycle": round(bytes_ / cycles, 4),
                "row_hits": hits,
                "row_conflicts": conf,
                "row_activations": acts,
                "cold_activations": cold,
                "rows_activated": rows_used,
                "hit_rate_check_14_26": round((14.0 * hits + 26.0 * (cmds - hits)) / max(1, cmds), 3),
                "row_hit_pct": pct(hits, cmds),
                "row_conflict_pct": pct(conf, cmds),
                "mean_service_cycles": round(mean_svc, 3),
                "bank_busy_pct": pct(bsum, BANKS * cycles),
                "bank0_util_pct": pct(i64(d, "dram_bank0_busy"), cycles),
                "bank1_util_pct": pct(i64(d, "dram_bank1_busy"), cycles),
                "bank0_cmds": i64(d, "dram_bank0_reads"),
                "bank1_cmds": i64(d, "dram_bank1_reads"),
                "q_avg": round(q_sum / cycles, 3),
                "q_max": i64(d, "dram_max_q"),
                "wait_avg": round(wait_sum / max(1, cmds), 2),
                "wait_max": i64(d, "dram_wait_max"),
                "dram_limited_cycles": round(dram_min, 1),
                "dram_limited_pct_of_cycles": pct(dram_min, cycles),
                "l1_hits": i64(d, "l1_hits"),
                "l2_hits": i64(d, "l2_hits"),
                "l2_misses": i64(d, "l2_misses"),
                "uniq_lines": ti["uniq_lines"],
                "uniq_pages": ti["uniq_pages"],
                "uniq_cells": ti["uniq_cells"],
                "predicted_row_hit_rate": meta.get("stream_row_hit_pred_rate"),
                "same_cell_adjacent_rate": meta.get("same_cell_adjacent_rate"),
                "checks_ok": "yes" if not notes else "NO",
                "notes": ";".join(notes),
            }
            rows.append(r)

            if mode == "orig":
                base[wl] = r
            else:
                b = base[wl]
                svc_a, svc_b = b["mean_service_cycles"], r["mean_service_cycles"]
                ra, rb = b["dram_commands"], r["dram_commands"]
                ord_eff = ra * (svc_b - svc_a) / BANKS
                work_eff = (rb - ra) * svc_b / BANKS
                delta = r["cycles"] - b["cycles"]
                dec.append({
                    "workload": wl,
                    "mode": letter,
                    "cycles_baseline": b["cycles"],
                    "cycles_mode": r["cycles"],
                    "delta_cycles": delta,
                    "delta_pct": round(100.0 * delta / b["cycles"], 3),
                    "speedup": round(b["cycles"] / r["cycles"], 4),
                    "cycles_from_ordering": round(ord_eff, 1),
                    "cycles_from_work_change": round(work_eff, 1),
                    "residual_cycles": round(delta - ord_eff - work_eff, 1),
                    "ordering_pct_of_delta": round(100.0 * ord_eff / delta, 2) if delta else 0.0,
                    "work_pct_of_delta": round(100.0 * work_eff / delta, 2) if delta else 0.0,
                    "svc_baseline": svc_a,
                    "svc_mode": svc_b,
                    "const_work_speedup": round(svc_a / svc_b, 4),
                    "cmds_baseline": ra,
                    "cmds_mode": rb,
                    "cmd_ratio": round(rb / ra, 4),
                })

    rows.sort(key=lambda r: (r["workload"], "ARBC".index(r["mode"])))
    dec.sort(key=lambda r: (r["workload"], "ARBC".index(r["mode"])))

    with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    with open(os.path.join(OUT, "decomposition.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(dec[0].keys()))
        w.writeheader()
        w.writerows(dec)

    with open(os.path.join(OUT, "checks.txt"), "w") as f:
        f.write("# M23-G2D accounting and identity checks\n")
        for r in rows:
            f.write(f"{r['workload']:6s} {r['mode']:1s} {r['checks_ok']:3s}  {r['notes']}\n")
        f.write("\n# address multiset (sha256 of the sorted u64 address list), all four modes\n")
        seen = {}
        for r in rows:
            seen.setdefault(r["workload"], set()).add(r["address_multi_sha256"])
        for wl, s in seen.items():
            f.write(f"{wl:6s} distinct={len(s)} {sorted(s)}\n")
        f.write("\n# page vs row permutation identity\n")
        for wl, _, _ in WORKLOADS:
            a = os.path.join(OUT, f"traces/{wl}_page.bin")
            b = os.path.join(OUT, f"traces/{wl}_row.bin")
            if os.path.exists(a) and os.path.exists(b):
                f.write(f"{wl:6s} page_sha={sha256(a)[:16]} row_sha={sha256(b)[:16]} "
                        f"identical={'yes' if sha256(a) == sha256(b) else 'no'}\n")

    bad = [r for r in rows if r["checks_ok"] != "yes"]
    print(f"{len(rows)} cells -> summary.csv, {len(dec)} comparisons -> decomposition.csv")
    for r in rows:
        print(f"  {r['workload']:6s} {r['mode']:1s} cycles={r['cycles']:>9,} "
              f"cmds={r['dram_commands']:>9,} hit={r['row_hit_pct']:6.2f}% "
              f"svc={r['mean_service_cycles']:6.2f} B/cyc={r['bytes_per_cycle']:6.3f} "
              f"q={r['q_avg']:6.2f} wait={r['wait_avg']:8.1f} {r['checks_ok']}")
    print(f"failing cells: {len(bad)}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
