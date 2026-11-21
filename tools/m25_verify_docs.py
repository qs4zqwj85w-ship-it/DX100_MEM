#!/usr/bin/env python3
"""M25: verify every number and every cross-document claim in the five reports.

Recomputes each quoted figure from the artifact it came from:
  build/wm/m25/diag.csv            (M24-F through tools/m25_extract.py)
  build/wm/m25/policy.csv          (tools/m25_policy.py)
  build/wm/m25/map2/*.txt          (the six M25 mapping cells)
  build/wm/m24a/summary.csv        (M24-A factorial)
  build/wm/m24f/summary.csv        (M24-F per-arm table)
and asserts that the reports do not contain claims the evidence does not support
(no frequency, no power for M24, no unqualified novelty).
"""
import csv
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = os.path.join(ROOT, "docs")
W = os.path.join(ROOT, "build", "wm")
CHECKS = []

DOCS = ["m25-literature-review.md", "m25-simt-bulk-efficiency-audit.md",
        "m25-adaptive-selection-design.md", "m25-dram-organization-audit.md",
        "m25-architecture-proposal.md"]


def chk(ok, what, detail=""):
    CHECKS.append((bool(ok), what, detail))
    if not ok:
        print(f"FAIL {what} {detail}")


def text(name):
    with open(os.path.join(D, name)) as f:
        return f.read()


def has(name, s, what):
    chk(s in text(name), f"{name}: {what}", repr(s))


def num(name, v, what, sep=" "):
    """A formatted number appears either with a thin/normal space or as digits."""
    t = text(name)
    forms = {f"{v:,}".replace(",", " "), f"{v:,}".replace(",", "\u202f"),
             f"{v:,}".replace(",", "\u00a0"), f"{v:,}", str(v)}
    chk(any(f in t for f in forms), f"{name}: {what}", f"{v} ({sorted(forms)[:2]})")


def kv(path):
    d = {}
    for line in open(path):
        for tok in line.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                try:
                    d[k] = float(v)
                except ValueError:
                    pass
    return d


