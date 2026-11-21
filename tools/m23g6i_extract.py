#!/usr/bin/env python3
"""M23-G6I — continuous translation/DRAM pipeline: cell extractor + checks.

Reads build/wm/m23g6i/g6i_<cell>_<workload>.txt and prints the tables used in
docs/m23-g6i-continuous-translation-dram-pipeline.md, writes summary.csv, and
verifies the identities the brief requires.

  A0  pool 4096, diagnostic off   (must reproduce M23-G6H's G0 cell exactly)
  A1  pool 4096, diagnostic on    (must equal A0 cycle for cycle)
  B   pool 8192, diagnostic on    (the only parameter that changes)

Derived quantities:
  tr/cyc    bulk_g6b_tr_completed / cycles            translation throughput
  pool      bulk_g6b_ready_occ_sum / cycles           mean pool occupancy
  vpns      bulk_g6i_vpn_sum / samples                virtual pages in the pool
  cells     bulk_g6i_cells_sum / samples              physical (bank,row) cells
  xpg       bulk_g6i_xpage_cells_sum / samples        cells fed by >= 2 pages
  ent/cell  pool / cells                              runs per physical row cell
  rel/cyc   bulk_g6b_release_pops / cycles
  gap       bulk_g6i_gap_sum / gap_n                  cycles between same-row releases
  sw%       page switches among consecutive releases
"""
import os
import sys

WLS = ["orkut", "livejournal", "ogbntrain"]
CELLS = [("A0", 4096, 0), ("A1", 4096, 1), ("B", 8192, 1)]
DIR = "build/wm/m23g6i"
SKIP = ("bulk_g6i_", "g6i_diag")


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


def g(kv, name, default=0):
    return kv.get(name, default)


def as_dict(kv):
    c = kv["cycles"]
    pop = kv["bulk_g6b_release_pops"]
    samp = g(kv, "bulk_g6i_samples")
    rel_cyc = g(kv, "bulk_g6i_rel_cycles")
    gapn = g(kv, "bulk_g6i_gap_n")
    switch = g(kv, "bulk_g6i_rel_vpn_switch")
    same = g(kv, "bulk_g6i_rel_same_vpn")
    pool = kv["bulk_g6b_ready_occ_sum"] / c
    cells = (g(kv, "bulk_g6i_cells_sum") / samp) if samp else 0.0
    return dict(
        cycles=c,
        reads=kv["dram_reads"],
        hit=100.0 * kv["dram_row_hits"] / kv["dram_reads"],
        acts=kv["dram_row_acts_sum"],
        rpa=kv["dram_reads"] / kv["dram_row_acts_sum"],
        util=100.0 * kv["dram_busy_cycles"] / (2 * c),
        qmean=kv["dram_q_occ_sum"] / (g(kv, "dram_diag_decisions") or 1),
        tr_cyc=kv["bulk_g6b_tr_completed"] / c,
        tr_done=kv["bulk_g6b_tr_completed"],
        tr_req=kv["bulk_g6b_tr_requests"],
        adm=g(kv, "bulk_g6c_admit_accepted"),
        att=g(kv, "bulk_g6c_admit_attempts"),
        at_per=g(kv, "bulk_g6c_admit_attempts") / max(1, g(kv, "bulk_g6c_admit_accepted")),
        coh=g(kv, "bulk_g6c_cohorts_opened"),
        refus=g(kv, "bulk_g6c_refused"),
        refpark=g(kv, "bulk_g6c_refuse_parked"),
        reffull=g(kv, "bulk_g6c_refuse_full"),
        defer_other=g(kv, "bulk_g6c_defer_then_other_admit"),
        readmit=g(kv, "bulk_g6c_deferred_readmitted"),
        idle=g(kv, "bulk_g6c_admit_idle_cycles"),
        idle_pct=100.0 * g(kv, "bulk_g6c_admit_idle_cycles") / c,
        tr_stall=g(kv, "bulk_g6b_tr_stall_cycles"),
        full=g(kv, "bulk_g6b_full_cycles"),
        full_pct=100.0 * g(kv, "bulk_g6b_full_cycles") / c,
        pool=pool,
        poolmax=kv["bulk_g6b_ready_peak"],
        cap=kv["bulk_g6b_pa_capacity"],
        empt=g(kv, "bulk_g6i_pool_empty"),
        empt_pct=100.0 * g(kv, "bulk_g6i_pool_empty") / c,
        over=g(kv, "bulk_g6i_pool_over"),
        samp=samp,
        spool=(g(kv, "bulk_g6i_samp_pool_sum") / samp) if samp else 0.0,
        spoolmax=g(kv, "bulk_g6i_samp_pool_max"),
        vpns=(g(kv, "bulk_g6i_vpn_sum") / samp) if samp else 0.0,
        vpnmax=g(kv, "bulk_g6i_vpn_max"),
        cells=cells,
        cellsmax=g(kv, "bulk_g6i_cells_max"),
        ent_cell=(pool / cells) if cells else 0.0,
        xpg=(g(kv, "bulk_g6i_xpage_cells_sum") / samp) if samp else 0.0,
        xpgmax=g(kv, "bulk_g6i_xpage_cells_max"),
        xpgent=g(kv, "bulk_g6i_xpage_entries_sum"),
        pagesopen=g(kv, "bulk_g6i_pages_open_max"),
        pagesopen_mean=(g(kv, "bulk_g6i_pages_open_sum") / samp) if samp else 0.0,
        untr=(g(kv, "bulk_g6i_untr_sum") / samp) if samp else 0.0,
        untrmax=g(kv, "bulk_g6i_untr_max"),
        pops=pop,
        relcyc=rel_cyc,
        rel=pop / c,
        gap=(g(kv, "bulk_g6i_gap_sum") / gapn) if gapn else 0.0,
        gapmax=g(kv, "bulk_g6i_gap_max"),
        switch=switch,
        same=same,
        swpct=(100.0 * switch / (switch + same)) if (switch + same) else 0.0,
        reltr=g(kv, "bulk_g6i_rel_with_tr"),
        reltrpct=(100.0 * g(kv, "bulk_g6i_rel_with_tr") / rel_cyc) if rel_cyc else 0.0,
        overlap=100.0 * g(kv, "bulk_g6b_overlap_cycles") / c,
        trail=100.0 * g(kv, "bulk_g6b_tr_active_cycles") / c,
        tgtp=100.0 * g(kv, "bulk_g6b_tgt_out_cycles") / c,
        noready=g(kv, "bulk_g6b_no_ready_cycles"),
        blocked=g(kv, "bulk_g6b_release_blocked_cycles"),
        slotstall=g(kv, "bulk_g6b_release_slot_stall"),
        sojourn=(kv["bulk_g6b_tr_wait_dram"] / kv["bulk_g6b_runs_released"])
        if g(kv, "bulk_g6b_runs_released") else 0.0,
        trwaitmax=kv["bulk_g6b_tr_wait_max"],
        l2hit=100.0 * kv["cc3_l2_hits"] / kv["cc3_l2_reads"],
        l2r=kv["cc3_l2_reads"],
        walks=kv["walks"],
        mshr=kv["l2_mshr_max"],
        qmax=kv["dram_max_q"],
        qfull=kv["dram_q_full"],
        missed=kv["dram_missed_hit"],
        gen=kv["bulk_tgt_req_gen"],
        iss=kv["bulk_tgt_req_issued"],
        done=kv["bulk_tgt_req_done"],
        c3i=kv["cc3_issue"],
        c3d=kv["cc3_done"],
        rel_runs=kv["bulk_g6b_runs_released"],
        cons=g(kv, "conservation_ok"),
        viol=g(kv, "conservation_violations", -1),
        elems=g(kv, "bulk_elems_done"),
        total=g(kv, "bulk_elems_total"),
        resviol=g(kv, "bulk_residency_violations"),
        uniss=g(kv, "requests_unissued_end", -1),
        win=g(kv, "bulk_g6b_windows"),
    )


