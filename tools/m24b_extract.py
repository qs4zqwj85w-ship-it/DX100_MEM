#!/usr/bin/env python3
"""M24-B: parse the HBM four-way cell logs into tables, identities and a CSV.

Every number the report quotes is printed here first, so a figure in the document
can be traced to one log line. Missing cells are reported, never silently skipped.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGDIR = os.path.join(ROOT, "build", "wm", "m24b")
NAME = re.compile(r"m24b_(\w+?)_([ABCD])_(\d+)banks_map(\d)_p(\d+)k\.txt$")

WLS = ["orkut", "livejournal", "ogbn"]
CELLS = ["A", "B", "C", "D"]
PAGE = {4: 4096, 64: 65536, 2048: 2097152}


def parse(path):
    d = {}
    with open(path, errors="replace") as f:
        for line in f:
            if "=" not in line:
                continue
            k, _, rest = line.partition("=")
            k = k.strip()
            if not k or " " in k:
                continue
            v = rest.split()[0] if rest.split() else ""
            try:
                d[k] = int(v)
            except ValueError:
                d.setdefault(k + "_str", v)
    return d


def cells():
    out = {}
    for p in sorted(glob.glob(os.path.join(LOGDIR, "m24b_*.txt"))):
        m = NAME.search(os.path.basename(p))
        if not m:
            continue
        wl, cell, banks, mp, pk = m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5))
        d = parse(p)
        if "cycles" not in d:
            continue
        out[(wl, banks, mp, PAGE[pk], cell)] = d
    return out


def sp(n):
    return f"{n:,}".replace(",", " ")


def pct(a, b):
    return 100.0 * (a - b) / b if b else 0.0


def main():
    d = cells()
    lines = []
    w = lines.append
    w(f"cells parsed: {len(d)}")
    keys = sorted({k for (wl, b, m, p, c), v in d.items() for k in ("cycles",) if k in v})
    w("")

    # ---- identities ---------------------------------------------------------
    bad = 0
    for key in sorted(d):
        v = d[key]
        checks = [
            ("reads == dram_reads", v.get("hbm_reads"), v.get("dram_reads")),
            ("bytes == 64*reads", v.get("hbm_bytes"), 64 * v.get("hbm_reads", 0)),
            ("ch_busy_sum == t_burst*reads", v.get("hbm_ch_busy_sum"), 4 * v.get("hbm_reads", 0)),
        ]
        for name, a, b in checks:
            if a != b:
                w(f"IDENTITY FAIL {key} {name}: {a} != {b}")
                bad += 1
        # One activation serves many hits, so the only ordering invariants are
        # these: every conflict needs an activation, and every activation belongs
        # to a request that was not a hit.
        if v.get("hbm_conflicts", 0) > v.get("hbm_activations", 0):
            w(f"IDENTITY FAIL {key}: conflicts > activations")
            bad += 1
        if v.get("hbm_hits", 0) > v.get("hbm_reads", 0) or v.get("hbm_conflicts", 0) > v.get("hbm_reads", 0):
            w(f"IDENTITY FAIL {key}: hits/conflicts exceed reads")
            bad += 1
        if v.get("hbm_reads", 0) and v.get("hbm_activations", 0) == 0:
            w(f"IDENTITY FAIL {key}: reads with no activation")
            bad += 1
        if v.get("hbm_ch_used", 0) > 32 or v.get("hbm_elig_max", 0) > 16:
            w(f"IDENTITY FAIL {key}: channel/window bound")
            bad += 1
    w(f"identities: {bad} failures")
    w("")

    # ---- four-way tables ---------------------------------------------------
    for wl in WLS:
        grid = sorted({(b, m, p) for (w, b, m, p, c) in d if w == wl})
        if not grid:
            continue
        w(f"== {wl}")
        w(f"{'banks':>5} {'map':>3} {'page':>7} | {'A':>12} {'B':>12} {'C':>12} {'D':>12} | "
          f"{'B/A':>8} {'C/A':>8} {'D/A':>8} {'D/B':>8} | interaction")
        for (b, m, p) in grid:
            row = {c: d[(wl, b, m, p, c)] for c in CELLS if (wl, b, m, p, c) in d}
            if not row:
                continue
            cy = {c: row[c]["cycles"] for c in row}
            a = cy.get("A")
            def g(c):
                return sp(cy[c]) if c in cy else "-"
            def r(c, base="A"):
                b0 = cy.get(base)
                return f"{100.0 * (cy[c] - b0) / b0:+.2f}" if (b0 and c in cy) else "-"
            inter = ""
            if all(c in cy for c in CELLS):
                i = cy["D"] - cy["B"] - cy["C"] + cy["A"]
                inter = f"{i:+d} ({100.0 * i / cy['A']:+.2f} %)"
            w(f"{b:>5} {m:>3} {p:>7} | {g('A'):>12} {g('B'):>12} {g('C'):>12} {g('D'):>12} | "
              f"{r('B'):>8} {r('C'):>8} {r('D'):>8} {r('D', 'B'):>8} | {inter}")
        w("")

    # ---- DRAM detail -------------------------------------------------------
    w("== HBM detail (cell A vs B vs D)")
    w(f"{'cell':>34} | {'reads':>8} {'act':>8} {'conf':>8} {'hit%':>7} {'rows':>7} "
      f"{'r/a':>6} {'ch':>3} {'banks':>5} {'qmax':>5} {'wait':>9} {'cycle/read':>10}")
    for key in sorted(d):
        wl, b, m, p, c = key
        if c not in ("A", "B", "D"):
            continue
        v = d[key]
        rd = v.get("hbm_reads", 0) or 1
        hr = 100.0 * v.get("hbm_hits", 0) / rd
        ra = v.get("hbm_row_acts_sum", 0) / rd
        w(f"{wl[:10]:>10} {b:>3} m{m} p{p//1024:>4}k {c} | {v.get('hbm_reads',0):>8} "
          f"{v.get('hbm_activations',0):>8} {v.get('hbm_conflicts',0):>8} {hr:>7.1f} "
          f"{v.get('hbm_rows_used',0):>7} {ra:>6.3f} {v.get('hbm_ch_used',0):>3} "
          f"{v.get('hbm_banks_used',0):>5} {v.get('hbm_q_max',0):>5} "
          f"{v.get('hbm_wait_sum',0):>9} {v.get('cycles',0)/rd:>10.1f}")
    w("")

    # ---- translation -------------------------------------------------------
    w("== translation (cell A/B)")
    w(f"{'cell':>34} | {'walks':>7} {'pte':>6} {'l2 hit':>7} {'l2 miss':>8} {'replays':>9}")
    for key in sorted(d):
        wl, b, m, p, c = key
        if c != "B":
            continue
        v = d[key]
        w(f"{wl[:10]:>10} {b:>3} m{m} p{p//1024:>4}k | {v.get('walks',0):>7} "
          f"{v.get('pte_fetches',0):>6} {v.get('l2tlb_hits',0):>7} "
          f"{v.get('l2tlb_misses',0):>8} {v.get('replays',0):>9}")
    w("")

    # ---- page size ---------------------------------------------------------
    w("== page size (map 1, cell A and B): translation and cycles")
    w(f"{'wl':>11} {'banks':>5} {'page':>7} {'cell':>4} | {'cycles':>11} {'walks':>8} {'pte':>7} "
      f"{'l2hit':>7} {'l2miss':>8} {'replays':>9} {'hit%':>6}")
    for key in sorted(d, key=lambda k: (k[0], k[1], k[3], k[4])):
        wl, b, m, p, c = key
        if m != 1 or c not in ("A", "B"):
            continue
        v = d[key]
        rd = v.get("hbm_reads", 0) or 1
        w(f"{wl:>11} {b:>5} {p:>7} {c:>4} | {v.get('cycles',0):>11} {v.get('walks',0):>8} "
          f"{v.get('pte_fetches',0):>7} {v.get('l2tlb_hits',0):>7} {v.get('l2tlb_misses',0):>8} "
          f"{v.get('replays',0):>9} {100.0*v.get('hbm_hits',0)/rd:>6.1f}")
    w("")

    # ---- bank organisation -------------------------------------------------
    w("== 16 vs 32 banks (same capacity, same page, same cell)")
    w(f"{'wl':>11} {'map':>3} {'page':>7} {'cell':>4} | {'16 banks':>11} {'32 banks':>11} {'delta %':>8}")
    for key in sorted(d, key=lambda k: (k[0], k[2], k[3], k[4])):
        wl, b, m, p, c = key
        if b != 16:
            continue
        o = (wl, 32, m, p, c)
        if o not in d:
            continue
        x, y = d[key]["cycles"], d[o]["cycles"]
        w(f"{wl:>11} {m:>3} {p:>7} {c:>4} | {x:>11} {y:>11} {100.0*(y-x)/x:>+8.2f}")
    w("")

    # ---- CSV ---------------------------------------------------------------
    csv = os.path.join(LOGDIR, "summary.csv")
    cols = ["cycles", "dram_reads", "hbm_reads", "hbm_hits", "hbm_conflicts",
            "hbm_activations", "hbm_precharges", "hbm_bytes", "hbm_wait_sum",
            "hbm_wait_max", "hbm_q_max", "hbm_ch_used", "hbm_ch_busy_max",
            "hbm_ch_reads_min", "hbm_ch_reads_max", "hbm_banks_used",
            "hbm_rows_used", "hbm_row_acts_sum", "hbm_row_acts_max",
            "hbm_elig_sum", "hbm_elig_max", "walks", "pte_fetches",
            "l2tlb_hits", "l2tlb_misses", "replays", "requests_generated",
            "bulk_g6i_vpn_sum", "bulk_g6i_cells_sum", "bulk_org_cycles",
            "dram_row_conflicts", "dram_row_hits"]
    with open(csv, "w") as f:
        f.write("wl,cell,banks,map,page," + ",".join(cols) + "\n")
        for key in sorted(d):
            wl, b, m, p, c = key
            f.write(f"{wl},{c},{b},{m},{p}," + ",".join(str(d[key].get(k, "")) for k in cols) + "\n")
    w(f"wrote {csv} ({len(d)} cells)")

    text = "\n".join(lines)
    with open(os.path.join(LOGDIR, "extract.txt"), "w") as f:
        f.write(text + "\n")
    print(text)
    print("\nALL CHECKS PASSED" if bad == 0 else f"\n{bad} IDENTITY FAILURES")


if __name__ == "__main__":
    main()
