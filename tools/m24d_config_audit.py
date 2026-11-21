#!/usr/bin/env python3
"""M24-D — the exact configuration-difference audit.

Three independent checks, all on files, none on a report summary:

  A. The two pinned M24-D configs. Comment-stripped, they must parse to the same
     key set, the same key order, and to values that differ in exactly the eight
     ARCHITECTURE keys. Every resource row must also equal the value audited from
     sim/configs/*.cfg and src/sim.hpp.

  B. Each pinned file against its historical counterpart (M0-RM and G6D S1).
     Every resource row must be unchanged, and every non-resource difference must
     be a declared simulator setting or a compiled default.

  C. The simulator's own effective-configuration echo, from the primary cells.
     The echo is what the model actually ran; it must agree with check A and must
     differ between the arms in only the tokens derived from the eight keys.

Writes build/wm/m24d/config_audit.txt; exits 1 on any violation.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = os.path.join(ROOT, "build", "wm", "m24d")
S = os.path.join(ROOT, "sim", "configs")
SIMT = os.path.join(D, "m24d_simt.cfg")
BULK = os.path.join(D, "m24d_bulk.cfg")
HIST_SIMT = os.path.join(S, "m24c_m0_rm.cfg")
HIST_BULK = os.path.join(S, "m23g6b_base.cfg")

# The M23-G6D S1 cell, from build/wm/m23g6d_run.sh (w=8 c=8 m=8 l=150, bulk_org=8)
# plus the invocation every published trace cell used.
S1_CLI = {"walkers": "8", "page_cohorts": "8", "dtlb_mshr": "8", "bulk_org": "8",
          "bulk_index_base": "0x80000000", "bulk_desc_line": "0x1FFFFC0",
          "dram_diag": "1"}

# Compiled defaults for the keys the historical files do not state (src/sim.hpp).
# Only used to prove that a pinned value is the default, never to invent one.
DEFAULTS = {
    "g6i_diag": "0", "g6i_sample_period": "512", "sched_vis": "0", "sched_group": "0",
    "park_retry": "1", "bulk_stream": "0", "win_policy": "255", "win_abs_cap": "4",
    "idx_oracle": "0", "skip_b": "0", "dram_il_lines": "1", "dram_map": "0",
    "page_bytes": "4096", "dram_kind": "ddr", "m24_map": "0", "m24_perm_seed": "1",
    "m24_dram_org": "1", "dram_diag": "0", "ideal_translation": "0",
    "bulk_org": "0", "g6a_pa_capacity": "65536", "g6b_pa_capacity": "4096",
    "sched_vis": "0", "sched_group": "0", "g6i_diag": "0",
    "core_map": "round_robin",
}

# The eight keys that are the Bulk feature set and nothing else.
DECLARED = {
    "dispatch": ("0", "1"),          # no engine                 / engine
    "batch_window": ("0", "16384"),  # no window                 / 16K fused window
    "win_fused": ("0", "1"),         # no fused representation   / fused windows
    "page_sched": ("0", "1"),        # no page scheduler         / cohort scheduler
    "page_cohorts": ("0", "8"),      # no cohorts                / 8 page runs open
    "dedup": ("0", "1"),             # no fold                   / fold per line
    "page_order": ("0", "1"),        # no sealing order          / sealed page order
    "bulk_org": ("0", "8"),          # no organization           / G6D streaming, cohorts
}

# (row label, config keys, audited values, where the audited value comes from)
RESOURCES = [
    ("cores / warps per core", ["cores", "warps_per_core"], ("1", "4")),
    ("lane width / element bytes", ["lanes", "elem_bytes"], ("4", "4")),
    ("global outstanding budget R", ["R"], "128"),
    ("SIMT pending slots (LSU)", ["lsu_pending"], "0"),
    ("memory path", ["mem_path"], "l1"),
    ("L1 lines / ways / latency", ["l1_lines", "l1_ways", "l1_latency"], ("256", "4", "2")),
    ("L1 MSHRs", ["l1_mshr"], "128"),
    ("L2 lines / ways / latency", ["l2_lines", "l2_ways", "l2_latency"], ("16384", "8", "4")),
    ("L2 MSHRs", ["l2_mshr"], "64"),
    ("dTLB entries / ways", ["dtlb_entries", "dtlb_ways"], ("16", "4")),
    ("dTLB MSHRs", ["dtlb_mshr"], "8"),
    ("dTLB parked per entry", ["dtlb_parked"], "2"),
    ("L2 TLB entries / ways", ["l2tlb_entries", "l2tlb_ways"], ("512", "4")),
    ("L2 TLB MSHRs", ["l2tlb_mshr"], "8"),
    ("L2 TLB latency", ["l2tlb_latency"], "4"),
    ("page walkers", ["walkers"], "8"),
    ("walk cache entries", ["walk_cache"], "16"),
    ("page-table levels", ["levels"], "2"),
    ("walk latency per level", ["walk_latency_per_level"], "150"),
    ("ideal translation", ["ideal_translation"], "0"),
    ("DRAM banks / rows", ["banks", "rows"], ("2", "4096")),
    ("DRAM row bytes", ["row_bytes"], "1024"),
    ("DRAM interleave granule", ["dram_il_lines"], "1"),
    ("DRAM t_hit / t_miss", ["t_hit", "t_miss"], ("14", "26")),
    ("DRAM link width", ["link"], "0"),
    ("DRAM queue depth", ["dram_q"], "0"),
    ("FR-FCFS", ["frfcfs"], "1"),
    ("XOR bank hash", ["bank_hash"], "1"),
    ("DRAM address map", ["dram_map"], "0"),
    ("page size", ["page_bytes"], "4096"),
    ("memory hierarchy kind", ["dram_kind"], "ddr"),
    ("physical map / permutation seed", ["m24_map", "m24_perm_seed"], ("0", "1")),
    ("engine release organization", ["m24_dram_org"], "1"),
]

# Non-resource fields that must be equal in both pinned files (geometry, policy
# state and instrumentation). Listed so the "identical" claim is a checklist.
SETTINGS = ["core_map", "experiment", "op_class", "dispatch_order", "ag_lanes", "eng_issue",
            "idx_window", "desc_fetch", "idx_oracle", "idx_mode", "idx_buf_lines",
            "idx_mshr", "idx_reserve", "page_dir", "org_lanes", "fanout_lanes",
            "batch_deadline", "org_overlap", "park_retry", "bulk_stream", "win_policy",
            "win_abs_cap", "g6a_pa_capacity", "g6b_pa_capacity", "sched_vis",
            "sched_group", "g6i_diag", "cache_diag", "dram_diag", "pool_slots",
            "ab_charge_latch", "m23_select", "wl", "N", "Lambda", "P", "kappa",
            "rho_dup", "seed", "bulk_wl", "bulk_index_base", "bulk_data_base",
            "bulk_desc_line", "bulk_max_elems", "max_cycles", "skip_b"]

# Every non-resource difference a pinned file is allowed to have against its
# historical counterpart, and why.
DECLARED_SETTINGS = {
    "max_cycles": "simulator setting, pinned to 500 000 000 in both arms so no cell can be "
                  "truncated; the slowest cell that completes is 176 184 939, and the guard "
                  "is what identifies the one cell that does not terminate",
    "bulk_max_elems": "workload parameter (descriptor validation limit); identical in both "
                      "arms and larger than every trace, so it caps nothing",
    "bulk_index_base": "workload parameter (index-array placement); inert for SIMT, set to "
                       "the 2 GiB base every published trace cell used",
    "bulk_desc_line": "workload parameter (descriptor line); inert for SIMT",
    "dram_diag": "instrumentation, pinned on in both arms",
}

bad, lines = [], []


def out(s=""):
    lines.append(s)
    print(s)


def chk(ok, msg):
    if not ok:
        bad.append(msg)


def parse(path):
    """Comment-stripped key/value pairs, in file order."""
    d = {}
    with open(path) as f:
        for ln in f:
            t = ln.strip()
            if not t or t.startswith("#") or "=" not in t:
                continue
            k, _, v = t.partition("=")
            d[k.strip()] = v.strip()
    return d


def val(d, k):
    return d.get(k, DEFAULTS.get(k, "<unset>"))


def rows(keys, d):
    return tuple(val(d, k) for k in keys)


simt, bulk = parse(SIMT), parse(BULK)
hist_s, hist_b = parse(HIST_SIMT), dict(parse(HIST_BULK), **S1_CLI)

out("M24-D configuration audit")
out("=" * 78)
out(f"pinned SIMT : build/wm/m24d/m24d_simt.cfg   ({len(simt)} keys)")
out(f"pinned Bulk : build/wm/m24d/m24d_bulk.cfg   ({len(bulk)} keys)")
out(f"historical  : sim/configs/m24c_m0_rm.cfg, sim/configs/m23g6b_base.cfg + G6D S1 CLI")

# ---- A. the two pinned files ------------------------------------------------
out("\nA. m24d_simt.cfg vs m24d_bulk.cfg\n")
chk(list(simt) == list(bulk), "the two files do not declare the same keys in the same order")
nres = sum(len(k) for _, k, _ in RESOURCES)
chk(len(simt) == nres + len(SETTINGS) + len(DECLARED),
    f"key count {len(simt)} != resource keys {nres} + settings {len(SETTINGS)} "
    f"+ architecture {len(DECLARED)}")

diff = {k: (val(simt, k), val(bulk, k)) for k in set(simt) | set(bulk)
        if val(simt, k) != val(bulk, k)}
chk(set(diff) == set(DECLARED),
    f"differing keys {sorted(diff)} != declared {sorted(DECLARED)}")
for k in sorted(diff):
    chk(diff[k] == DECLARED.get(k, diff[k]), f"{k}: {diff[k]} != declared {DECLARED.get(k)}")

out(f"{'field':32s} {'SIMT':>8s} {'Bulk':>8s}  role")
for k in DECLARED:
    out(f"{k:32s} {val(simt, k):>8s} {val(bulk, k):>8s}  architecture")
chk(len(diff) == 8, f"{len(diff)} differing keys, expected 8")

# ---- resources: equal in both files and equal to the audited value ---------
out(f"\n{'resource':34s} {'value':>18s}  {'SIMT':>6s} {'Bulk':>6s} {'M0-RM':>9s} {'G6D S1':>9s}")
for label, keys, want in RESOURCES:
    want = want if isinstance(want, tuple) else (want,)
    vs, vb, vh, vg = rows(keys, simt), rows(keys, bulk), rows(keys, hist_s), rows(keys, hist_b)
    shown = " / ".join(want)
    out(f"{label:34s} {shown:>18s}  "
        f"{'/'.join(vs):>6s} {'/'.join(vb):>6s} {'/'.join(vh):>9s} {'/'.join(vg):>9s}")
    chk(vs == want, f"resource {label}: pinned SIMT {vs} != audited {want}")
    chk(vb == want, f"resource {label}: pinned Bulk {vb} != audited {want}")
    chk(vh == want, f"resource {label}: historical M0-RM {vh} != audited {want}")
    chk(vg == want, f"resource {label}: historical G6D S1 {vg} != audited {want}")

# ---- settings --------------------------------------------------------------
out("\nsettings identical in both pinned files")
for k in SETTINGS:
    chk(val(simt, k) == val(bulk, k), f"setting {k}: {val(simt, k)} != {val(bulk, k)}")
out("  " + ", ".join(f"{k}={val(simt, k)}" if val(simt, k) else f"{k}=" for k in SETTINGS))

# ---- B. against the historical files ---------------------------------------
arch_hist = {"simt": {"dispatch": "0", "batch_window": "0", "win_fused": "0",
                      "page_sched": "0", "page_cohorts": "0", "dedup": "0",
                      "page_order": "0", "bulk_org": "0"},
             "bulk": {"dispatch": "1", "batch_window": "16384", "win_fused": "1",
                      "page_sched": "1", "page_cohorts": "8", "dedup": "1",
                      "page_order": "1", "bulk_org": "8"}}
for name, pinned, hist in (("SIMT", simt, hist_s), ("Bulk", bulk, hist_b)):
    out(f"\nB. pinned {name} vs historical {name}")
    for k in SETTINGS + list(DECLARED):
        a, b = val(pinned, k), val(hist, k)
        if a == b:
            continue
        if k in DECLARED:
            chk(arch_hist[name.lower()][k] == a,
                f"{name}: architecture {k}={a} is not the historical value")
            out(f"  architecture  {k:22s} {b:>10s} -> {a:>10s}  (the feature set, by design)")
        else:
            chk(k in DECLARED_SETTINGS, f"{name}: undeclared difference in {k} ({b} -> {a})")
            out(f"  setting       {k:22s} {b:>10s} -> {a:>10s}  {DECLARED_SETTINGS.get(k, '?')}")

# ---- C. the simulator's own effective configuration ------------------------
# Substrings the echo must contain in BOTH arms. The echo prints the compiled
# capacities the model actually ran with, so this is the resource claim checked
# against the run rather than against the config file it was built from.
ECHO_REQ = [
    "cores=1 warps=4 lanes=4 R=128",
    "l1=256x4/2cy mshr=128",
    "l2=16384x8/4cy mshr=64",
    "dtlb=16x4 mshr=8 parked=2",
    "l2tlb=512x4 mshr=8 lat=4",
    "walkers=8 wc=16 walk_lat=150x2",
    "dram=2 banks x 4096 rows, hit=14 miss=26 link=0 q=0 frfcfs hash lsu_pending=0",
    "page_bytes=4096",
    "m24_map=0",
]
def echo(path):
    txt = open(path).read()
    blk = txt.split("== configuration ==")[1].split("== ") [0]
    d = {}
    for tok in blk.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            d[k] = v
    return d


out("\nC. effective configuration as the model ran it")
bases = {}
for wl in ("orkut", "livejournal", "ogbn", "criteo"):
    ps, pb = os.path.join(D, f"m24d_simt_{wl}.txt"), os.path.join(D, f"m24d_bulk_{wl}.txt")
    if not (os.path.exists(ps) and os.path.exists(pb)):
        bad.append(f"missing primary cell {wl}")
        continue
    es, eb = echo(ps), echo(pb)
    d = {k: (es.get(k), eb.get(k)) for k in set(es) | set(eb) if es.get(k) != eb.get(k)}
    bases[wl] = d
    # `window` is batch_window, `win_policy` is derived from dispatch, and
    # page_sched/page_cohorts are not echoed as separate tokens.
    want = {"dispatch", "window", "win_policy", "win_fused", "dedup", "page_order",
            "bulk_org"}
    out(f"  {wl:12s} {len(es)} echoed keys, {len(d)} differ: {', '.join(sorted(d))}")
    chk(set(d) == want, f"{wl}: echo differs in {sorted(d)} != {sorted(want)}")
    for frag in ECHO_REQ:
        chk(frag in open(ps).read().split("== configuration ==")[1].split("== ")[0],
            f"{wl}: SIMT echo lacks '{frag}'")
        chk(frag in open(pb).read().split("== configuration ==")[1].split("== ")[0],
            f"{wl}: Bulk echo lacks '{frag}'")

known = {"dispatch", "window", "win_policy", "win_fused", "dedup", "page_order", "bulk_org"}
for wl, d in bases.items():
    chk(set(d) <= known | {"dispatch"}, f"{wl}: unexpected echoed difference")

out(f"\n{len(simt)} keys: {nres} resource keys in {len(RESOURCES)} rows, "
    f"{len(SETTINGS)} settings, {len(DECLARED)} architecture fields")
out("\n" + ("AUDIT PASSED: 0 common-resource mismatches" if not bad
            else f"AUDIT FAILURES: {len(bad)}"))
for b in bad:
    out("   " + b)
with open(os.path.join(D, "config_audit.txt"), "w") as f:
    f.write("\n".join(lines) + "\n")
sys.exit(1 if bad else 0)
