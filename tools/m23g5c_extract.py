#!/usr/bin/env python3
"""M23-G5C — extract the sparse-matrix cells, check the accounting, compare to M18.

Cells: build/wm/m23g5c/g5c_<matrix>_<variant>_w<W>_p<ORG>.txt
       build/wm/m23g5c/g5c_anchor_unfused_<matrix>_full_w8192_p0.txt

Each log is one M18 harness run: arm A = SIMT, arm B skipped (skip_b = 1), arm C =
dispatched Bulk with `bulk_org` = 0/2/3. The environment is M18's
(configs/ab_controlled.cfg, elem_bytes = 8, per-trace descriptor/index base and
row space, real translation, R = 128 shared), with the fused window enabled
because `bulk_org` exists only on that representation.

M18's published numbers are transcribed below as constants and compared against
the P0 cells, so "preserved" is verified rather than asserted. A mismatch is
printed and fails the run; it is never smoothed over.
"""
import csv
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m23g5c")
BANKS = 2
MATRICES = ["bcsstk25", "qc2534", "lnsp3937", "dubcova3"]
VARIANTS = ["full", "noidx"]
WINDOWS = [8192, 16384]
POL = {0: "P0", 2: "P2", 3: "P3"}

# ---- M18 reference (docs/m18-spmv-validation-report.md §2.1 / §2.2) ----------
# same-trace: A(full) and C(full) at 2x8K and 2x16K
M18_SAME = {
    "bcsstk25": {"A": 461688, "C8": 492449, "C16": 505260},
    "qc2534":   {"A": 762503, "C8": 923790, "C16": 923269},
    "lnsp3937": {"A": 57014,  "C8": 48298,  "C16": 48205},
    "dubcova3": {"A": 3522146, "C8": 4011805, "C16": 4013206},
}
# faithful pairing: A(noidx) and C(noidx)
M18_NOIDX = {
    "bcsstk25": {"A": 308704, "C8": 390495, "C16": 392188},
    "qc2534":   {"A": 446600, "C8": 728062, "C16": 743702},
    "lnsp3937": {"A": 37422,  "C8": 46113,  "C16": 46104},
    "dubcova3": {"A": 2461123, "C8": 2988123, "C16": 2996821},
}
# M18 §2.1 row-hit rates for arm C, 8K
M18_ROWHIT_C8 = {"bcsstk25": 86.8, "qc2534": 88.5, "lnsp3937": 87.7, "dubcova3": 83.8}

REQUIRED_M16 = ["dram_reads", "dram_row_hits", "dram_row_conflicts", "dram_svc_events",
                "dram_busy_cycles", "dram_q_occ_sum", "dram_starved_cycles",
                "dram_blocked_cycles", "l1_reads", "l1_hits", "l2_reads", "l2_hits",
                "l2_misses", "walks", "tlb0_misses", "tlb0_mshr_stalls", "pte_fetches",
                "l2tlb_misses", "inflight_max", "replays", "simt_completed",
                "simt_requests_generated", "bulk_dedup_merged", "bulk_lines_issued",
                "bulk_org_cycles", "bulk_slot_stall", "bulk_tgt_req_issued",
                "bulk_tgt_req_done", "bulk_sched_idle", "bulk_sched_defer"]


def parse(path):
    """Return ({'A': {...}, 'C': {...}}, cycles {'A': n, 'C': n}, gates, m15)."""
    arms = {"A": {}, "B": {}, "C": {}}
    m15 = {}
    cycles = {}
    gates = False
    with open(path, errors="replace") as f:
        for line in f:
            if "M9 GATES PASSED" in line:
                gates = True
            m = re.match(r"^\s+cycles\s+(\d+)\s+(\d+)\s+(\d+)\s*$", line)
            if m:
                cycles = {"A": int(m.group(1)), "C": int(m.group(3))}
            for tok in line.split():
                if tok.startswith("m16_") or (tok.startswith("m16") and "_" not in tok):
                    pass
            # m16 lines look like: "  m16 A_dram_reads=50245"
            m = re.search(r"\bm16\s+([ABC])_([a-z0-9_]+)=(-?\d+)", line)
            if m:
                arms.setdefault(m.group(1), {})[m.group(2)] = int(m.group(3))
                continue
            m = re.search(r"\bm15\s+([a-z0-9_]+)=(-?\d+)", line)
            if m:
                m15[m.group(1)] = int(m.group(2))
    return arms, cycles, gates, m15


