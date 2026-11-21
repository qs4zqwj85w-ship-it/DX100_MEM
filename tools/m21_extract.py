#!/usr/bin/env python3
"""M21 metric extraction: workload structure x simulator counters.

  structure   unique lines/pages, duplicate rate, working set, reuse distance
  perf        cycles and both speedups per workload x seed x window
  memory      DRAM, row hits, cache hit rates, TLB/walks per arm
  mechanism   engine phases, occupancy, stalls, descriptor/AG/fanout cost
  ablation    control vs page-order-off vs cohort counts, with liveness counters

Structure comes from tools/m21_reuse.py (offline, reads the trace); everything
else comes from the M9 cell logs parsed by the M20 extractor.
"""
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m20_extract as E

PRIMARY = re.compile(r"^m21_(?P<id>w\d+_\w+?)_s(?P<s>\d+)_(?P<v>full|noidx)_w(?P<W>\d+)\.txt$")
ABL = re.compile(r"^m21abl_(?P<id>w\d+_\w+?)_(?P<cfg>\w+?)_(?P<v>full|noidx)\.txt$")
WORKLOADS = ["w1_uniform", "w2_clustered", "w3_duphigh", "w4_duplow",
             "w5_sequential", "w6_zipf", "w7_wssmall", "w8_wslarge"]


def primary():
    out = {}
    for name in sorted(os.listdir("build/wm")):
        m = PRIMARY.match(name)
        if not m:
            continue
        out[(m.group("id"), int(m.group("s")), int(m.group("W")), m.group("v"))] = \
            E.load(os.path.join("build/wm", name))
    return out


def ablations():
    out = {}
    for name in sorted(os.listdir("build/wm")):
        m = ABL.match(name)
        if not m:
            continue
        out[(m.group("id"), m.group("cfg"), m.group("v"))] = \
            E.load(os.path.join("build/wm", name))
    return out


_STRUCT = {}


def structure(wid, seed=1):
    key = (wid, seed)
    if key in _STRUCT:
        return _STRUCT[key]
    path = "build/wm/m21_%s_s%d_full.bin" % (wid, seed)
    d = json.loads(subprocess.check_output([sys.executable, "tools/m21_reuse.py", path, "--json"]))
    gen = json.load(open(path + ".json"))["generation"]
    d["generation"] = gen
    _STRUCT[key] = d
    return d


