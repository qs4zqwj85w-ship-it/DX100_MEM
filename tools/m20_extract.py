#!/usr/bin/env python3
"""M20 metric extraction.

Parses the audited M19 cell parser (tools/m19_recompute.py) and adds the M20
metric families: width trend, memory behaviour, engine mechanism, and the
index-stream / target-gather split.

  python3 tools/m20_extract.py summary     # one row per matrix x width x window
  python3 tools/m20_extract.py memory      # DRAM / cache / TLB per arm
  python3 tools/m20_extract.py mechanism   # engine phase, occupancy, stalls
  python3 tools/m20_extract.py split       # index vs gather contribution
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m19_recompute as R  # audited parser; verified to reproduce M18 to the digit

CELL = re.compile(r"^m20_(?P<m>.+?)_(?P<v>full|noidx)_w(?P<w>\d+)_l(?P<L>\d+)\.txt$")
RE_TABLE = re.compile(r"^  ([A-Za-z][A-Za-z0-9 /()\-]*?)\s{2,}(\d+)\s+(\d+)\s+(\d+)\s*$")


RE_CD = re.compile(r"^  (simt|e-idx|e-tgt|walk)\s+" + r"\s+".join([r"(\d+)"] * 13) + r"\s*$")


def load(path):
    d = R.read_cell(path)
    d["rows"] = {}
    d["phase"] = {}
    d["cd"] = {}
    for line in open(path, errors="replace"):
        m = RE_CD.match(line)
        if m:
            # groups: 2..14 = L1r L1h L1m L2r L2h L2m L2ev evx hitx DRAMrd issued done lastdone
            g = [int(m.group(i)) for i in range(2, 15)]
            d["phase"][m.group(1)] = g[12]          # lastdone
            d["cd"][m.group(1)] = {"l1r": g[0], "l2r": g[3], "l2m": g[5],
                                   "l2ev": g[6], "issued": g[10], "lastdone": g[12]}
            continue
        m = re.match(r"^  m16 ([A-Za-z0-9_]+)=(-?\d+)\s*$", line)
        if m:
            d["m15"][m.group(1)] = int(m.group(2))   # arm counters live here too
            continue
        m = RE_TABLE.match(line)
        if m and not re.search(r"\d", m.group(1).split()[-1]):
            d["rows"][m.group(1).strip()] = (int(m.group(2)), int(m.group(4)))
    d.update(d["m15"])  # arm counters (m16 A_/C_) readable at top level too
    return d


def cells():
    out = {}
    for name in sorted(os.listdir("build/wm")):
        m = CELL.match(name)
        if not m:
            continue
        p = os.path.join("build/wm", name)
        d = load(p)
        if "cyc" not in d:
            continue
        key = (m.group("m"), int(m.group("L")), int(m.group("w")), m.group("v"))
        out[key] = d
    return out


def dual(d, row, idx):
    """A/C value of a table row, e.g. ('DRAM reads', 0/1)."""
    v = d["rows"].get(row)
    return v[idx] if v else 0


def m15(d, k):
    return d["m15"].get(k, 0)


def row_hit(d, idx):
    hits = m15(d, "dram_row_hits")
    confl = m15(d, "dram_row_conflicts")
    arm = "A_" if idx == 0 else "C_"
    h, c = d.get(arm + "dram_row_hits", 0), d.get(arm + "dram_row_conflicts", 0)
    if h + c == 0:
        return float("nan")
    return 100.0 * h / (h + c)


def cmd_summary(argv):
    cs = cells()
    mats = ["bcsstk25", "qc2534", "lnsp3937", "dubcova3"]
    print("| matrix | width | window | A cyc | C cyc | **S_faithful** | C(noidx) cyc | S_same | Bulk wins? |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---|")
    rows = []
    for m in mats:
        for L in (4, 8, 16, 32):
            for w in (8192, 16384):
                f = cs.get((m, L, w, "full"))
                n = cs.get((m, L, w, "noidx"))
                if not f or not n:
                    print("| %s | %d | %d | — | — | *missing* | — | — | — |" % (m, L, w))
                    continue
                a, c = f["cyc"][0], f["cyc"][2]
                cn = n["cyc"][2]
                sf = a / cn if cn else float("nan")
                ss = a / c if c else float("nan")
                wins = "**win**" if sf > 1.0 else "loss"
                print("| %s | %d | %d | %d | %d | **%.3f** | %d | %.3f | %s |" % (
                    m, L, w, a, c, sf, cn, ss, wins))
                rows.append({"matrix": m, "lanes": L, "window": w, "A": a, "C": c,
                             "C_noidx": cn, "S_f": sf, "S_same": ss})
    with open("build/wm/m20_summary.json", "w") as fp:
        json.dump(rows, fp, indent=1)


def cmd_memory(argv):
    cs = cells()
    print("| matrix | w | L | arm | cycles | DRAM rd | row-hit | L1 rd/hit% | L2 rd/hit% | TLB miss | walks | reqs |")
    print("|---|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for (m, L, w, v), d in sorted(cs.items()):
        if v != "full":
            continue
        for idx, arm in ((0, "A"), (2, "C")):
            l1r, l1h = d.get("%s_l1_reads" % arm, 0), d.get("%s_l1_hits" % arm, 0)
            l2r, l2h = d.get("%s_l2_reads" % arm, 0), d.get("%s_l2_hits" % arm, 0)
            print("| %s | %d | %d | %s | %d | %d | %.1f%% | %d/%.1f%% | %d/%.1f%% | %d | %d | %d |" % (
                m, w, L, arm, d["cyc"][idx], d["dram"][idx], row_hit(d, idx),
                l1r, 100.0 * l1h / l1r if l1r else 0.0,
                l2r, 100.0 * l2h / l2r if l2r else 0.0,
                d.get("%s_tlb0_misses" % arm, 0), d["walk"][idx], d["req"][idx]))


def cmd_mechanism(argv):
    cs = cells()
    print("| matrix | w | L | idx req | idx phase | idx wait | AG cyc | tgt req | tgt DRAM | tgt phase | win occ | peak slot | L1 mshr | L2 mshr | staging |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for (m, L, w, v), d in sorted(cs.items()):
        if v != "full":
            continue
        # Index acquisition runs from the first index request (cycle 1, from the
        # G4 line) to the last index response; the target gather from the first
        # target issue to the last target response. Both ends are counters.
        idx_phase = d["phase"].get("e-idx", 0) - 1
        tgt_first = m15(d, "bulk_first_tgt_issue")
        tgt_phase = (d["phase"].get("e-tgt", 0) - tgt_first) if tgt_first else 0
        tgt_dram = 0
        for row_name, vals in (("e-tgt", d.get("cd", {})),):
            if row_name in vals:
                tgt_dram = vals[row_name]["l2m"]
        print("| %s | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d |" % (
            m, w, L, d["idx"][2] // 64, idx_phase, m15(d, "bulk_idx_wait_stall"),
            m15(d, "bulk_ag_cycles"), d["tgt"][2], tgt_dram, tgt_phase,
            m15(d, "bulk_win_occ_max"), m15(d, "peak_total_outstanding"),
            d.get("C_l1_mshr_max", 0), d.get("C_l2_mshr_max", 0),
            d["rows"].get("coalescer staging (peak)", (0, 0))[0]))


def cmd_split(argv):
    print("| matrix | width | dA_idx | dC_eng | dC_dup | S_f | index share of dA |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    cs = cells()
    for m in ("bcsstk25", "qc2534", "lnsp3937", "dubcova3"):
        for L in (4, 8, 16, 32):
            for w in (8192, 16384):
                f = cs.get((m, L, w, "full"))
                n = cs.get((m, L, w, "noidx"))
                if not f or not n:
                    continue
                dA = f["cyc"][0] - n["cyc"][0]
                dE = n["cyc"][2] - n["cyc"][0]
                dD = f["cyc"][2] - n["cyc"][2]
                sf = f["cyc"][0] / n["cyc"][2]
                print("| %s | %d (w%d) | %d | %d | %d | %.3f | %.0f%% |" % (
                    m, L, w, dA, dE, dD, sf, 100.0 * dA / f["cyc"][0]))


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    {"summary": cmd_summary, "memory": cmd_memory,
     "mechanism": cmd_mechanism, "split": cmd_split}.get(cmd, lambda a: print(__doc__))(sys.argv[2:])
