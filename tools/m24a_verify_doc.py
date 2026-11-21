#!/usr/bin/env python3
"""M24-A — verify every figure quoted in the report against the logs.

  python3 tools/m24a_verify_doc.py

Recomputes each value from build/wm/m24a/*.txt and asserts the exact token the
document prints is present in docs/m24-a-four-way-organization-evaluation.md.
"""
import os
import sys

DOC = "docs/m24-a-four-way-organization-evaluation.md"
DIR = "build/wm/m24a"
WLS = ["orkut", "livejournal", "ogbntrain"]
WALKERS = {"orkut": 8, "livejournal": 4, "ogbntrain": 4}
MAPS = [0, 1, 2, 3]


def sp(v, dec=0):
    s = ("%.*f" % (dec, v)) if dec else ("%d" % round(v))
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    ip, _, fp = s.partition(".")
    out = ""
    while len(ip) > 3:
        out = " " + ip[-3:] + out
        ip = ip[:-3]
    return ("-" if neg else "") + ip + out + ("." + fp if fp else "")


def parse(p):
    kv = {}
    for line in open(p, errors="replace"):
        k, _, v = line.strip().partition("=")
        if v:
            try:
                kv[k] = int(v)
            except ValueError:
                pass
    return kv


def main():
    doc = open(DOC, encoding="utf-8").read().replace("\u2212", "-").replace("\u00d7", "x")
    d = {}
    for wl in WLS:
        for m in MAPS:
            for c in "ABCD":
                p = os.path.join(DIR, "m24_%s_M%d_%s.txt" % (c, m, wl))
                if os.path.exists(p) and "cycles" in parse(p):
                    d[(wl, m, c)] = parse(p)
    simt = {}
    for wl in WLS:
        for m in MAPS:
            p = os.path.join(DIR, "simt_M%d_%s.txt" % (m, wl))
            if os.path.exists(p) and "cycles" in parse(p):
                simt[(wl, m)] = parse(p)["cycles"]

    checks = []

    def want(tok, label):
        checks.append((tok, label))

    # ---- cycles, all four cells x available maps -------------------------
    for (wl, m, c), v in d.items():
        want(sp(v["cycles"]), "%s M%d %s cycles" % (wl, m, c))
    # ---- per-cell derived figures ----------------------------------------
    for (wl, m, c), v in d.items():
        cy = v["cycles"]
        reads = v["dram_reads"]
        acts = v["dram_row_acts_sum"]
        s = max(1, v.get("bulk_g6i_samples", 1))
        pool = v["bulk_g6b_ready_occ_sum"] / cy
        tag = "%s M%d %s" % (wl, m, c)
        if c == "D":
            b = d[(wl, m, "B")]["cycles"]
            want("%+.4f %%" % (100.0 * (cy - b) / b), tag + " D vs B %")
        if m == 0:
            want(sp(reads), tag + " reads")
            want("%.2f %%" % (100.0 * v["dram_row_hits"] / reads), tag + " row hit")
            want(sp(acts), tag + " activations")
            want("%.2f" % (reads / acts), tag + " reads/act")
            want(sp(pool, 1), tag + " pool")
            want("%.1f" % (v.get("bulk_g6i_vpn_sum", 0) / s), tag + " pages")
            want("%.1f" % (v.get("bulk_g6i_cells_sum", 0) / s), tag + " cells")
            want("%.2f" % (pool / max(1e-9, v.get("bulk_g6i_cells_sum", 1) / s)), tag + " e/cl")
            want("%.2f %%" % (100.0 * v.get("bulk_m24_rel_inversions", 0) /
                              max(1, v.get("bulk_m24_rel_pairs", 1))), tag + " inversions")
            want("%.1f %%" % (100.0 * v.get("walker_busy", 0) / (cy * WALKERS[wl])),
                 tag + " walker util")
    # ---- page-factor table (Orkut M0) ------------------------------------
    a, b, dd = d[("orkut", 0, "A")], d[("orkut", 0, "B")], d[("orkut", 0, "D")]
    want("%.1f %%" % (100.0 * a["bulk_g6b_tr_stall_cycles"] / a["cycles"]), "A tr-stall share")
    want(sp(a["bulk_g6b_tr_stall_cycles"]), "A tr-stall cycles")
    for lab, v in (("A", a), ("B", b), ("D", dd)):
        want("%.2f" % (v.get("tlb0_mshr_occ", 0) / v["cycles"]), lab + " dTLB MSHR occupancy")
    want(sp(a.get("tlb0_mshr_stalls", 0)), "A MSHR stalls")
    want(sp(b.get("tlb0_mshr_stalls", 0)), "B MSHR stalls")
    want(sp(dd.get("tlb0_mshr_stalls", 0)), "D MSHR stalls")
    want("%.1f %%" % (100.0 * (a["dram_bank0_busy"] + a["dram_bank1_busy"]) / (2 * a["cycles"])),
         "A bank busy")
    want("%.1f %%" % (100.0 * (dd["dram_bank0_busy"] + dd["dram_bank1_busy"]) / (2 * dd["cycles"])),
         "D bank busy")
    want("%.1f" % (a["dram_q_occ_sum"] / a["cycles"]), "A mean queue")
    want("%.1f" % (dd["dram_q_occ_sum"] / dd["cycles"]), "D mean queue")
    want(sp(dd["dram_wait_sum"] / dd["dram_svc_events"]), "D mean wait")
    want("%.3f" % (a["bulk_g6b_tr_completed"] / a["cycles"]), "A tr/cyc")
    want("%.3f" % (b["bulk_g6b_tr_completed"] / b["cycles"]), "B tr/cyc")
    for lab, v in (("A", a), ("B", b), ("D", dd)):
        want(sp(v["dram_reads"]), lab + " orkut M0 reads")
        want(sp(v["walks"]), lab + " walks")
    # ---- logistic figures -------------------------------------------------
    want(sp(a["dram_rows_used"]), "A rows used")
    want(sp(dd["dram_rows_used"]), "D rows used")
    want(sp(d[("orkut", 3, "D")]["dram_rows_used"]), "D M3 rows used")
    # ---- DRAM-factor table (M0) ------------------------------------------
    for wl in WLS:
        bb, ddd = d[(wl, 0, "B")], d[(wl, 0, "D")]
        want(sp(ddd["cycles"] - bb["cycles"]), "%s M0 D-B cycles" % wl)
        want("%+.4f %%" % (100.0 * (ddd["cycles"] - bb["cycles"]) / bb["cycles"]),
             "%s M0 D-B %%" % wl)
    # ---- M3 Orkut detail --------------------------------------------------
    b3, d3 = d[("orkut", 3, "B")], d[("orkut", 3, "D")]
    for lab, v in (("B3", b3), ("D3", d3)):
        want(sp(v["dram_row_acts_sum"]), lab + " activations")
        want("%.2f %%" % (100.0 * v["dram_row_hits"] / v["dram_reads"]), lab + " row hit")
        s = max(1, v["bulk_g6i_samples"])
        want("%.1f" % (v["bulk_g6i_cells_sum"] / s), lab + " cells")
        want("%.2f" % (v["bulk_g6b_ready_occ_sum"] / v["cycles"] /
                       (v["bulk_g6i_cells_sum"] / s)), lab + " e/cl")
        want("%.2f" % (v["bulk_g6i_xpage_cells_sum"] / s), lab + " xpage mean")
        want(sp(v["bulk_g6i_xpage_cells_max"]), lab + " xpage max")
        want("%.2f %%" % (100.0 * v["bulk_m24_rel_inversions"] / v["bulk_m24_rel_pairs"]),
             lab + " inversions")
    want("%.1f %%" % (100.0 * (b3["dram_row_acts_sum"] - d3["dram_row_acts_sum"]) /
                      b3["dram_row_acts_sum"]), "M3 activation reduction")
    want("%.2f pt" % (100.0 * d3["dram_row_hits"] / d3["dram_reads"] -
                      100.0 * b3["dram_row_hits"] / b3["dram_reads"]), "M3 row-hit gain")
    # ---- factorial -------------------------------------------------------
    for wl in WLS:
        for m in MAPS:
            if (wl, m, "A") not in d:
                continue
            A = d[(wl, m, "A")]["cycles"]
            B = d[(wl, m, "B")]["cycles"]
            C = d[(wl, m, "C")]["cycles"]
            D = d[(wl, m, "D")]["cycles"]
            want("%.2f %%" % (100.0 * (A - B) / A), "%s M%d page%%" % (wl, m))
            want("%.2f %%" % (100.0 * (A - D) / A), "%s M%d combined%%" % (wl, m))
            want("%.2f %%" % (100.0 * (B - D) / B), "%s M%d incDram%%" % (wl, m))
            want(sp(D - B - C + A), "%s M%d interaction" % (wl, m))
    # ---- SIMT -------------------------------------------------------------
    for wl in ("orkut", "livejournal"):
        for m in MAPS:
            want(sp(simt[(wl, m)]), "%s simt M%d" % (wl, m))
        want("%.2f" % (simt[(wl, 0)] / d[(wl, 0, "D")]["cycles"]), "%s D vs SIMT" % wl)
        want("%.2f" % (simt[(wl, 0)] / d[(wl, 0, "A")]["cycles"]), "%s A vs SIMT" % wl)
    # ---- overhead --------------------------------------------------------
    want(sp(dd["bulk_org_cycles"]), "D org cycles")
    want(sp(b["bulk_org_cycles"]), "B org cycles")
    want(sp(dd["bulk_org_cycles"] - b["bulk_org_cycles"]), "org cycles delta")
    want("%.1f %%" % (100.0 * (dd["bulk_org_cycles"] - b["bulk_org_cycles"]) / dd["cycles"]),
         "org delta share")
    want(sp(dd["bulk_g6b_pa_bytes"]), "pool bytes")
    want(sp(d[("orkut", 3, "B")]["bulk_g6b_ready_occ_sum"] / d[("orkut", 3, "B")]["cycles"], 1), "M3 B pool")

    bad = []
    for tok, label in checks:
        if tok not in doc:
            bad.append((label, tok))
    print("%d figures checked, %d missing" % (len(checks), len(bad)))
    for label, tok in bad:
        print("   %-34s computed=%r not found" % (label, tok))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
