#!/usr/bin/env python3
"""M23-G6H — verify every number quoted in the report against the logs.

  python3 tools/m23g6h_verify_doc.py

Each check recomputes a value from build/wm/m23g6h/g6h_<arm>_<wl>.txt and asserts
that the exact token the document prints is present in
docs/m23-g6h-full-window-row-organization.md. Exits non-zero on any mismatch.
"""
import os
import sys

DOC = "docs/m23-g6h-full-window-row-organization.md"
ARMS = {"G0": "g6h_G0r_%s.txt", "G1": "g6h_G1r_%s.txt", "P2F": "g6h_P2F_%s.txt"}
WLS = ["orkut", "livejournal", "ogbntrain"]


def sp(v, dec=0):
    """Group the integer part with thin spaces, exactly like the report."""
    if dec:
        s = "%.*f" % (dec, v)
    else:
        s = "%d" % round(v)
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    ip, _, fp = s.partition(".")
    out = ""
    while len(ip) > 3:
        out = " " + ip[-3:] + out
        ip = ip[:-3]
    out = ip + out
    return ("-" if neg else "") + out + ("." + fp if fp else "")


def parse(path):
    kv = {}
    for line in open(path, errors="replace"):
        k, _, v = line.strip().partition("=")
        if v:
            try:
                kv[k] = int(v)
            except ValueError:
                pass
    return kv


def main():
    doc = open(DOC, encoding="utf-8").read().replace("\u2212", "-")
    cells = {}
    for arm, pat in ARMS.items():
        for wl in WLS:
            p = os.path.join("build/wm/m23g6h", pat % wl)
            if not os.path.exists(p):
                print("missing %s" % p, file=sys.stderr)
                return 1
            cells[(arm, wl)] = parse(p)

    checks = []

    def want(token, label):
        checks.append((token, label))

    for wl in WLS:
        base = cells[("G0", wl)]
        for arm in ("G0", "G1", "P2F"):
            kv = cells[(arm, wl)]
            tag = "%s/%s" % (arm, wl)
            c = kv["cycles"]
            want(sp(c), tag + " cycles")
            want("%+.3f %%" % (100.0 * (c - base["cycles"]) / base["cycles"]), tag + " dCyc")
            want(sp(kv["dram_reads"]), tag + " reads")
            want("%.3f %%" % (100.0 * kv["dram_row_hits"] / kv["dram_reads"]), tag + " hit%")
            want(sp(kv["dram_row_acts_sum"]), tag + " acts")
            want("%.4f" % (kv["dram_reads"] / kv["dram_row_acts_sum"]), tag + " reads/act")
            want(sp(kv["dram_row_hits"]), tag + " row hits")
            want(sp(kv["dram_conflicts"] if "dram_conflicts" in kv else kv["dram_row_conflicts"]),
                 tag + " conflicts")
            want("%.2f / %.2f" % (kv["dram_svc_hit_cycles"] / kv["dram_svc_events"],
                                  kv["dram_svc_miss_cycles"] / kv["dram_svc_events"]),
                 tag + " service hit/miss")
            want("%.2f %%" % (100.0 * kv["dram_busy_cycles"] / (2 * c)), tag + " util")
            want("%.3f" % (kv["dram_row_acts_sum"] / kv["bulk_sched_opens"]), tag + " acts/cell run")
            want(sp(kv["bulk_sched_opens"]), tag + " sched_opens")
            want("%.3f" % (kv["bulk_g6b_release_pops"] / kv["bulk_sched_opens"]), tag + " rel/cell run")
            want("%.2f %%" % (100.0 * kv["cc3_l2_hits"] / kv["cc3_l2_reads"]), tag + " L2 hit")
            want(sp(kv["cc3_l2_reads"]), tag + " L2 reads")
            want(sp(kv["cc3_l2_misses"]), tag + " L2 misses")
            want(sp(kv["walks"]), tag + " walks")
            if arm != "G0":
                d = kv["bulk_g6h_decisions"]
                want(sp(d), tag + " decisions")
                want(sp(kv["bulk_g6h_exam_sum"] / d, 1), tag + " exam mean")
                want(sp(kv["bulk_g6h_exam_max"]), tag + " exam max")
                want(sp(kv["bulk_g6h_untr_sum"] / d, 1), tag + " untr mean")
                want(sp(kv["bulk_g6h_untr_max"]), tag + " untr max")
                want(sp(kv["bulk_g6h_groups_sum"] / d, 1), tag + " cells mean")
                want(sp(kv["bulk_g6h_groups_max"]), tag + " cells max")
                want("%.2f" % (kv["bulk_g6h_elig_sum"] / kv["bulk_g6h_groups_sum"]), tag + " req/cell")
                want("%.2f %%" % (100.0 * kv["bulk_g6h_deferred_sum"] / kv["bulk_g6h_elig_sum"]),
                     tag + " deferred")
                op = kv["bulk_g6g_groups_opened"]
                want(sp(op), tag + " groups opened")
                want("%.2f" % (kv["bulk_g6g_group_size_sum"] / op), tag + " group size mean")
                want("%d" % kv["bulk_g6g_group_size_max"], tag + " group size max")
                want("%.2f" % (kv["bulk_g6g_group_drain_sum"] / kv["bulk_g6g_groups_closed"]),
                     tag + " drain mean")
                want("%.2f %%" % (100.0 * kv["bulk_g6g_same_cell"] /
                                  (kv["bulk_g6g_same_cell"] + kv["bulk_g6g_switch_cell"])),
                     tag + " same cell")
                want("%.2f %%" % (100.0 * kv["bulk_g6g_revisit"] / op), tag + " revisit")
            want(sp(kv["bulk_g6b_tr_wait_max"]), tag + " wait max")

    # Figures quoted in the prose.
    want(sp(cells[("P2F", "orkut")]["bulk_g6h_elig_sum"]), "orkut elig sum")
    want(sp(cells[("P2F", "livejournal")]["bulk_g6h_elig_sum"]), "livejournal elig sum")
    want(sp(cells[("P2F", "ogbntrain")]["bulk_g6h_elig_sum"]), "ogbn elig sum")
    want(sp(cells[("P2F", "orkut")]["bulk_g6g_scan_calls"]), "orkut scan calls")
    want(sp(cells[("P2F", "orkut")]["bulk_g6g_scan_calls"] -
            cells[("P2F", "orkut")]["bulk_g6h_decisions"]), "orkut drain checks")
    want(sp(cells[("G0", "orkut")]["bulk_g6b_release_pops"]), "orkut releases")
    for wl in WLS:
        want(sp(cells[("G0", wl)]["bulk_batches"]), "%s batches" % wl)
        # groups closed = opened - batches
        kv = cells[("P2F", wl)]
        assert kv["bulk_g6g_groups_opened"] - kv["bulk_g6g_groups_closed"] == kv["bulk_batches"], wl
    # 8 192 cells, 4096 capacity
    want("4 096", "capacity token")
    want("8 192", "cell count token")

    bad = []
    for token, label in checks:
        if token not in doc:
            bad.append((label, token))
    print("%d figures checked, %d missing from the report" % (len(checks), len(bad)))
    for label, token in bad:
        print("   %-28s computed=%r not found" % (label, token))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