def pct(a, b):
    return round(100.0 * a / b, 4) if b else None


def ratio(a, b):
    return round(a / b, 4) if a and b else None


def main():
    if not os.path.isdir(OUT):
        print("no cells", file=sys.stderr)
        return 1

    meta = {}
    for m in MATRICES:
        with open(os.path.join(ROOT, f"build/wm/m18_{m}_full.bin.json")) as f:
            meta[m] = json.load(f)

    rows, problems = [], []
    a_ref = {}
    for m in MATRICES:
        for v in VARIANTS:
            for w in WINDOWS:
                for org in (0, 2, 3):
                    p = os.path.join(OUT, f"g5c_{m}_{v}_w{w}_p{org}.txt")
                    if not os.path.exists(p) or os.path.getsize(p) == 0:
                        problems.append(f"missing cell {os.path.basename(p)}")
                        continue
                    arms, cycles, gates, m15 = parse(p)
                    A, C = arms["A"], arms["C"]
                    if not cycles:
                        problems.append(f"{os.path.basename(p)}: no cycles line")
                        continue
                    missing = [k for k in REQUIRED_M16 if k not in A or k not in C]
                    elems = m15.get("bulk_elems_total") or m15.get("bulk_elems")
                    merged = C.get("bulk_dedup_merged")
                    issued = C.get("bulk_lines_issued")
                    checks = {
                        "m9_gates": gates,
                        "no_addr_mismatch": m15.get("bulk_addr_mismatch") == 0,
                        "dedup_identity": (elems is not None and merged is not None
                                           and issued is not None
                                           and issued + merged == elems),
                        "simt_requests_all_completed":
                            A.get("simt_requests_generated") == A.get("simt_completed"),
                        "bulk_targets_all_done":
                            C.get("bulk_tgt_req_issued") == C.get("bulk_tgt_req_done"),
                        "residency_bound": (A.get("inflight_max") or 0) <= 128
                                           and (C.get("inflight_max") or 0) <= 128,
                        "fused_window": m15.get("bulk_win_fused") == 1,
                    }
                    bad = [k for k, ok in checks.items() if not ok]
                    if missing:
                        problems.append(f"{os.path.basename(p)}: missing {missing}")
                    if bad:
                        problems.append(f"{os.path.basename(p)}: failed {bad}")
                    rows_by = A.get("dram_reads")
                    row = {
                        "matrix": m, "variant": v, "window": w, "policy": POL[org],
                        "cycles_simt": cycles["A"], "cycles_bulk": cycles["C"],
                        "speedup_vs_simt": ratio(cycles["A"], cycles["C"]),
                        "a_dram_reads": rows_by, "c_dram_reads": C.get("dram_reads"),
                        "c_row_hits": C.get("dram_row_hits"),
                        "c_row_hit_pct": pct(C.get("dram_row_hits") or 0, C.get("dram_reads") or 0),
                        "a_row_hit_pct": pct(A.get("dram_row_hits") or 0, A.get("dram_reads") or 0),
                        "c_row_conflicts": C.get("dram_row_conflicts"),
                        "c_svc_events": C.get("dram_svc_events"),
                        "c_busy_cycles": C.get("dram_busy_cycles"),
                        "c_busy_per_cmd": round((C.get("dram_busy_cycles") or 0)
                                                / (C.get("dram_reads") or 1), 3),
                        "c_q_occ_sum": C.get("dram_q_occ_sum"),
                        "c_starved": C.get("dram_starved_cycles"),
                        "c_blocked": C.get("dram_blocked_cycles"),
                        "c_l1_reads": C.get("l1_reads"), "c_l1_hits": C.get("l1_hits"),
                        "c_l2_reads": C.get("l2_reads"), "c_l2_hits": C.get("l2_hits"),
                        "c_l2_hit_pct": pct(C.get("l2_hits") or 0, C.get("l2_reads") or 0),
                        "a_l2_hit_pct": pct(A.get("l2_hits") or 0, A.get("l2_reads") or 0),
                        "elems": elems, "dedup_merged": merged, "lines_issued": issued,
                        "dedup_pct": pct(merged or 0, elems or 0),
                        "c_walks": C.get("walks"), "c_dtlb_misses": C.get("tlb0_misses"),
                        "c_dtlb_mshr_stalls": C.get("tlb0_mshr_stalls"),
                        "c_pte_fetches": C.get("pte_fetches"),
                        "c_l2tlb_misses": C.get("l2tlb_misses"),
                        "a_walks": A.get("walks"), "a_dtlb_misses": A.get("tlb0_misses"),
                        "a_dtlb_mshr_stalls": A.get("tlb0_mshr_stalls"),
                        "c_org_cycles": C.get("bulk_org_cycles"),
                        "c_org_pct": pct(C.get("bulk_org_cycles") or 0, cycles["C"]),
                        "c_slot_stall": C.get("bulk_slot_stall"),
                        "c_sched_idle": C.get("bulk_sched_idle"),
                        "c_sched_defer": C.get("bulk_sched_defer"),
                        "c_inflight_max": C.get("inflight_max"),
                        "checks_ok": "yes" if not bad and not missing else "NO",
                        "checks_failed": ";".join(bad),
                    }
                    rows.append(row)
                    if v == "full" and org == 0:
                        a_ref[(m, w)] = cycles["A"]

    if not rows:
        print("no parsable cells", file=sys.stderr)
        return 1

    # ---- A must not move with the policy (it is the same arm A) -------------
    for m in MATRICES:
        for v in VARIANTS:
            for w in WINDOWS:
                vals = {r["cycles_simt"] for r in rows if r["matrix"] == m
                        and r["variant"] == v and r["window"] == w}
                if len(vals) > 1:
                    problems.append(f"A moved with bulk_org on {m}/{v}/w{w}: {sorted(vals)}")

    # ---- M18 preservation: P0 must equal M18's published C arm -------------
    m18 = []
    for m in MATRICES:
        ref = M18_SAME[m] if True else None
        for w, key in ((8192, "C8"), (16384, "C16")):
            got = next((r for r in rows if r["matrix"] == m and r["variant"] == "full"
                        and r["window"] == w and r["policy"] == "P0"), None)
            refn = M18_NOIDX[m][key]
            got_n = next((r for r in rows if r["matrix"] == m and r["variant"] == "noidx"
                          and r["window"] == w and r["policy"] == "P0"), None)
            m18.append({
                "matrix": m, "window": w,
                "m18_A_full": M18_SAME[m]["A"],
                "g5c_A_full": a_ref.get((m, w)),
                "m18_C_full": M18_SAME[m][key],
                "g5c_C_full_p0": got["cycles_bulk"] if got else None,
                "m18_A_noidx": M18_NOIDX[m]["A"],
                "g5c_A_noidx": got_n["cycles_simt"] if got_n else None,
                "m18_C_noidx": refn,
                "g5c_C_noidx_p0": got_n["cycles_bulk"] if got_n else None,
                "identical": (got and got_n
                              and got["cycles_bulk"] == M18_SAME[m][key]
                              and got_n["cycles_bulk"] == refn
                              and got_n["cycles_simt"] == M18_NOIDX[m]["A"]),
            })
        for rec in m18:
            if rec["matrix"] == m and not rec["identical"]:
                problems.append(f"M18 mismatch on {m} w{rec['window']}")

    # ---- anchor cells (unfused representation) -----------------------------
    anchors = []
    for m in MATRICES:
        p = os.path.join(OUT, f"g5c_anchor_unfused_{m}_full_w8192_p0.txt")
        if not os.path.exists(p):
            problems.append(f"missing anchor {os.path.basename(p)}")
            continue
        arms, cycles, gates, m15 = parse(p)
        fused = next((r for r in rows if r["matrix"] == m and r["variant"] == "full"
                      and r["window"] == 8192 and r["policy"] == "P0"), None)
        anchors.append({
            "matrix": m,
            "unfused_A": cycles.get("A"), "unfused_C": cycles.get("C"),
            "unfused_row_hits": arms["C"].get("dram_row_hits"),
            "fused_A": fused["cycles_simt"] if fused else None,
            "fused_C": fused["cycles_bulk"] if fused else None,
            "fused_row_hits": fused["c_row_hits"] if fused else None,
            "m18_published_C": M18_SAME[m]["C8"],
            "unfused_eq_fused": (fused and cycles.get("C") == fused["cycles_bulk"]
                                 and cycles.get("A") == fused["cycles_simt"]),
            "unfused_eq_m18": fused and cycles.get("C") == M18_SAME[m]["C8"],
        })

    # ---- faithful pairing (M18's primary metric) ---------------------------
    faithful = []
    for m in MATRICES:
        for w in WINDOWS:
            for org in (0, 2, 3):
                af = next((r for r in rows if r["matrix"] == m and r["variant"] == "full"
                           and r["window"] == w and r["policy"] == POL[org]), None)
                cn = next((r for r in rows if r["matrix"] == m and r["variant"] == "noidx"
                           and r["window"] == w and r["policy"] == POL[org]), None)
                if af and cn:
                    faithful.append({
                        "matrix": m, "window": w, "policy": POL[org],
                        "A_full": af["cycles_simt"], "C_noidx": cn["cycles_bulk"],
                        "S_faithful": ratio(af["cycles_simt"], cn["cycles_bulk"]),
                        "S_same_trace": af["speedup_vs_simt"],
                    })

    # ---- structural table --------------------------------------------------
    struct = []
    for m in MATRICES:
        d = meta[m]
        struct.append({
            "matrix": m, "rows": d["rows"], "cols": d["cols"],
            "nnz_full": d["nnz_full"], "nnz_stored": d["nnz_stored"],
            "row_len_mean": round(d["row_len_mean"], 2),
            "row_len_max": d["row_len_max"],
            "refs_per_unique_column": round(d["refs_per_unique_column"], 2),
            "accesses": d["accesses"], "instructions": d["instructions"],
            "unique_lines": d["unique_lines"], "unique_pages": d["unique_pages"],
            "duplicate_line_frac": round(d["duplicate_line_frac"], 4),
            "x_bytes": d["cols"] * 8,
            "max_addr": d["layout"]["max_addr"],
            "symmetric_expanded": d.get("symmetric_expanded"),
            "rows_scope_note": ("first 45 000 rows (M18)" if m == "dubcova3" else "full matrix"),
        })

    def dump(name, recs):
        if not recs:
            return
        with open(os.path.join(OUT, name), "w", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=list(recs[0].keys()))
            wr.writeheader()
            wr.writerows(recs)

    dump("summary.csv", rows)
    dump("m18_reference.csv", m18)
    dump("anchor.csv", anchors)
    dump("faithful.csv", faithful)
    dump("structure.csv", struct)

    # ---- console -----------------------------------------------------------
    for v in VARIANTS:
        print(f"\n### variant = {v}  (cycles: A = SIMT, C = Bulk)")
        print(f"{'matrix':10s} {'W':>6s} {'P':3s} {'A':>10s} {'C':>10s} {'S':>6s} "
              f"{'rowhit':>7s} {'conf':>7s} {'cmds':>8s} {'busy/cmd':>8s} {'dedup%':>7s} "
              f"{'walks':>7s} {'org%':>5s} {'chk':>4s}")
        for r in rows:
            if r["variant"] != v:
                continue
            print(f"{r['matrix']:10s} {r['window']:6d} {r['policy']:3s} "
                  f"{r['cycles_simt']:10,} {r['cycles_bulk']:10,} "
                  f"{(r['speedup_vs_simt'] or 0):6.3f} {(r['c_row_hit_pct'] or 0):6.2f}% "
                  f"{(r['c_row_conflicts'] or 0):7,} {(r['c_dram_reads'] or 0):8,} "
                  f"{(r['c_busy_per_cmd'] or 0):8.2f} {(r['dedup_pct'] or 0):6.1f}% "
                  f"{(r['c_walks'] or 0):7,} {(r['c_org_pct'] or 0):5.1f} "
                  f"{r['checks_ok']:>4s}")

    print("\n### M18 preservation (P0 vs the published M18 cells)")
    for rec in m18:
        print(f"{rec['matrix']:10s} w{rec['window']:5d} A {rec['m18_A_full']:>9,}/"
              f"{rec['g5c_A_full']:>9,}  C {rec['m18_C_full']:>9,}/{rec['g5c_C_full_p0']:>9,}  "
              f"C(noidx) {rec['m18_C_noidx']:>9,}/{rec['g5c_C_noidx_p0']:>9,}  "
              f"-> {'identical' if rec['identical'] else 'DIFFERS'}")

    print("\n### faithful pairing S = A(full) / C(noidx)")
    for rec in faithful:
        print(f"{rec['matrix']:10s} w{rec['window']:5d} {rec['policy']:3s} "
              f"A {rec['A_full']:>9,} C {rec['C_noidx']:>9,}  S {rec['S_faithful']:5.3f}  "
              f"(same-trace {rec['S_same_trace']:5.3f})")

    print(f"\n{len(rows)} cells, {len(faithful)} faithful pairs, {len(anchors)} anchors")
    print(f"problems: {len(problems)}")
    for p in problems:
        print("  " + p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
