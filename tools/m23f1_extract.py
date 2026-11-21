#!/usr/bin/env python3
"""M23-F1: parse every experiment log in build/wm/m23f1/ into one table.

Reads the `key=value` counter lines the simulator prints with --all. Emits
build/wm/m23f1/summary.csv and prints the two tables used by the report.

Nothing here re-simulates or edits anything; the logs are the ground truth.
"""
import glob
import os
import re
import sys

FIELDS = """cycles walks walker_busy walker_max pte_fetches walk_wc_hits
requests_generated completed accesses instrs lines pages
l1_reads l1_hits l1_misses l1_mshr_max l2_reads l2_hits l2_misses l2_mshr_max
dram_reads dram_bytes dram_row_hits dram_row_conflicts dram_row_acts_sum
dram_row_acts_max dram_rows_used dram_busy_cycles dram_starved_cycles
inflight_max inflight_occ simt_peak_slot simt_peak_outstanding unissued_max
tlb0_reads tlb0_hits tlb0_misses tlb0_mshr_occ tlb0_mshr_max
tlb_refuse_full tlb_refuse_parked tlb_max_parked
l2tlb_reads l2tlb_hits l2tlb_misses l2tlb_max_occ l2tlb_walk_stalls
lat_tlb_mean lat_cache_mean lat_mean_x100 replays
bulk_elems bulk_instr simt_instr bulk_win_occ_max bulk_win_size_mean
bulk_org_cycles bulk_fanout_cycles bulk_ag_cycles bulk_slot_stall
conservation""".split()


def parse(path):
    d = {}
    for line in open(path, errors="replace"):
        s = line.strip()
        if "=" not in s or s.startswith("=="):
            continue
        for tok in s.split():
            if "=" not in tok:
                continue
            k, v = tok.split("=", 1)
            if k in FIELDS and k not in d:
                try:
                    d[k] = float(v)
                except ValueError:
                    d[k] = v
    return d


def main():
    root = os.path.join(os.path.dirname(__file__), "..", "build", "wm", "m23f1")
    root = os.path.normpath(root)
    rows = []
    for f in sorted(glob.glob(os.path.join(root, "*.txt"))):
        tag = os.path.basename(f)[:-4]
        if tag.startswith(("identity", "timing")):
            continue
        d = parse(f)
        if "cycles" not in d:
            continue
        d["tag"] = tag
        rows.append(d)

    cols = ["tag"] + FIELDS
    with open(os.path.join(root, "summary.csv"), "w") as out:
        out.write(",".join(cols) + "\n")
        for r in rows:
            out.write(",".join(str(r.get(c, "")) for c in cols) + "\n")
    print(f"wrote {os.path.join(root, 'summary.csv')} ({len(rows)} rows)")

    def num(r, k):
        v = r.get(k, "")
        return v if isinstance(v, str) else int(v)

    # ---- Phase 1 table -------------------------------------------------------
    p1 = [r for r in rows if re.fullmatch(r"[bcdu]_(m0|dx16k)_(normal|ideal)", r["tag"])]
    if p1:
        by = {(r["tag"].split("_")[0], r["tag"].split("_")[1], r["tag"].split("_")[2]): r
              for r in p1}
        print("\nid  | M0 normal | DX normal | normal sp | M0 ideal | DX ideal | ideal sp")
        for w in sorted({k[0] for k in by}):
            try:
                a = by[(w, "m0", "normal")]; b = by[(w, "dx16k", "normal")]
                c = by[(w, "m0", "ideal")]; d = by[(w, "dx16k", "ideal")]
            except KeyError:
                continue
            print(f"{w}   | {int(a['cycles']):>9} | {int(b['cycles']):>9} | "
                  f"{a['cycles']/b['cycles']:9.3f} | {int(c['cycles']):>8} | "
                  f"{int(d['cycles']):>8} | {c['cycles']/d['cycles']:8.3f}")

    # ---- Phase 2 table -------------------------------------------------------
    p2 = [r for r in rows if re.search(r"_m\d+w\d+$", r["tag"])]
    if p2:
        print("\nPhase 2 (normal translation), cycles / walker_max / walker util")
        for r in sorted(p2, key=lambda x: x["tag"]):
            cyc = int(r["cycles"])
            wb = r.get("walker_busy", 0)
            print(f"{r['tag']:22} {cyc:>12} wm={int(r.get('walker_max', 0))} "
                  f"avgW={wb/cyc:5.2f} tw={int(r.get('walks', 0)):>8} "
                  f"infl_max={int(r.get('inflight_max', 0)):>3} "
                  f"unissued={int(r.get('unissued_max', 0)):>3} "
                  f"cons={r.get('conservation', '?')}")


if __name__ == "__main__":
    sys.exit(main())