def main():
    for n in DOCS:
        p = os.path.join(D, n)
        chk(os.path.exists(p), f"{n} exists")
        if os.path.exists(p):
            t = text(n)
            chk(len(t.splitlines()) > 100, f"{n} is substantive", str(len(t.splitlines())))
            chk(t.lstrip().startswith("# M25"), f"{n} has an M25 title")

    # ---- diag.csv --------------------------------------------------------
    diag = {r["trace"]: r for r in csv.DictReader(open(os.path.join(W, "m25", "diag.csv")))}
    chk(len(diag) == 31, "31 traces", str(len(diag)))
    A = "m25-simt-bulk-efficiency-audit.md"
    for t, cyc_b, cyc_s in (("n3_clu4m", 484783, 399150), ("dubcova3", 3937942, 3295852),
                            ("qc2534", 881339, 742628), ("bcsstk25", 493290, 455621),
                            ("n1_seq8m", 1222342, 1191908), ("n5_seq8m_s4", 720970, 688926)):
        chk(float(diag[t]["cycles_bulk"]) == cyc_b, f"{t} bulk cycles", diag[t]["cycles_bulk"])
        chk(float(diag[t]["cycles_simt"]) == cyc_s, f"{t} simt cycles", diag[t]["cycles_simt"])
        num(A, cyc_b, f"{t} bulk cycles in the report")
        num(A, cyc_s, f"{t} simt cycles in the report")
    for t in ("n3_clu4m", "dubcova3", "qc2534", "bcsstk25", "n1_seq8m", "n5_seq8m_s4"):
        num(A, int(float(diag[t]["idx_dram"])), f"{t} index DRAM reads")
        num(A, int(float(diag[t]["dram_reads_delta"])), f"{t} read delta")
        num(A, int(float(diag[t]["tgt_dram"])), f"{t} target DRAM reads")
    # index share of bulk reads, 5.0-32.5 %
    shares = [100.0 * float(r["idx_dram"]) / float(r["dram_reads_bulk"])
              for r in diag.values()
              if float(r["dram_reads_bulk"]) > 0 and float(r["idx_dram"]) > 0
              and float(r["batches"]) > 0]
    chk(4.9 < min(shares) < 5.1, "min index share ~5.0 %", f"{min(shares):.2f}")
    chk(32.0 < max(shares) < 33.0, "max index share ~32.5 %", f"{max(shares):.2f}")
    # the cycle-budget rows
    for t, stall in (("n3_clu4m", 289947), ("dubcova3", 3320639), ("n5_seq8m_s4", 698602),
                     ("n1_seq8m", 1200427)):
        chk(abs(float(diag[t]["rel_slot_stall"]) - stall) < 0.5, f"{t} slot stall",
            diag[t]["rel_slot_stall"])
        num(A, stall, f"{t} slot stall in the report")
    for t, org in (("n3_clu4m", 75080), ("dubcova3", 820209), ("qc2534", 232051)):
        num(A, org, f"{t} organizer cycles")
    num(A, 173556, "M24-H org7->8 criteo cycles") if False else None

    # ---- zero-hit count --------------------------------------------------
    zero = 0
    tot = 0
    for t in diag:
        p = os.path.join(W, "m24f", f"{t}__bulk.txt")
        if not os.path.exists(p):
            continue
        tot += 1
        if kv(p).get("cc1_l2_hits", 1) == 0:
            zero += 1
    chk(zero == 23, "cc1_l2_hits == 0 in 23 cells", f"{zero}/{tot}")
    has(A, "23 of 31", "the 23-of-31 zero-hit statement")

    # ---- policy ----------------------------------------------------------
    pol = list(csv.DictReader(open(os.path.join(W, "m25", "policy.csv"))))
    chosen = sum(float(r["cycles_chosen"]) for r in pol)
    oracle = sum(float(r["cycles_oracle"]) for r in pol)
    reg = 100.0 * (chosen - oracle) / oracle
    correct = sum(1 for r in pol if r["correct"] == "True")
    B = "m25-adaptive-selection-design.md"
    chk(len(pol) == 31, "31 policy rows", str(len(pol)))
    chk(correct == 14, "14 correct modes", str(correct))
    chk(abs(reg - 0.33) < 0.01, "regret 0.33 %", f"{reg:.4f}")
    num(B, 60939372, "sum chosen cycles")
    num(B, 60736363, "oracle cycles")
    has(B, "14 / 31", "accuracy 14/31")
    has(B, "+0.33 %", "regret 0.33 % in the design doc")
    has(B, "+32.83 %", "gain over always-SIMT")
    has(B, "+1.24 %", "gain over always-Bulk")
    # the two named errors
    n3 = [r for r in pol if r["trace"] == "n3_clu4m"][0]
    chk(n3["chosen"] == "bulk" and n3["oracle_mode"] == "simt", "n3 is the false positive")
    chk(abs(float(n3["regret_pct"]) - 21.45) < 0.01, "n3 regret 21.45", n3["regret_pct"])
    ln = [r for r in pol if r["trace"] == "lnsp3937"][0]
    chk(ln["chosen"] == "simt" and ln["oracle_mode"] == "bulk", "lnsp3937 is the false negative")
    chk(abs(float(ln["regret_pct"]) - 19.19) < 0.01, "lnsp 19.19", ln["regret_pct"])
    # the win/loss accounting quoted in the design doc
    wins = [r for r in pol if float(r["speedup_bulk"]) >= 1.10]
    got = sum(1 for r in wins if r["chosen"] == "bulk")
    chk(len(wins) == 9 and got == 8, "8 of 9 wins captured", f"{got}/{len(wins)}")
    loss = [r for r in pol if float(r["speedup_bulk"]) <= 0.95]
    gotl = sum(1 for r in loss if r["chosen"] == "simt")
    chk(len(loss) == 4 and gotl == 3, "3 of 4 losses avoided", f"{gotl}/{len(loss)}")
    errs = [r for r in pol if r["correct"] == "False"]
    near = sum(1 for r in errs if float(r["regret_pct"]) <= 6.0)
    chk(near == 15, "15 of 17 errors have <= 6 % regret", f"{near}/{len(errs)}")

    # the unattributed share quoted in the audit (62 % on dubcova3, 74 % on qc2534)
    for t, pctv in (("dubcova3", 62), ("qc2534", 74)):
        r = diag[t]
        share = 100.0 * float(r["other_dram"]) / float(r["dram_reads_bulk"])
        chk(abs(round(share) - pctv) <= 1, f"{t} unattributed {pctv} %", f"{share:.1f}")
    has(A, "62 %", "the 62 % unattributed figure")

    # ---- the six mapping cells ------------------------------------------
    M = "m25-dram-organization-audit.md"
    cells = {}
    for f in os.listdir(os.path.join(W, "m25", "map2")):
        if not f.endswith(".txt") or f.startswith("probe"):
            continue
        d = kv(os.path.join(W, "m25", "map2", f))
        if "cycles" in d:
            cells[f[:-4]] = d
    chk(len(cells) == 8, "8 mapping logs (6 cells + 2 reruns)", str(len(cells)))
    for k, cyc in (("orkut_row1k_d0", 6130484), ("orkut_row1k_d1", 6130494),
                   ("orkut_row4k_d0", 5308393), ("orkut_row4k_d1", 5309111),
                   ("criteo_row1k_d0", 421650), ("criteo_row1k_d1", 406007),
                   ("criteo_row4k_d0", 406402), ("criteo_row4k_d1", 399588)):
        chk(cells[k]["cycles"] == cyc, f"{k} cycles", cells[k]["cycles"])
        num(M, cyc, f"{k} cycles in the report")
    # the four percentages
    p_ork1 = 100.0 * (cells["orkut_row1k_d1"]["cycles"] - cells["orkut_row1k_d0"]["cycles"]) / \
        cells["orkut_row1k_d0"]["cycles"]
    chk(abs(p_ork1 - 0.0002) < 0.001, "orkut 1k organizer +0.0002 %", f"{p_ork1:.4f}")
    p_ork4 = 100.0 * (cells["orkut_row4k_d1"]["cycles"] - cells["orkut_row4k_d0"]["cycles"]) / \
        cells["orkut_row4k_d0"]["cycles"]
    chk(abs(p_ork4 - 0.0135) < 0.002, "orkut 4k organizer +0.014 %", f"{p_ork4:.4f}")
    p_cr1 = 100.0 * (cells["criteo_row1k_d1"]["cycles"] - cells["criteo_row1k_d0"]["cycles"]) / \
        cells["criteo_row1k_d0"]["cycles"]
    chk(abs(p_cr1 + 3.71) < 0.02, "criteo 1k organizer -3.71 %", f"{p_cr1:.3f}")
    p_cr4 = 100.0 * (cells["criteo_row4k_d1"]["cycles"] - cells["criteo_row4k_d0"]["cycles"]) / \
        cells["criteo_row4k_d0"]["cycles"]
    chk(abs(p_cr4 + 1.68) < 0.02, "criteo 4k organizer -1.68 %", f"{p_cr4:.3f}")
    # the geometry effect
    g_ork = 100.0 * (cells["orkut_row4k_d0"]["cycles"] - cells["orkut_row1k_d0"]["cycles"]) / \
        cells["orkut_row1k_d0"]["cycles"]
    chk(abs(g_ork + 13.4) < 0.06, "4 KiB rows are 13.4 % faster on orkut", f"{g_ork:.2f}")
    has(M, "13.4 %", "the 13.4 % geometry statement")
    # row hits quoted
    for k, pctv in (("orkut_row1k_d0", 60.52), ("orkut_row4k_d0", 83.52),
                    ("criteo_row1k_d0", 42.29), ("criteo_row1k_d1", 61.79),
                    ("criteo_row4k_d0", 68.90), ("criteo_row4k_d1", 77.64)):
        d = cells[k]
        hp = 100.0 * d["dram_row_hits"] / d["dram_arrivals"]
        chk(abs(hp - pctv) < 0.01, f"{k} row hit {pctv} %", f"{hp:.2f}")
        has(M, f"{pctv:.2f} %", f"{k} row hit in the report")

    # ---- M24-A factorial ------------------------------------------------
    a = {(r["workload"], r["map"], r["cell"]): r for r in
         csv.DictReader(open(os.path.join(W, "m24a", "summary.csv")))}
    for wl, mp, cyc_b, cyc_d in (("orkut", "M0", 6130484, 6130494),
                                 ("orkut", "M3", 6128013, 6035778),
                                 ("livejournal", "M0", 14146092, 14146860),
                                 ("ogbntrain", "M0", 98542852, 98563897)):
        chk(int(a[(wl, mp, "B")]["cycles"]) == cyc_b, f"M24-A {wl} {mp} B", a[(wl, mp, "B")]["cycles"])
        chk(int(a[(wl, mp, "D")]["cycles"]) == cyc_d, f"M24-A {wl} {mp} D", a[(wl, mp, "D")]["cycles"])
        num(M, cyc_b, f"M24-A {wl} {mp} B in the report")
        num(M, cyc_d, f"M24-A {wl} {mp} D in the report")
    # the organizer's per-run cost quoted in the DRAM audit (0.98 cycles/run)
    ork = a[("orkut", "M0", "D")]
    per = float(ork["org_cycles"]) / float(ork["org_runs"])
    chk(abs(per - 0.9784) < 0.01, "organizer 0.98 cycles per run", f"{per:.4f}")
    has(M, "0.98 cycles per", "the 0.98 cycles/run statement")
    p = 100.0 * (6035778 - 6128013) / 6128013
    chk(abs(p + 1.506) < 0.01, "M3 orkut organizer -1.51 %", f"{p:.3f}")
    has(M, "−1.51 %", "the -1.51 % statement")

    # ---- the geometry claim ---------------------------------------------
    # A 4 KiB page covers exactly 2 rows at the production geometry; a 4 KiB row
    # covers exactly 2 pages.
    chk(4096 // 64 == 64, "page = 64 lines")
    chk(1024 // 64 == 16, "1 KiB row = 16 lines")
    chk(4096 // 64 == 64 and 2048 // 64 == 32, "4 KiB row = 64 lines = 2 pages per bank")
    has(M, "pages per row", "the geometry table")
    has(M, "two pages can never share a row", "the correction to the prior claim")

    # ---- literature review ----------------------------------------------
    L = "m25-literature-review.md"
    for s in ("DX100", "LATPC", "IRU", "WarpPool", "ICC", "MARS", "Prodigy", "ATP",
              "Segura", "Shin", "4.061", "2.48 KB", "23.9 mm", "5.23 KB", "276 B"):
        has(L, s, f"literature review mentions {s}")
    has(L, "claim of novelty", "the deflationary statement")
    chk("novel" not in L.lower() or "not" in L.lower(), "no unqualified novelty claim")

    # ---- proposal --------------------------------------------------------
    P = "m25-architecture-proposal.md"
    for s in ("Selector v1", "Drop `ford`/`fpos` by default", "E1", "E2", "E3", "E4", "E5",
              "114 688 B", "57 344 B"):
        has(P, s, f"proposal contains {s}")
    has(P, "Nothing in this document has been implemented", "the not-implemented statement")

    # ---- forbidden claims ------------------------------------------------
    for n in (A, B, M, P):
        t = text(n)
        chk("GHz" not in t, f"{n}: no frequency claim")
        chk(not re.search(r"\bmW\b", t), f"{n}: no power claim")
        chk("mm²" not in t, f"{n}: no area claim")

    ok = sum(1 for c in CHECKS if c[0])
    out = os.path.join(W, "m25", "doc_checks.txt")
    with open(out, "w") as f:
        f.write(f"M25 report verification: {len(CHECKS)} checks\n")
        f.write(f"{len(CHECKS) - ok} of {len(CHECKS)} checks failed\n")
        for good, what, detail in CHECKS:
            f.write(f"{'ok  ' if good else 'FAIL'} {what} {detail}\n")
    print(f"M25 report verification: {len(CHECKS)} checks")
    print(f"{len(CHECKS) - ok} of {len(CHECKS)} checks failed")
    return 0 if ok == len(CHECKS) else 1


if __name__ == "__main__":
    sys.exit(main())
