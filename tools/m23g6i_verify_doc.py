#!/usr/bin/env python3
"""M23-G6I — verify every figure quoted in the report against the logs.

  python3 tools/m23g6i_verify_doc.py

Recomputes each value from build/wm/m23g6i/g6i_<cell>_<workload>.txt and asserts the
exact token the document prints is present in
docs/m23-g6i-continuous-translation-dram-pipeline.md.
"""
import os
import sys

DOC = "docs/m23-g6i-continuous-translation-dram-pipeline.md"
CELLS = [("A1", "orkut"), ("B", "orkut"), ("A1", "livejournal"), ("B", "livejournal"),
         ("A1", "ogbntrain"), ("B", "ogbntrain")]


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
    doc = open(DOC, encoding="utf-8").read().replace("\u2212", "-")
    kv = {}
    for cell, wl in CELLS:
        p = os.path.join("build/wm/m23g6i", "g6i_%s_%s.txt" % (cell, wl))
        if not os.path.exists(p):
            print("missing %s" % p, file=sys.stderr)
            return 1
        kv[(cell, wl)] = parse(p)

    checks = []

    def want(token, label):
        checks.append((token, label))

    for cell, wl in CELLS:
        k = kv[(cell, wl)]
        c = k["cycles"]
        tag = "%s/%s" % (cell, wl)
        samples = k["bulk_g6i_samples"]
        pool = k["bulk_g6b_ready_occ_sum"] / c
        vpns = k["bulk_g6i_vpn_sum"] / samples
        cells = k["bulk_g6i_cells_sum"] / samples
        gapn = k["bulk_g6i_gap_n"]
        sw = k["bulk_g6i_rel_vpn_switch"]
        same = k["bulk_g6i_rel_same_vpn"]
        want(sp(c), tag + " cycles")
        if cell == "B":
            base = kv[("A1", wl)]["cycles"]
            want("%+.3f %%" % (100.0 * (c - base) / base), tag + " dCyc")
        want(sp(k["dram_reads"]), tag + " reads")
        want("%.4f %%" % (100.0 * k["dram_row_hits"] / k["dram_reads"]), tag + " hit")
        want(sp(k["dram_row_acts_sum"]), tag + " acts")
        want("%.4f" % (k["dram_reads"] / k["dram_row_acts_sum"]), tag + " reads/act")
        want(sp(k["dram_row_conflicts"]), tag + " conflicts")
        want(sp(pool, 1), tag + " pool mean")
        want(sp(k["bulk_g6b_ready_peak"]), tag + " pool peak")
        want("%.2f %%" % (100.0 * k["bulk_g6b_full_cycles"] / c), tag + " pool full%")
        want("%.2f %%" % (100.0 * k["bulk_g6i_pool_empty"] / c), tag + " pool empty%")
        want(sp(vpns, 1), tag + " pages mean")
        want(sp(k["bulk_g6i_vpn_max"]), tag + " pages max")
        want(sp(cells, 1), tag + " cells mean")
        want(sp(k["bulk_g6i_cells_max"]), tag + " cells max")
        want("%.2f" % (pool / cells), tag + " runs/cell")
        want(sp(k["bulk_g6i_untr_sum"] / samples), tag + " untranslated mean")
        want("%.2f / %d" % (k["bulk_g6i_pages_open_sum"] / samples,
                            k["bulk_g6i_pages_open_max"]), tag + " descriptors")
        want("%.2f %%" % (100.0 * sw / (sw + same)), tag + " page switches")
        want("%.1f %%" % (100.0 * k["bulk_g6i_rel_with_tr"] /
                          k["bulk_g6i_rel_cycles"]), tag + " releases while translating")
        want(sp(k["bulk_g6i_gap_sum"] / gapn), tag + " revisit gap")
        want(sp(k["bulk_g6i_gap_max"]), tag + " revisit gap max")
        want(sp(k["bulk_g6b_tr_wait_dram"] / k["bulk_g6b_runs_released"]), tag + " sojourn")
        if k["bulk_g6i_xpage_cells_sum"]:
            want("%.2f / %d" % (k["bulk_g6i_xpage_cells_sum"] / samples,
                                k["bulk_g6i_xpage_cells_max"]), tag + " xpage cells")

    # Cross-page / translation prose figures.
    k = kv[("A1", "orkut")]
    c = k["cycles"]
    want(sp(k["bulk_g6b_tr_completed"] / c, 3), "orkut tr/cyc")
    want("%.1f %%" % (100.0 * k["bulk_g6c_admit_idle_cycles"] / c), "admission idle")
    want("%.1f" % (k["bulk_g6c_admit_attempts"] / k["bulk_g6c_admit_accepted"]),
         "attempts per admission")
    want(sp(k["bulk_g6c_admit_attempts"]), "attempts")
    want(sp(k["bulk_g6c_refused"]), "refusals")
    want(sp(k["bulk_g6c_refuse_parked"]), "refusals parked")
    want(sp(k["bulk_g6c_refuse_full"]), "refusals full")
    want(sp(k["bulk_g6c_defer_then_other_admit"]), "deferral then other admit")
    want(sp(k["bulk_g6c_cohorts_opened"]), "cohorts opened")
    want(sp(k["bulk_g6b_release_pops"]), "releases")
    want("%.2f" % (k["dram_row_acts_sum"] / k["bulk_g6b_release_pops"]),
         "activations per release")
    want(sp(k["bulk_g6b_ready_h12"]), "orkut hist 2048+")
    want("%.1f %%" % (100.0 * k["bulk_g6b_ready_h12"] / c), "orkut hist 2048+ pct")
    # occupancy integral == summed sojourn (the check script asserts it; quote the value)
    want(sp(k["bulk_g6b_ready_occ_sum"]), "occupancy integral")

    bad = []
    for token, label in checks:
        if token not in doc:
            bad.append((label, token))
    print("%d figures checked, %d missing" % (len(checks), len(bad)))
    for label, token in bad:
        print("   %-32s computed=%r not found" % (label, token))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