def cmd_structure(argv):
    print("| workload | pattern | cols | tgt lines | tgt pages | WS (KiB) | dup(line) | dup(word) | reuse<8K | idx req | tgt req |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for w in WORKLOADS:
        d = structure(w)
        st = d["structures"]
        s = d["streams"]
        print("| %s | %s | %d | %d | %d | %.0f | %.4f | %.4f | %.3f | %d | %d |" % (
            w, d["pattern"], d["generation"]["cols"], st["target_unique_lines"],
            st["target_unique_pages"], st["target_working_set_bytes"] / 1024.0,
            st["target_dup_line_frac"], st["target_dup_word_frac"],
            st["target_reuse_within_window"], s["index"]["requests"], s["target"]["requests"]))
    print()
    print("| workload | reuse-distance histogram (accesses since last touch) |")
    print("|---|---|")
    for w in WORKLOADS:
        st = structure(w)["structures"]
        print("| %s | %s |" % (w, json.dumps(st["reuse_distance_hist"])))
    print()
    print("| workload | seed | trace sha256 (first 16) | index-vector sha256 (first 16) |")
    print("|---|---:|---|---|")
    for w in WORKLOADS:
        for s in (1, 2, 3):
            p = "build/wm/m21_%s_s%d_full.bin.json" % (w, s)
            if not os.path.exists(p):
                continue
            m = json.load(open(p))
            print("| %s | %d | `%s` | `%s` |" % (w, s, m["trace_sha256"][:16],
                                                m["index_vector_sha256"][:16]))


def cmd_perf(argv):
    cs = primary()
    print("| workload | seed | window | A cyc | C(noidx) | **S_faithful** | C(full) | S_same | verdict |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---|")
    for w in WORKLOADS:
        for s in (1, 2, 3):
            for W in ((8192, 16384) if s == 1 else (8192,)):
                f = cs.get((w, s, W, "full"))
                n = cs.get((w, s, W, "noidx"))
                if not f or not n:
                    print("| %s | %d | %d | — | — | *missing* | — | — | — |" % (w, s, W))
                    continue
                A, C, Cn = f["cyc"][0], f["cyc"][2], n["cyc"][2]
                sf, ss = A / Cn, A / C
                verdict = "**win**" if sf > 1.02 else ("**LOSS**" if sf < 0.98 else "parity")
                print("| %s | %d | %d | %d | %d | **%.3f** | %d | %.3f | %s |" % (
                    w, s, W, A, Cn, sf, C, ss, verdict))


def cmd_boundary(argv):
    """Structure x performance x the M19 four-arm decomposition.

    dA_idx = A(full) - A(noidx)      index-stream cost inside the SIMT path
    dC_eng = C(noidx) - A(noidx)     engine's target-side cost vs SIMT, index free
    surplus = A(full) - C(noidx)     = dA_idx - dC_eng, and S_f = 1 + surplus/C(noidx)
    """
    cs = primary()
    print("| workload | S_f 8K | S_f 16K | S_same 8K | A full | A noidx | C noidx | dA_idx | dC_eng | surplus | tgt lines | WS KiB | dup(line) | reuse<8K |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for w in WORKLOADS + ["w9_seqsmall", "w10_runresid"]:
        f, n = cs.get((w, 1, 8192, "full")), cs.get((w, 1, 8192, "noidx"))
        if not f or not n:
            continue
        f16, n16 = cs.get((w, 1, 16384, "full")), cs.get((w, 1, 16384, "noidx"))
        st = structure(w)["structures"]
        A, An, Cn, C = f["cyc"][0], n["cyc"][0], n["cyc"][2], f["cyc"][2]
        dA, dE, sur = A - An, Cn - An, A - Cn
        s16 = (f16["cyc"][0] / n16["cyc"][2]) if f16 and n16 else float("nan")
        print("| %s | %.3f | %.3f | %.3f | %d | %d | %d | %d | %d | %d | %d | %.0f | %.4f | %.3f |" % (
            w, A / Cn, s16, A / C, A, An, Cn, dA, dE, sur,
            st["target_unique_lines"], st["target_working_set_bytes"] / 1024.0,
            st["target_dup_line_frac"], st["target_reuse_within_window"]))


def _rowrate(d, idx):
    arm = "A_" if idx == 0 else "C_"
    h, c = d.get(arm + "dram_row_hits", 0), d.get(arm + "dram_row_conflicts", 0)
    return 100.0 * h / (h + c) if h + c else float("nan")


def cmd_memory(argv):
    cs = primary()
    print("| workload | arm | DRAM rd | row-hit | L1 hit% | L2 hit% | TLB miss | walks | reqs |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|")
    for w in WORKLOADS:
        for (key, d) in sorted(cs.items(), key=lambda kv: kv[0][2]):
            if key[0] != w or key[1] != 1 or key[2] != 8192:
                continue
            for idx, arm in ((0, "A"), (2, "C")):
                l1r, l1h = d.get("%s_l1_reads" % arm, 0), d.get("%s_l1_hits" % arm, 0)
                l2r, l2h = d.get("%s_l2_reads" % arm, 0), d.get("%s_l2_hits" % arm, 0)
                print("| %s | %s (%s) | %d | %.1f%% | %.1f%% | %.1f%% | %d | %d | %d |" % (
                    w, arm, key[3], d["dram"][idx], _rowrate(d, idx),
                    100.0 * l1h / l1r if l1r else 0.0,
                    100.0 * l2h / l2r if l2r else 0.0,
                    d.get("%s_tlb0_misses" % arm, 0), d["walk"][idx], d["req"][idx]))


def cmd_mechanism(argv):
    cs = primary()
    print("| workload | wraps | idx phase | tgt phase | AG cyc | slot stall | idx wait | win-full | win occ | peak slot | staging | org cyc | fanout cyc |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for w in WORKLOADS:
        for (key, d) in sorted(cs.items(), key=lambda kv: kv[0][2]):
            if key[0] != w or key[1] != 1 or key[2] != 8192 or key[3] != "full":
                continue
            g = d["m15"]
            idx_phase = d["phase"].get("e-idx", 0) - 1
            tf = g.get("bulk_first_tgt_issue", 0)
            tgt_phase = (d["phase"].get("e-tgt", 0) - tf) if tf else 0
            print("| %s | dp %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d |" % (
                w, g.get("bulk_sched_cohorts", 0), idx_phase, tgt_phase,
                g.get("bulk_ag_cycles", 0), g.get("bulk_slot_stall", 0),
                g.get("bulk_idx_wait_stall", 0), g.get("bulk_win_full_stall", 0),
                g.get("bulk_win_occ_max", 0), g.get("peak_total_outstanding", 0),
                d["rows"].get("coalescer staging (peak)", (0, 0))[0],
                g.get("bulk_org_cycles", 0), g.get("bulk_fanout_cycles", 0)))


def cmd_ablation(argv):
    a = ablations()
    print("| workload | config | A cyc | C(noidx) | S_faithful | C(full) | S_same | sched switches | sched defer | sched idle |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for w in ("w3_duphigh", "w4_duplow", "w2_clustered", "w5_sequential"):
        base = None
        for cfg in ("ctrl", "noorder", "coh1", "coh2", "coh4"):
            f = a.get((w, cfg, "full"))
            n = a.get((w, cfg, "noidx"))
            if not f or not n:
                print("| %s | %s | — | — | *missing* | — | — | — | — | — |" % (w, cfg))
                continue
            A, C, Cn = f["cyc"][0], f["cyc"][2], n["cyc"][2]
            if cfg == "ctrl":
                base = (A, C, Cn)
            g = f["m15"]
            print("| %s | %s | %d | %d | %.3f | %d | %.3f | %d | %d | %d |" % (
                w, cfg, A, Cn, A / Cn, C, A / C,
                g.get("C_bulk_sched_switches", 0), g.get("C_bulk_sched_defer", 0),
                g.get("C_bulk_sched_idle", 0)))


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    {"structure": cmd_structure, "perf": cmd_perf, "memory": cmd_memory,
     "mechanism": cmd_mechanism, "ablation": cmd_ablation,
     "boundary": cmd_boundary}.get(
        cmd, lambda a: print(__doc__))(sys.argv[2:])