HEAD = ("cell pool      cycles   dCyc%   reads   hit%  reads/act    acts | "
        "tr/cyc  pool poolmx  cap vpns vMax cells cMax e/cl  xpg pgOpn | "
        "rel/cyc   gap  sw%  rl/tr | full%")
FMT = ("%-4s %5d %11d %+7.3f %7d %6.2f %9.4f %7d | "
       "%5.2f %5.1f %6d %4d %4.1f %4d %5.1f %4d %4.2f %4d %5.1f | "
       "%6.2f %6.0f %4.1f %5.1f | %5.1f")


def main():
    rows = []
    ok = True
    for wl in WLS:
        cells = {}
        for name, _, _ in CELLS:
            p = os.path.join(DIR, "g6i_%s_%s.txt" % (name, wl))
            if not os.path.exists(p) or os.path.getsize(p) == 0:
                # A0 (the feature-off cell) exists for Orkut only; A1 carries the
                # same baseline on the binary that has the diagnostic.
                if name != "A0":
                    print("missing %s" % p, file=sys.stderr)
                    ok = False
                continue
            cells[name] = parse(p)
        if "A1" not in cells or "B" not in cells:
            ok = False
            continue
        base = as_dict(cells["A1"])
        print("== %s" % wl)
        print(HEAD)
        for name, cap, diag in CELLS:
            if name not in cells:
                continue
            r = as_dict(cells[name])
            r["dcyc"] = 100.0 * (r["cycles"] - base["cycles"]) / base["cycles"]
            rows.append((wl, name, cap, diag, r))
            print(FMT % (name, cap, r["cycles"], r["dcyc"], r["reads"], r["hit"],
                         r["rpa"], r["acts"], r["tr_cyc"], r["pool"], r["poolmax"],
                         r["cap"], r["vpns"], r["vpnmax"], r["cells"], r["cellsmax"],
                         r["ent_cell"], r["xpg"], r["pagesopen"], r["rel"], r["gap"],
                         r["swpct"], r["reltrpct"], r["full_pct"]))
        print("   untranslated runs in the window: mean %.0f max %d | translation "
              "attempts/admission %.1f | admission idle %.1f %% | admit-defer-other %d"
              % (base["untr"], base["untrmax"], base["at_per"], base["idle_pct"],
                 base["defer_other"]))
        print()

    if rows:
        out = os.path.join(DIR, "summary.csv")
        cols = [(k, ("%d" if isinstance(v, int) else "%.4f"))
                for k, v in rows[0][4].items() if not isinstance(v, str)]
        with open(out, "w") as f:
            f.write("workload,cell,pool_capacity,diag," +
                    ",".join(c for c, _ in cols) + "\n")
            for wl, name, cap, diag, r in rows:
                f.write("%s,%s,%d,%d," % (wl, name, cap, diag) +
                        ",".join(fmt % r[k] for k, fmt in cols) + "\n")
        print("wrote %s" % out)

    print("== checks")
    for wl in WLS:
        for name, cap, diag in CELLS:
            p = os.path.join(DIR, "g6i_%s_%s.txt" % (name, wl))
            if not os.path.exists(p):
                if name != "A0":   # A0 exists for Orkut only; A1 is the baseline
                    ok = False
                continue
            k = parse(p)
            r = as_dict(k)
            fails = []

            def req(cond, msg):
                if not cond:
                    fails.append(msg)

            req(r["cap"] == cap, "pool capacity is %d, expected %d" % (r["cap"], cap))
            req(r["poolmax"] <= cap, "pool peak above capacity")
            req(r["over"] == 0, "pool exceeded capacity in %d cycles" % r["over"])
            req(r["tr_req"] == r["tr_done"], "translation requests != completions")
            req(r["rel_runs"] == r["tr_done"], "released != translated")
            req(r["adm"] == r["tr_req"], "admissions != translation requests")
            req(g(k, "bulk_g6c_dup_submit_blocked") == 0, "a run was admitted twice")
            req(g(k, "bulk_g6c_refuse_other") == 0, "a refusal had no named cause")
            req(r["gen"] == r["iss"] == r["done"], "target conservation")
            req(r["c3i"] == r["c3d"], "cc3 issue != done")
            req(r["elems"] == r["total"], "elements lost")
            req(r["cons"] == 1 and r["viol"] == 0, "conservation failure")
            req(r["resviol"] == 0, "residency violation")
            req(r["mshr"] <= 64, "L2 MSHR file exceeded")
            req(r["qmax"] <= 64, "DRAM queue bound exceeded")
            req(r["qfull"] == 0, "DRAM queue was full")
            req(r["missed"] == 0, "an open-row hit was missed")
            req(r["uniss"] == 0, "requests left unissued")
            # sojourn identity: the occupancy integral is the sum of pool times
            req(k["bulk_g6b_ready_occ_sum"] == k["bulk_g6b_tr_wait_dram"],
                "pool occupancy integral != summed sojourn")
            if diag:
                req(r["samp"] > 0, "no pool sample")
                req(r["switch"] + r["same"] + r["win"] == r["rel_runs"],
                    "page-switch accounting does not close")
                req(r["vpnmax"] >= 1, "no page in the pool")
                req(r["cellsmax"] >= 1, "no physical cell in the pool")
            else:
                req(r["samp"] == 0, "diagnostic off but counters present")
            status = "PASS" if not fails else "FAIL"
            print("%-12s %-3s pool=%-5d %s" % (wl, name, cap, status))
            for m in fails:
                print("    - %s" % m)
            ok &= not fails

    for wl in WLS:
        pa = os.path.join(DIR, "g6i_A0_%s.txt" % wl)
        pb = os.path.join(DIR, "g6i_A1_%s.txt" % wl)
        if not os.path.exists(pb):
            ok = False
            continue
        b = parse(pb)
        ref = b
        if os.path.exists(pa):
            a = parse(pa)
            ref = a
            keys = sorted(x for x in a if not x.startswith(SKIP))
            diff = [(x, a[x], b[x]) for x in keys if b.get(x) != a[x]]
            print("== A0 vs A1 %-12s %d keys, %d differ" % (wl, len(keys), len(diff)))
            for x, u, v in diff[:8]:
                print("   %-30s off=%s on=%s" % (x, u, v))
            if diff:
                ok = False
        h6 = "build/wm/m23g6h/g6h_G0r_%s.txt" % wl
        if os.path.exists(h6):
            h = parse(h6)
            keys = sorted(x for x in ref if not x.startswith(SKIP))
            diff = [(x, h.get(x), ref[x]) for x in keys if h.get(x) != ref[x]]
            print("== G6H G0 repro %-12s %d keys, %d differ (%s)"
                  % (wl, len(keys), len(diff), "A0" if ref is not b else "A1"))
            for x, u, v in diff[:8]:
                print("   %-30s g6h=%s g6i=%s" % (x, u, v))
            if diff:
                ok = False
    print("ALL CHECKS PASSED" if ok else "FAILURES PRESENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
