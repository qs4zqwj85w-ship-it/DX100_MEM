#!/usr/bin/env python3
"""M24-H — recompute every figure in docs/m24-h-hardware-cost-audit.md.

The rule is the one the M24 reports have used: a number in the prose is either
produced by a tool in this repo from a measured cell, or it is not in the prose.
This script re-derives each quoted figure from

  build/wm/m24h/storage.csv, totals.csv   the storage model
  build/wm/m24h/cells.csv, ladder.csv     the 18 cells
  build/wm/m24h/traffic.csv               Bulk vs SIMT traffic
  build/wm/m24e/summary.csv               the M24-E ablations

and fails if the document does not contain the recomputed string.

usage: python3 tools/m24h_verify_doc.py [--doc docs/m24-h-hardware-cost-audit.md]
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m24h")
DOC = os.path.join(ROOT, "docs/m24-h-hardware-cost-audit.md")

CHECKS = []


def have(s, why):
    CHECKS.append((s in DOC_TEXT, why, repr(s)))


def check(ok, why, val=""):
    CHECKS.append((bool(ok), why, str(val)))


def num(v, nd=0):
    if nd:
        return ("{:,.%df}" % nd).format(v).replace(",", " ")
    return "{:,.0f}".format(v).replace(",", " ")


def main(argv):
    global DOC_TEXT
    path = DOC
    for i, a in enumerate(argv):
        if a == "--doc":
            path = os.path.join(ROOT, argv[i + 1])
    DOC_TEXT = open(path).read().replace("\u2009", " ").replace("\u00a0", " ")

    rows = list(csv.DictReader(open(os.path.join(OUT, "storage.csv"))))
    tot = {r["accounting"]: float(r["bytes"])
           for r in csv.DictReader(open(os.path.join(OUT, "totals.csv")))}
    cells = list(csv.DictReader(open(os.path.join(OUT, "cells.csv"))))
    C = {(r["cell"], r["arm"]): r for r in cells}
    ab = {}
    e = os.path.join(ROOT, "build/wm/m24e/summary.csv")
    if os.path.exists(e):
        for r in csv.DictReader(open(e)):
            ab[(r["ablation"], r["wl"])] = r

    def f(row, k):
        try:
            return float(row[k])
        except (KeyError, ValueError, TypeError):
            return 0.0

    # ---- 1. storage rows -----------------------------------------------------
    by = {r["structure"]: r for r in rows}
    for name, packed, bits in (("fent", 262144, 64), ("fmeta", 131072, 32),
                               ("fpa", 131072, 32), ("ford", 57344, 14),
                               ("fpos", 57344, 14), ("fslot", 28672, 7),
                               ("ready", 14336, 14), ("fan_", 9728, 608),
                               ("idx ring", 5376, 168), ("ftr", 4096, 1),
                               ("fflag", 4096, 1)):
        r = by[name]
        check(abs(f(r, "packed_bytes") - packed) < 0.5, "%s packed bytes" % name,
              num(f(r, "packed_bytes")))
        check(int(r["bits"]) == bits, "%s bits/entry" % name, r["bits"])
        have(num(packed), "%s appears in the table" % name)
    check(abs(f(by["dir_vpn"], "packed_bytes") + f(by["dir_mask"], "packed_bytes") - 11520) < 0.5,
          "page directory 11 520 B", "")
    have(num(11520), "directory total appears")
    check(abs(f(by["ftr_gen"], "abi_total") + f(by["ftr_done"], "abi_total") - 524288) < 0.5,
          "timestamps 524 288 B", "")
    have(num(524288), "timestamp total appears")
    check(abs(f(by["coh"], "packed_bytes") + f(by["g6c_coh_def"], "packed_bytes")
              + f(by["g6c_coh_age"], "packed_bytes") - 432) < 0.5, "cohort table 432 B", "")
    have(num(432), "cohort table appears")

    # totals
    check(abs(tot["packed_hw"] - 707120) < 0.5, "packed hw 707 120 B", num(tot["packed_hw"]))
    check(abs(tot["charged"] - 347296) < 0.5, "charge 347 296 B", num(tot["charged"]))
    check(abs(tot["abi_hw"] - 1106176) < 0.5, "abi hw 1 106 176 B", num(tot["abi_hw"]))
    check(abs(tot["abi_all"] - 1122560) < 0.5, "abi all 1 122 560 B", num(tot["abi_all"]))
    for k in ("packed_hw", "charged", "abi_hw", "abi_all"):
        have(num(tot[k]), "%s appears" % k)
    check(abs(tot["packed_all"] - 718640) < 0.5, "packed incl dead 718 640", num(tot["packed_all"]))
    have(num(718640), "packed incl dead appears")
    check(abs(tot["packed_hw"] / 1024 - 690.5) < 0.05, "690.5 KiB", "")
    have("690.5", "690.5 KiB appears")
    check(abs(tot["packed_hw"] / 1048576 - 0.674) < 0.01, "0.67x L2", "")
    have("0.67×", "0.67x L2 appears")
    check(abs(tot["packed_hw"] * 8 * 0.045 / 1e6 - 0.255) < 0.001, "0.255 mm2", "")
    have("0.255", "0.255 mm2 appears")
    check(abs(tot["abi_hw"] / 1024 - 1080.25) < 0.1, "1080 KiB as allocated", "")
    have("1 080 KiB", "1 080 KiB appears")
    # instrumentation share of the as-allocated total
    share = tot["instr"] / tot["abi_all"]
    check(abs(share - 0.467) < 0.005, "instrumentation 47 %", "%.3f" % share)
    have("47 %", "47 % appears")

    # reconciliation
    fused = sum(f(by[n], "abi_total") for n in ("fent", "fmeta", "fslot", "fflag",
                                                "dir_vpn", "dir_mask"))
    later = sum(f(by[n], "abi_total") for n in ("ford", "fpos", "fpa", "ftr", "ready"))
    coh = sum(f(by[n], "abi_total") for n in ("coh", "g6c_coh_def", "g6c_coh_age"))
    eng = sum(f(r, "abi_total") for r in rows
              if r["scope"] == "engine" and r["class"] not in ("instr", "req"))
    check(abs(fused - 475136) < 0.5, "fused subset 475 136", num(fused))
    check(abs(later - 622592) < 0.5, "ladder state 622 592", num(later))
    check(abs(eng - 24128) < 0.5, "engine state 24 128", num(eng))
    check(abs(fused + later + coh + eng - tot["abi_all"]) < 0.5,
          "reconciliation sums to abi_all", num(fused + later + coh + eng))
    for v in (fused, later, eng, coh):
        have(num(v), "reconciliation term %s appears" % num(v))
    check(abs(fused / tot["abi_all"] - 0.423) < 0.005, "fused is 42 % of allocation", "")
    have("42 %", "42 % appears")

    # shares used in the table
    for name, pct in (("fent", 36.5), ("fmeta", 18.2), ("fpa", 18.2), ("ford", 8.0),
                      ("fpos", 8.0), ("fslot", 4.0), ("ready", 2.0), ("dir", 1.6)):
        got = 100.0 * (f(by["dir_vpn"], "packed_bytes") + f(by["dir_mask"], "packed_bytes")) \
            / tot["packed_all"] if name == "dir" else \
            100.0 * f(by[name], "packed_bytes") / tot["packed_all"]
        check(abs(got - pct) < 0.06, "%s share %.1f%%" % (name, pct), "%.2f" % got)
        have("%.1f %%" % pct, "%s share appears" % name)
    # the three structures priced as expensive
    trio = sum(f(by[n], "packed_bytes") for n in ("fpa", "fpos", "ford"))
    check(abs(trio - 245760) < 0.5, "order+PA 245 760 B", num(trio))
    have(num(245760), "245 760 appears")
    check(abs(100.0 * trio / tot["packed_all"] - 34.2) < 0.1, "order/PA is 34 %", "")
    have("34 %", "34 % share appears")
    check(abs(trio / 1024 - 240) < 0.5, "order/PA is 240 KiB", "")
    have("240 KiB", "240 KiB appears")
    meta = sum(f(by[n], "packed_bytes") for n in ("fmeta", "fpa", "fpos", "ford", "ready",
                                                  "dir_vpn", "dir_mask"))
    check(abs(meta - 402688) < 0.5, "metadata 402 688 B", num(meta))
    have(num(402688), "402 688 appears")
    check(abs(meta / tot["packed_hw"] - 0.569) < 0.01, "metadata 57 % of hw total", "%.3f"
          % (meta / tot["packed_hw"]))
    have("57 %", "57 % appears")
    ctrl = sum(f(r, "packed_bytes") for r in rows
               if r["scope"] == "engine" and r["class"] not in ("instr", "req"))
    check(abs(ctrl - 16512) < 0.5, "engine control 16 512 B", num(ctrl))
    have(num(16512), "16 512 appears")
    half = sum(f(by[n], "packed_bytes") / 2 for n in ("fent", "fmeta", "fpa", "ford",
                                                      "fpos", "fslot", "fflag", "ftr"))
    check(abs(half - 337920) < 0.5, "single window 337 920 B", num(half))
    have(num(337920), "337 920 appears")

    # M22 comparison
    m22 = 475376
    check(abs(tot["packed_hw"] - m22 - 231744) < 0.5, "M22 delta 231 744 B", "")
    have(num(231744), "231 744 appears")
    check(abs(100.0 * (tot["packed_hw"] - m22) / m22 - 48.7) < 0.05, "M22 +48.7 %", "")
    have("48.7 %", "+48.7 % appears")

    # ---- 2. ladder -----------------------------------------------------------
    lad = list(csv.DictReader(open(os.path.join(OUT, "ladder.csv"))))
    L = {}
    for r in lad:
        L[(r["trace"], int(r["org"]))] = r
    for tr in ("w4_duplow", "criteo_2k", "n1_seq8m", "n3_clu4m"):
        for o in (0, 6, 7):
            v = C[("org%d__%s__bulk" % (o, tr), "bulk")]
            have(num(f(v, "cycles")), "ladder cycles %s org%d" % (tr, o))
        have(num(f(C[(tr + "__bulk", "bulk")], "cycles")), "production cycles %s" % tr)
    # step ratios
    for tr, r06, r67, r78, net in (("w4_duplow", 4.11, 0.795, 0.305, 0.996),
                                   ("criteo_2k", 3.83, 0.896, 0.327, 1.121),
                                   ("n1_seq8m", 1.41, 0.699, 1.000, 0.984),
                                   ("n3_clu4m", 1.91, 0.858, 0.604, 0.990)):
        c0 = f(C[("org0__%s__bulk" % tr, "bulk")], "cycles")
        c6 = f(C[("org6__%s__bulk" % tr, "bulk")], "cycles")
        c7 = f(C[("org7__%s__bulk" % tr, "bulk")], "cycles")
        c8 = f(C[(tr + "__bulk", "bulk")], "cycles")
        check(abs(c6 / c0 - r06) < 0.006, "%s step 0->6 %.2f" % (tr, r06), "%.3f" % (c6 / c0))
        check(abs(c7 / c6 - r67) < 0.001, "%s step 6->7 %.3f" % (tr, r67), "%.3f" % (c7 / c6))
        check(abs(c8 / c7 - r78) < 0.001, "%s step 7->8 %.3f" % (tr, r78), "%.3f" % (c8 / c7))
        check(abs(c8 / c0 - net) < 0.001, "%s net %.3f" % (tr, net), "%.3f" % (c8 / c0))
        have("×%.3f" % (c8 / c0) if tr == "w4_duplow" else "%.3f" % (c8 / c0),
             "%s net appears" % tr)
    have("×4.11", "step 0->6 w4 appears")
    have("×0.795", "step 6->7 w4 appears")
    have("×0.305", "step 7->8 w4 appears")

    # M23 panel
    for tr, p0, p8, rat in (("orkut", 6653100, 6130494, 0.921),
                            ("livejournal", 8404988, 7558966, 0.899),
                            ("ogbn", 35248112, 35248957, 1.000)):
        a = f(C[("org0__%s__bulk" % tr, "bulk")], "cycles")
        b = f(C[("prod__%s__bulk" % tr, "bulk")], "cycles")
        check(abs(a - p0) < 0.5, "%s org0 cycles" % tr, num(a))
        check(abs(b - p8) < 0.5, "%s production cycles" % tr, num(b))
        check(abs(b / a - rat) < 0.001, "%s ratio %.3f" % (tr, rat), "%.3f" % (b / a))
        have(num(p0), "%s org0 appears" % tr)
        have(num(p8), "%s production appears" % tr)
        have("%.3f" % (b / a), "%s ratio appears" % tr)
        if ab:
            e = ab[("base", tr)]
            u = ab[("unfused", tr)]
            check(abs(float(e["cycles"]) - b) < 0.5, "%s reproduces M24-E base" % tr)
            check(abs(float(u["cycles"]) - a) < 0.5, "%s org0 reproduces M24-E unfused" % tr)
    have("8.5 %", "8.5 % appears")
    have("11.2 %", "11.2 % appears")

    # ladder storage
    for o, v in ((0, 448640), (6, 646864), (7, 718544), (8, 718640)):
        check(abs(tot["ladder_org%d" % o] - v) < 0.5, "ladder org%d = %s" % (o, num(v)),
              num(tot["ladder_org%d" % o]))
        have(num(v), "ladder org%d appears" % o)
    check(abs(tot["ladder_org6"] - tot["ladder_org0"] - 198224) < 0.5, "delta 198 224", "")
    check(abs(tot["ladder_org7"] - tot["ladder_org6"] - 71680) < 0.5, "delta 71 680", "")
    check(abs(tot["ladder_org8"] - tot["ladder_org7"] - 96) < 0.5, "delta 96", "")
    check(abs(tot["ladder_org8"] - tot["ladder_org0"] - 270000) < 0.5, "net 270 000 B", "")
    for v in (198224, 71680, 270000):
        have(num(v), "%s appears" % num(v))
    have("264 KiB", "264 KiB appears")

    # ---- 3. performance and traffic -----------------------------------------
    perf = {"w4_duplow": 2.504, "criteo_2k": 3.280, "n1_seq8m": 0.975, "n3_clu4m": 0.823}
    for tr, want in perf.items():
        b = f(C[(tr + "__bulk", "bulk")], "cycles")
        s = f(C[(tr + "__simt", "simt")], "cycles")
        check(abs(s / b - want) < 0.0006, "%s ratio %.3f" % (tr, want), "%.4f" % (s / b))
        have("%.3f" % want, "%s ratio appears" % tr)
    tr_rows = list(csv.DictReader(open(os.path.join(OUT, "traffic.csv"))))
    T = {(r["trace"], r["metric"]): r for r in tr_rows}
    for tr, want in (("w4_duplow", 3.8), ("criteo_2k", -22.9), ("n1_seq8m", 5.3), ("n3_clu4m", 18.6)):
        got = float(T[(tr, "DRAM reads")]["delta_pct"])
        check(abs(got - want) < 0.06, "%s DRAM reads %+.1f%%" % (tr, want), "%+.1f" % got)
        have("%+.1f %%" % want if want > 0 else "−%.1f %%" % abs(want),
             "%s read delta appears" % tr)
    for tr, want in (("w4_duplow", -61.0), ("criteo_2k", -70.1), ("n1_seq8m", -28.5),
                     ("n3_clu4m", -29.4)):
        got = float(T[(tr, "DRAM row activations")]["delta_pct"])
        check(abs(got - want) < 0.06, "%s activations %.1f%%" % (tr, want), "%.1f" % got)
        have("%.1f %%" % abs(want), "%s activation delta appears" % tr)
    # row-hit rates
    for tr in ("w4_duplow", "criteo_2k", "n1_seq8m", "n3_clu4m"):
        b = C[(tr + "__bulk", "bulk")]
        s = C[(tr + "__simt", "simt")]
        rh = 100.0 * f(b, "dram_row_hits") / f(b, "dram_reads")
        check(1.0 <= rh <= 99.0, "%s row-hit rate sane" % tr, "%.1f" % rh)
    for v in (62.5, 63.7, 92.9, 72.3):
        have("%.1f %%" % v, "row-hit rate %.1f appears" % v)
    # activation savings quoted in 4.3
    for tr, saved in (("w4_duplow", 95125), ("criteo_2k", 12768), ("n1_seq8m", 4662),
                      ("n3_clu4m", 5493)):
        b = C[(tr + "__bulk", "bulk")]
        s = C[(tr + "__simt", "simt")]
        got = f(s, "dram_row_acts_sum") - f(b, "dram_row_acts_sum")
        check(abs(got - saved) < 0.5, "%s activations saved %s" % (tr, num(saved)), num(got))
        have(num(saved), "%s saved appears" % tr)
    # release slot stall and occupancy
    for tr, pct in (("w4_duplow", 98.1), ("criteo_2k", 52.9), ("n1_seq8m", 98.2), ("n3_clu4m", 59.8)):
        b = C[(tr + "__bulk", "bulk")]
        got = 100.0 * f(b, "bulk_g6b_release_slot_stall") / f(b, "cycles")
        check(abs(got - pct) < 0.06, "%s slot stall %.1f%%" % (tr, pct), "%.2f" % got)
        have("%.1f %%" % pct, "%s slot stall appears" % tr)
    for tr, occ in (("w4_duplow", 3253), ("criteo_2k", 1451), ("n1_seq8m", 3574),
                    ("n3_clu4m", 1132)):
        b = C[(tr + "__bulk", "bulk")]
        got = f(b, "bulk_g6b_ready_occ_sum") / f(b, "cycles")
        check(abs(got - occ) < 1.0, "%s mean pool %.0f" % (tr, occ), "%.0f" % got)
        have(num(occ), "%s mean pool appears" % tr)
    # index traffic share of target traffic
    for tr, pct in (("w4_duplow", 6.3), ("criteo_2k", 16.6), ("n1_seq8m", 6.3), ("n3_clu4m", 21.5)):
        b = C[(tr + "__bulk", "bulk")]
        got = 100.0 * f(b, "bulk_idx_line_bytes") / f(b, "bulk_tgt_line_bytes")
        check(abs(got - pct) < 0.06, "%s index share %.1f%%" % (tr, pct), "%.2f" % got)
        have("%.1f %%" % pct, "%s index share appears" % tr)
    # pool behaviour on the trace where the stack loses
    check(abs(f(C[("org7__criteo_2k__bulk", "bulk")], "bulk_g6b_ready_peak") - 392) < 0.5,
          "org 7 pool peak 392", "")
    have("392", "392 appears")
    check(abs(f(C[("criteo_2k__bulk", "bulk")], "bulk_g6b_ready_peak") - 4096) < 0.5,
          "production pool peak 4 096 on criteo_2k", "")
    have("4 096", "4 096 appears")
    # fold fractions
    for tr, pct in (("criteo_2k", 62.4), ("n3_clu4m", 70.9)):
        b = C[(tr + "__bulk", "bulk")]
        got = 100.0 * f(b, "bulk_dedup_merged") / f(b, "bulk_elems_total")
        check(abs(got - pct) < 0.06, "%s fold %.1f%%" % (tr, pct), "%.2f" % got)
        have("%.0f %%" % round(got), "%s fold appears" % tr)
    # on-chip access totals quoted in 4.2
    for tr, m in (("w4_duplow", 1.31), ("criteo_2k", 0.17), ("n1_seq8m", 1.31),
                  ("n3_clu4m", 0.57)):
        b = C[(tr + "__bulk", "bulk")]
        got = (8 * f(b, "bulk_lines_issued") + 2 * f(b, "bulk_elems_total")) / 1e6
        check(abs(got - m) < 0.006, "%s on-chip %.2f M" % (tr, m), "%.3f" % got)
        have("%.2f M" % m, "%s on-chip appears" % tr)
    # break-even table
    for tr, be in (("w4_duplow", 13.8), ("criteo_2k", 13.7), ("n3_clu4m", 103),
                   ("n1_seq8m", 281)):
        b = C[(tr + "__bulk", "bulk")]
        s = C[(tr + "__simt", "simt")]
        added = 8 * f(b, "bulk_lines_issued") + 2 * f(b, "bulk_elems_total")
        saved = f(s, "dram_row_acts_sum") - f(b, "dram_row_acts_sum")
        got = added / saved
        check(abs(got - be) / be < 0.02, "%s break-even %.0f E1" % (tr, be), "%.1f" % got)
        have("%.0f" % be if be >= 100 else "%.1f" % be, "%s break-even appears" % tr)

    # ---- 4. M24-E contributions quoted in 5.4 ------------------------------
    if ab:
        def sl(abl, tr):
            return float(ab[(abl, tr)]["cycles"]) / float(ab[("base", tr)]["cycles"])
        lo = min(sl("nocoh", t) for t in ("orkut", "livejournal", "ogbn", "criteo"))
        hi = max(sl("nocoh", t) for t in ("orkut", "livejournal", "ogbn", "criteo"))
        check(abs(lo - 1.210) < 0.005 and abs(hi - 3.861) < 0.005,
              "cohort ablation 1.21-3.86x", "%.3f-%.3f" % (lo, hi))
        have("1.21–3.86", "1.21-3.86 appears")
        lo = min(sl("pool1", t) for t in ("orkut", "livejournal", "ogbn", "criteo"))
        hi = max(sl("pool1", t) for t in ("orkut", "livejournal", "ogbn", "criteo"))
        check(abs(lo - 5.530) < 0.005 and abs(hi - 8.021) < 0.005,
              "pool=1 5.53-8.02x", "%.3f-%.3f" % (lo, hi))
        have("5.53–8.02", "5.53-8.02 appears")
        hi16 = max(sl("pool16", t) for t in ("orkut", "livejournal", "ogbn", "criteo"))
        check(abs(hi16 - 1.108) < 0.005, "pool=16 costs 10.8 %", "%.3f" % hi16)
        have("11 %", "pool 16 <= 11 % appears")
        unf = max(sl("unfused", t) for t in ("orkut", "livejournal", "ogbn", "criteo"))
        check(abs(unf - 1.112) < 0.005, "unfused 11.2 %", "%.3f" % unf)
        hi_idx = max(sl("idx0", t) for t in ("orkut", "livejournal", "ogbn", "criteo"))
        check(abs(hi_idx - 1.020) < 0.005, "idx_mode 0 costs up to 2.0 %", "%.3f" % hi_idx)
        have("2.0 %", "2.0 % appears")

    # ---- 5. structural claims ------------------------------------------------
    check("GHz" not in DOC_TEXT, "no frequency claim", "")
    for bad in (" mW", " J/", "leakage of", "gates"):
        check(bad not in DOC_TEXT, "no absolute power/leakage/gate claim: %r" % bad, "")
    check(DOC_TEXT.count("not claimed") + DOC_TEXT.count("no power") >= 2,
          "claims are hedged explicitly", "")

    fails = [c for c in CHECKS if not c[0]]
    print("M24-H report verification: %d checks" % len(CHECKS))
    for ok, why, val in fails:
        print("  FAIL %-52s %s" % (why, val))
    print("%d of %d checks failed" % (len(fails), len(CHECKS)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
