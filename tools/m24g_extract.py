#!/usr/bin/env python3
"""M24-G — parse the sweep, validate it, and print the report tables.

Reads   build/wm/m24g/log/<cell>.txt   + build/wm/m24f/<trace>__{bulk,simt}.txt
Writes  build/wm/m24g/cells.csv         one row per cell, every metric named
        build/wm/m24g/pairs.csv         speedup per cell against its reference
        build/wm/m24g/checks.txt        validation and liveness
        build/wm/m24g/report.txt        the tables the report is written from

speedup is always M0-RM cycles / Bulk cycles, so 1.0 means parity.  The
*baseline* speedup of a trace is the M24-F cell of the unmodified M24-D pair
(no --set beyond the workload fields); every sweep cell is compared with it, so
a knob's value is read as "what it does to each arm", not "what it does to the
ratio of two differently-provisioned machines".
"""
import csv
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m24g")
M24F = os.path.join(ROOT, "build/wm/m24f")
CLK_HZ = 400e6
LINE = 64
GUARD = 500000000        # max_cycles in both M24-D configuration files
# The two cells the sweep found that do not terminate; see report §8.  They are
# excluded from every speedup table and reported as liveness results instead.
GUARDED = {"criteo_2k__l2mshr_128__bulk", "criteo_2k__cohorts_1__bulk"}

PRIMARY = ["w4_duplow", "criteo_2k", "n1_seq8m", "n3_clu4m"]
SECOND = ["w1_uniform", "orkut_hub", "n4_uni4m"]


def parse(path):
    d = {}
    if not os.path.exists(path):
        return None
    with open(path, errors="replace") as f:
        for line in f:
            if not line.strip() or line.startswith(("== ", "#", "M24", "  ")):
                continue
            for tok in line.split():
                if "=" not in tok:
                    continue
                k, v = tok.split("=", 1)
                if not k or not (k[0].isalpha() or k[0] == "_"):
                    continue
                try:
                    d[k] = float(v)
                except ValueError:
                    d[k] = v
    return d


def g(d, k, default=0.0):
    v = d.get(k, default)
    return v if isinstance(v, float) else default


def cellval(v):
    """exact integers as integers, everything else at 6 significant digits"""
    if isinstance(v, float):
        if abs(v - round(v)) < 1e-9 and abs(v) < 1e15:
            return str(int(round(v)))
        return "%.6g" % v
    return v


def fmt(x, nd=0):
    if x is None:
        return "—"
    if isinstance(x, str):
        return x
    if nd == 0:
        return "{:,.0f}".format(x).replace(",", "\u2009")
    return ("{:,.%df}" % nd).format(x).replace(",", "\u2009")


def f2(x):
    return "%.2f" % x


# ---------------------------------------------------------------- the cells --
def load():
    rows = []
    with open(os.path.join(OUT, "cells.tsv")) as f:
        for r in csv.DictReader(f, delimiter="\t"):
            d = parse(os.path.join(OUT, "log", r["cell"] + ".txt"))
            if d is None or "cycles" not in d:
                r["missing"] = True
                rows.append(r)
                continue
            r["missing"] = False
            r["d"] = d
            rows.append(r)
    return rows


def base(trace):
    b = parse(os.path.join(M24F, trace + "__bulk.txt"))
    s = parse(os.path.join(M24F, trace + "__simt.txt"))
    return b, s


def metrics(d):
    cyc = g(d, "cycles")
    reads = g(d, "dram_reads")
    banks = g(d, "dram", 2) or 2      # the echo's `dram=N banks x ...`
    out = {
        "cycles": cyc,
        "walks": g(d, "walks"),
        "walker_max": g(d, "walker_max"),
        "walker_busy": g(d, "walker_busy"),
        "walk_util": 100.0 * g(d, "walker_busy") / (cyc * max(1.0, g(d, "walkers"))) if cyc else None,
        "pte": g(d, "pte_fetches"),
        "l2tlb_stall": g(d, "l2tlb_walk_stalls"),
        "tlb_refuse_full": g(d, "tlb_refuse_full"),
        "tlb_refuse_parked": g(d, "tlb_refuse_parked"),
        "tlb_mshr_occ": 100.0 * g(d, "tlb0_mshr_occ") / (cyc * max(1.0, g(d, "tlb0_mshr_max"))) if cyc else None,
        "l2tlb_occ": g(d, "l2tlb_max_occ"),
        "inflight_max": g(d, "inflight_max"),
        "inflight_avg": g(d, "inflight_occ") / cyc if cyc else 0,
        "peak_out": g(d, "peak_total_outstanding"),
        "l1_mshr_max": g(d, "l1_mshr_max"),
        "l2_mshr_max": g(d, "l2_mshr_max"),
        "l2_mshr_occ": 100.0 * g(d, "l2_mshr_occ") / (cyc * max(1.0, g(d, "l2_mshr_max"))) if cyc else 0,
        "l2_reads": g(d, "l2_reads"),
        "l2_hits": g(d, "l2_hits"),
        "l2_hit": 100.0 * g(d, "l2_hits") / g(d, "l2_reads") if g(d, "l2_reads") else 0,
        "dram_reads": reads,
        "dram_bytes": g(d, "dram_bytes"),
        "row_hits": g(d, "dram_row_hits"),
        "row_conf": g(d, "dram_row_conflicts"),
        "row_hit": 100.0 * g(d, "dram_row_hits") / reads if reads else 0,
        "dram_busy": 100.0 * g(d, "dram_busy_cycles") / (cyc * max(1.0, banks)) if cyc else 0,
        "q_avg": g(d, "dram_q_occ_sum") / cyc if cyc else 0,
        "q_max": g(d, "dram_max_q"),
        "q_full": g(d, "dram_q_full"),
        "starved": g(d, "dram_starved_cycles"),
        "gbs": g(d, "dram_bytes") * CLK_HZ / cyc / 1e9 if cyc else 0,
        "eng": g(d, "bulk_share_x1000") / 10.0,
        "elems_done": g(d, "bulk_elems_done"),
        "elems_total": g(d, "bulk_elems_total"),
        "desc_sub": g(d, "bulk_desc_submitted"),
        "desc_rej": g(d, "bulk_desc_rejected"),
        "win_size": g(d, "bulk_win_size_mean"),
        "win_occ": g(d, "bulk_win_occ_sum") / cyc if cyc else 0,
        "seal_full": g(d, "bulk_seals_full"),
        "seal_dl": g(d, "bulk_seals_deadline"),
        "seal_end": g(d, "bulk_seals_end"),
        "ready_peak": g(d, "bulk_g6b_ready_peak"),
        "ready_avg": g(d, "bulk_g6b_ready_occ_sum") / cyc if cyc else 0,
        "g6b_full": g(d, "bulk_g6b_full_cycles"),
        "g6b_bp": g(d, "bulk_g6b_backpressure_cycles"),
        "g6b_noready": g(d, "bulk_g6b_no_ready_cycles"),
        "g6b_trstall": g(d, "bulk_g6b_tr_stall_cycles"),
        "g6b_relstall": g(d, "bulk_g6b_release_slot_stall"),
        "admit_idle": g(d, "bulk_g6c_admit_idle_cycles"),
        "refused": g(d, "bulk_g6c_refused"),
        "refuse_full": g(d, "bulk_g6c_refuse_full"),
        "refuse_parked": g(d, "bulk_g6c_refuse_parked"),
        "defer": g(d, "bulk_g6c_defer_events"),
        "slot_stall": g(d, "bulk_slot_stall"),
        "tgt_noslot": g(d, "bulk_tgt_refuse_noslot"),
        "win_full_stall": g(d, "bulk_win_full_stall"),
        "fanout_cyc": g(d, "bulk_fanout_cycles"),
        "org_cyc": g(d, "bulk_org_cycles"),
        "idx_wait": g(d, "bulk_idx_wait_stall"),
        "idx_refuse": g(d, "bulk_idx_refuse"),
        "buf_max": g(d, "bulk_buf_max"),
        "buf_stall": g(d, "bulk_buf_full_stall"),
        "sched_bytes": g(d, "bulk_sched_bytes"),
        "win_bytes": g(d, "bulk_win_bytes"),
        "pa_bytes": g(d, "bulk_g6b_pa_bytes"),
        "reuse_bytes": g(d, "bulk_reuse_vec_bytes"),
        "guard": 1.0 if (cyc >= GUARD or "cycle limit" in str(d.get("conservation", ""))) else 0.0,
        "cons": d.get("conservation_ok") or d.get("conservation"),
        "hash": d.get("completion_hash"),
        "max_cycles": g(d, "max_cycles"),
    }
    return out


def main():
    rows = load()
    miss = [r["cell"] for r in rows if r["missing"]]
    have = [r for r in rows if not r["missing"]]
    print("cells: planned %d, complete %d, missing %d" % (len(rows), len(have), len(miss)))
    for c in miss[:10]:
        print("  missing:", c)
    if miss:
        return 1

    # ------------------------------------------------------------- cells.csv --
    cols = ["cell", "kind", "sweep", "level", "arm", "trace", "knobs"]
    keys = list(metrics(have[0]["d"]).keys())
    with open(os.path.join(OUT, "cells.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(cols + keys)
        for r in have:
            m = metrics(r["d"])
            w.writerow([r["cell"], r["kind"], r["sweep"], r["level"], r["arm"], r["trace"], r["knobs"]]
                       + [cellval(m[k]) for k in keys])

    # ------------------------------------------------------------- checks ----
    chk = []
    ok = True

    def check(cond, text):
        nonlocal ok
        chk.append(("ok  " if cond else "FAIL") + "  " + text)
        if not cond:
            ok = False

    tr_of = {}
    for r in have:
        tr_of.setdefault((r["kind"], r["sweep"], r["level"], r["trace"]), {})[r["arm"]] = r
    def cons_ok(d):
        return str(d.get("conservation_ok", 1)) in ("1", "1.0", "OK", "True")

    guarded = sorted(r["cell"] for r in have if metrics(r["d"])["guard"])
    bad = [r["cell"] for r in have
           if not cons_ok(r["d"]) and r["cell"] not in GUARDED]
    check(not bad, "every cell that terminates conserves accesses/requests (%d of %d cells)"
          % (len(have) - len(guarded), len(have)))
    check(set(guarded) == GUARDED,
          "the only non-terminating cell is the known one: %s" % ", ".join(guarded or ["none"]))

    for (kind, sweep, lv, tr), arms in sorted(tr_of.items()):
        if "bulk" in arms and "simt" in arms:
            b, s = arms["bulk"], arms["simt"]
            check(g(b["d"], "trace_accesses") == g(s["d"], "trace_accesses"),
                  "%s/%s/%s: both arms see %s accesses" % (tr, sweep, lv, fmt(g(b["d"], "trace_accesses"))))
            check(g(b["d"], "bulk_elems_done") == g(b["d"], "bulk_elems_total"),
                  "%s/%s/%s: engine completed every element (%s)" % (tr, sweep, lv, fmt(g(b["d"], "bulk_elems_done"))))
            # DRAM row identity: hits + conflicts + 2 (row-miss first access) = reads
            for arm in ("bulk", "simt"):
                d = arms[arm]["d"]
                lhs = g(d, "dram_row_hits") + g(d, "dram_row_conflicts") + 2 * g(d, "dram_banks_used", 2)
                if "dram_row_hits" in d:
                    check(abs(lhs - g(d, "dram_reads")) <= 2 * 16,
                          "%s/%s/%s/%s: row hits + conflicts + first-touch = reads" % (tr, sweep, lv, arm))
                    break

    # a common-resource cell gives both arms the same knob value, so the pair
    # differs only in its configuration file (the M24-D pair, audited in M24-D
    # §2).  Assert the plan actually did that.
    for (kind, sweep, lv, tr), arms in sorted(tr_of.items()):
        if kind in ("common", "diag") and "bulk" in arms and "simt" in arms:
            check(arms["bulk"]["knobs"] == arms["simt"]["knobs"],
                  "%s/%s/%s: both arms received the same knob" % (tr, sweep, lv))
    # exactly one knob per common cell; the diagnostic point is the only cell set
    # that moves three, and it is a single explicit translation-concurrency level
    multi = [r["cell"] for r in have if r["kind"] == "common" and r["knobs"].count(";") > 0]
    check(not multi, "every common-resource cell moves exactly one knob")
    diag = [r for r in have if r["kind"] == "diag"]
    check(len(diag) == 8 and all(r["sweep"] == "translation16" for r in diag),
          "the diagnostic cells are the one 3-knob translation point (%d cells)" % len(diag))

    # ---------------------------------------------------- determinism repeats --
    rep = []
    for name in ("repeat1", "repeat2"):
        a = parse(os.path.join(OUT, name + ".txt"))
        if a is None:
            rep.append((name, None, None))
            continue
        b = parse(os.path.join(M24F, a.get("_cell_trace", "w4_duplow") + "__bulk.txt")) if False else None
        rep.append((name, g(a, "cycles"), a.get("completion_hash")))

    # ------------------------------------------------------------- pairs.csv --
    with open(os.path.join(OUT, "pairs.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["trace", "kind", "sweep", "level", "arm_of_reference",
                    "bulk_cycles", "simt_cycles", "speedup", "base_bulk", "base_simt",
                    "base_speedup", "dspeedup"])
        for r in have:
            m = metrics(r["d"])
            bb, bs = base(r["trace"])
            base_sp = g(bs, "cycles") / g(bb, "cycles")
            if r["kind"] in ("common", "diag"):
                pair = tr_of[(r["kind"], r["sweep"], r["level"], r["trace"])]
                bc = g(pair["bulk"]["d"], "cycles")
                sc = g(pair["simt"]["d"], "cycles")
                gd = metrics(pair["bulk"]["d"])["guard"]
                w.writerow([r["trace"], r["kind"], r["sweep"], r["level"], "paired",
                            "%.0f" % bc, "%.0f" % sc,
                            "guard" if gd else "%.4f" % (sc / bc),
                            "%.0f" % g(bb, "cycles"), "%.0f" % g(bs, "cycles"),
                            "%.4f" % base_sp,
                            "" if gd else "%.4f" % (sc / bc - base_sp)])
            else:
                bc = g(r["d"], "cycles")
                gd = metrics(r["d"])["guard"]
                w.writerow([r["trace"], r["kind"], r["sweep"], r["level"], "m24f_simt",
                            "%.0f" % bc, "%.0f" % g(bs, "cycles"),
                            "guard" if gd else "%.4f" % (g(bs, "cycles") / bc),
                            "%.0f" % g(bb, "cycles"), "%.0f" % g(bs, "cycles"),
                            "%.4f" % base_sp,
                            "" if gd else "%.4f" % (g(bs, "cycles") / bc - base_sp)])

    out = []
    out.append("M24-G tables")
    out.append("")
    out.append("== 0. baseline (M24-D pair, unchanged, from build/wm/m24f) ==")
    out.append("%-12s %12s %12s %8s %8s %8s" % ("trace", "M0-RM", "Bulk", "speedup", "walks", "row%"))
    for tr in PRIMARY + SECOND:
        bb, bs = base(tr)
        out.append("%-12s %12s %12s %8.3f %8s %8.1f" % (
            tr, fmt(g(bs, "cycles")), fmt(g(bb, "cycles")),
            g(bs, "cycles") / g(bb, "cycles"), fmt(g(bb, "walks")),
            metrics(bb)["row_hit"]))
    out.append("")

    # ------------------------------------------------- table 1: common knobs --
    out.append("== 1. common resources: what the knob does to each arm ==")
    out.append("%-10s %-12s %-6s %12s %12s %8s %12s %8s %8s" % (
        "sweep", "trace", "level", "M0-RM", "Bulk", "speedup", "dSpeedup", "M0 d%", "Bulk d%"))
    for sweep in ["walkers", "dtlbmshr", "l2tlbmshr", "translation16", "rbudget", "l2mshr", "dramq", "banks"]:
        for tr in PRIMARY + SECOND:
            key = ("common", sweep, None, tr) if sweep != "translation16" else ("diag", sweep, None, tr)
            levels = sorted({r["level"] for r in have if r["sweep"] == sweep and r["trace"] == tr},
                            key=lambda x: float(x))
            if not levels:
                continue
            bb, bs = base(tr)
            for lv in levels:
                arms = tr_of.get(("common" if sweep != "translation16" else "diag", sweep, lv, tr))
                if not arms:
                    continue
                bc, sc = g(arms["bulk"]["d"], "cycles"), g(arms["simt"]["d"], "cycles")
                if metrics(arms["bulk"]["d"])["guard"]:
                    out.append("%-10s %-12s %-6s %12s %12s %8s %8s %8s %8s" % (
                        sweep, tr, lv, fmt(sc), "GUARD", "—", "—", "—", "—"))
                    continue
                sp = sc / bc
                bsp = g(bs, "cycles") / g(bb, "cycles")
                out.append("%-10s %-12s %-6s %12s %12s %8.3f %+8.3f %+8.1f %+8.1f" % (
                    sweep, tr, lv, fmt(sc), fmt(bc), sp, sp - bsp,
                    100 * (sc / g(bs, "cycles") - 1), 100 * (bc / g(bb, "cycles") - 1)))
        out.append("")

    # --------------------------------------- table 1b: aggregate by level ------
    def med(xs):
        xs = sorted(xs)
        n = len(xs)
        return xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])

    out.append("== 1b. the same, aggregated over the four primary traces ==")
    out.append("%-11s %-6s %8s %8s %8s %8s %8s" % (
        "sweep", "level", "med speedup", "min", "max", "med vs base", "n"))
    for sweep in ["walkers", "dtlbmshr", "l2tlbmshr", "translation16", "rbudget", "l2mshr", "dramq", "banks"]:
        kind = "diag" if sweep == "translation16" else "common"
        levels = sorted({r["level"] for r in have if r["sweep"] == sweep},
                        key=lambda x: float(x))
        for lv in levels:
            sps, dss = [], []
            for tr in PRIMARY:
                arms = tr_of.get((kind, sweep, lv, tr))
                if not arms or metrics(arms["bulk"]["d"])["guard"]:
                    continue
                bb, bs = base(tr)
                sp = g(arms["simt"]["d"], "cycles") / g(arms["bulk"]["d"], "cycles")
                sps.append(sp)
                dss.append(sp - g(bs, "cycles") / g(bb, "cycles"))
            if not sps:
                continue
            out.append("%-11s %-6s %8.3f %8.3f %8.3f %+8.3f %8d" % (
                sweep, lv, med(sps), min(sps), max(sps), med(dss), len(sps)))
        out.append("")

    # ------------------------------------------- table 2: translation detail --
    out.append("== 2. translation: effective concurrency and its ceiling ==")
    out.append("%-10s %-12s %-6s %-5s %6s %6s %8s %9s %12s %12s %10s" % (
        "sweep", "trace", "level", "arm", "wmax", "wutil", "tlbocc", "l2tlbOk", "refuse_full", "l2tlb_stall", "walks"))
    for sweep in ["walkers", "dtlbmshr", "l2tlbmshr", "translation16"]:
        for tr in PRIMARY:
            for lv in sorted({r["level"] for r in have if r["sweep"] == sweep and r["trace"] == tr},
                             key=lambda x: float(x)):
                arms = tr_of.get(("common" if sweep != "translation16" else "diag", sweep, lv, tr))
                if not arms:
                    continue
                for arm in ("simt", "bulk"):
                    m = metrics(arms[arm]["d"])
                    out.append("%-10s %-12s %-6s %-5s %6s %5.1f%% %7.1f%% %9s %12s %12s %10s" % (
                        sweep, tr, lv, arm, fmt(m["walker_max"]), m["walk_util"], m["tlb_mshr_occ"],
                        fmt(m["l2tlb_occ"]), fmt(m["tlb_refuse_full"]), fmt(m["l2tlb_stall"]),
                        fmt(m["walks"])))
        out.append("")

    # ---------------------------------------------- table 3: shared budget ----
    out.append("== 3. outstanding capacity: R, L2 MSHRs, and what saturates ==")
    out.append("%-10s %-12s %-6s %-5s %6s %6s %8s %8s %8s %8s %8s" % (
        "sweep", "trace", "level", "arm", "infmx", "infavg", "peak", "l1mx", "l2mx", "l2occ", "qavg"))
    for sweep in ["rbudget", "l2mshr"]:
        for tr in PRIMARY:
            for lv in sorted({r["level"] for r in have if r["sweep"] == sweep and r["trace"] == tr},
                             key=lambda x: float(x)):
                arms = tr_of.get(("common", sweep, lv, tr))
                if not arms:
                    continue
                for arm in ("simt", "bulk"):
                    m = metrics(arms[arm]["d"])
                    out.append("%-10s %-12s %-6s %-5s %6s %6.1f %8s %8s %8s %7.1f%% %8.1f" % (
                        sweep, tr, lv, arm, fmt(m["inflight_max"]), m["inflight_avg"],
                        fmt(m["peak_out"]), fmt(m["l1_mshr_max"]), fmt(m["l2_mshr_max"]),
                        m["l2_mshr_occ"], m["q_avg"]))
        out.append("")

    # ------------------------------------------------------ table 4: DRAM ------
    out.append("== 4. DRAM: banks and queue depth ==")
    out.append("%-10s %-12s %-6s %-5s %10s %10s %8s %7s %7s %8s %8s" % (
        "sweep", "trace", "level", "arm", "reads", "row_hits", "row%", "busy", "qavg", "qmax", "GB/s"))
    for sweep in ["banks", "dramq"]:
        for tr in PRIMARY:
            for lv in sorted({r["level"] for r in have if r["sweep"] == sweep and r["trace"] == tr},
                             key=lambda x: float(x)):
                arms = tr_of.get(("common", sweep, lv, tr))
                if not arms:
                    continue
                for arm in ("simt", "bulk"):
                    m = metrics(arms[arm]["d"])
                    out.append("%-10s %-12s %-6s %-5s %10s %10s %7.1f%% %6.1f%% %7.1f %8s %7.2f" % (
                        sweep, tr, lv, arm, fmt(m["dram_reads"]), fmt(m["row_hits"]),
                        m["row_hit"], m["dram_busy"], m["q_avg"], fmt(m["q_max"]), m["gbs"]))
        out.append("")

    # ------------------------------------------------ table 5: bulk capacity --
    out.append("== 5. Bulk-specific capacities (Bulk arm only; SIMT reference fixed) ==")
    out.append("%-10s %-12s %-6s %12s %8s %8s %8s %8s %8s %9s %9s" % (
        "sweep", "trace", "level", "Bulk cyc", "vs base", "speedup", "win_avg", "sealsD", "admitIdle", "refused", "readyPk"))
    for sweep, _, _ in [("cohorts", "", []), ("window", "", []), ("readypool", "", []),
                        ("pagedir", "", []), ("orglanes", "", []), ("fanout", "", []),
                        ("idxmshr", "", []), ("aglanes", "", [])]:
        for tr in PRIMARY:
            bb, bs = base(tr)
            levels = sorted({r["level"] for r in have if r["sweep"] == sweep and r["trace"] == tr},
                            key=lambda x: float(x))
            for lv in levels:
                r = tr_of.get(("bulk", sweep, lv, tr), {}).get("bulk")
                if not r:
                    continue
                m = metrics(r["d"])
                out.append("%-10s %-12s %-6s %12s %7.3fx %8.3f %8.0f %8s %9s %9s %9s" % (
                    sweep, tr, lv, fmt(m["cycles"]), m["cycles"] / g(bb, "cycles"),
                    g(bs, "cycles") / m["cycles"], m["win_occ"], fmt(m["seal_dl"]),
                    fmt(m["admit_idle"]), fmt(m["refused"]), fmt(m["ready_peak"])))
        out.append("")

    # ------------------------------------------------------ table 6: storage --
    out.append("== 6. storage of the swept structures (bytes, from the simulator) ==")
    out.append("%-12s %-28s %10s %10s %10s" % ("trace", "configuration", "window", "sched/cohort", "ready pool"))
    for tr in PRIMARY:
        bb, _ = base(tr)
        mb = metrics(bb)
        for sweep, lv in [("window", "8192"), ("window", "16384base"), ("window", "32768"),
                          ("cohorts", "1"), ("cohorts", "8base"), ("cohorts", "16"),
                          ("readypool", "512"), ("readypool", "4096base"), ("readypool", "8192")]:
            if lv.endswith("base"):
                m = mb
                name = sweep + "=" + lv.replace("base", " (production)")
            else:
                r = tr_of.get(("bulk", sweep, lv, tr), {}).get("bulk")
                if not r:
                    continue
                m = metrics(r["d"])
                name = "%s=%s" % (sweep, lv)
            out.append("%-12s %-28s %10s %10s %10s" % (
                tr, name, fmt(m["win_bytes"]), fmt(m["sched_bytes"]), fmt(m["pa_bytes"])))
    out.append("")

    # ------------------------------------------------------ table 7: frontier --
    out.append("== 7. resource/performance frontier points ==")
    out.append("%-12s %-10s %-6s %-5s %10s %8s %8s %10s %8s" % (
        "trace", "sweep", "level", "arm", "cycles", "walks", "row%", "gbps", "SRAM B"))
    for tr in PRIMARY:
        for sweep in ("walkers", "rbudget", "banks"):
            for lv in sorted({r["level"] for r in have if r["sweep"] == sweep and r["trace"] == tr},
                             key=lambda x: float(x)):
                for arm in ("simt", "bulk"):
                    r = tr_of.get(("common", sweep, lv, tr), {}).get(arm)
                    if not r:
                        continue
                    m = metrics(r["d"])
                    out.append("%-12s %-10s %-6s %-5s %10s %8s %7.1f%% %9.2f %8s" % (
                        tr, sweep, lv, arm, fmt(m["cycles"]), fmt(m["walks"]), m["row_hit"],
                        m["gbs"], fmt(m["win_bytes"] + m["sched_bytes"] + m["pa_bytes"])))
    out.append("")
    out.append("== 7b. combinations (Bulk arm only, two knobs at once) ==")
    out.append("%-24s %-12s %12s %8s %10s %10s %10s" % (
        "combination", "trace", "Bulk cyc", "vs base", "SRAM B", "vs single", "speedup"))
    singles = {("banks", "16"): "banks", ("window", "32768"): "window", ("readypool", "512"): "readypool"}
    for sweep in ("combo_banks16_pool512", "combo_win32k_pool512"):
        for tr in PRIMARY:
            r = tr_of.get(("combo", sweep, "combo", tr), {}).get("bulk")
            if not r:
                continue
            m = metrics(r["d"])
            bb, bs = base(tr)
            # the single-knob cell this combination stacks on top of
            ref = "banks_16" if "banks16" in sweep else "window_32768"
            kn, kl = ref.split("_", 1)
            s = (tr_of.get(("common", kn, kl, tr)) or tr_of.get(("bulk", kn, kl, tr)) or {}).get("bulk")
            single = ("%+.1f%%" % (100 * (m["cycles"] / metrics(s["d"])["cycles"] - 1))) if s else "—"
            out.append("%-24s %-12s %12s %7.3fx %10s %10s %10.3f" % (
                sweep, tr, fmt(m["cycles"]), m["cycles"] / g(bb, "cycles"),
                fmt(m["win_bytes"] + m["sched_bytes"] + m["pa_bytes"]), single,
                g(bs, "cycles") / m["cycles"]))
    out.append("")

    out.append("== 8. liveness ==")

    for cell in sorted(GUARDED):
        d = parse(os.path.join(OUT, "log", cell + ".txt"))
        if d is None:
            continue
        m = metrics(d)
        out.append("%s: cycles=%s conservation=%s" % (cell, fmt(m["cycles"]), d.get("conservation")))
        out.append("  elems done %s / %s, requests generated/issued/done %s / %s / %s, "
                   "desc submitted/completed %s / %s" % (
                       fmt(m["elems_done"]), fmt(m["elems_total"]),
                       fmt(g(d, "bulk_tgt_req_gen")), fmt(g(d, "bulk_tgt_req_issued")),
                       fmt(g(d, "bulk_tgt_req_done")), fmt(m["desc_sub"]), fmt(d.get("bulk_desc_completed", 0))))
        out.append("  inflight_max %s, l2_mshr_max %s, unissued_max %s, slot_stall %s" % (
            fmt(m["inflight_max"]), fmt(m["l2_mshr_max"]),
            fmt(g(d, "bulk_unissued_max")), fmt(m["slot_stall"])))
        out.append("  conservation: %s" % d.get("conservation", ""))
    for q in ("64", "96", "128", "160"):
        p = os.path.join(OUT, "probe_l2mshr_%s.txt" % q)
        if not os.path.exists(p):
            continue
        d = parse(p)
        if not d or "cycles" not in d:
            continue
        out.append("  probe l2_mshr=%s: cycles=%s %s" % (q, fmt(g(d, "cycles")), d.get("conservation", "")))
    out.append("")

    out.append("== checks ==")
    out.extend(chk)
    txt = "\n".join(out) + "\n"
    with open(os.path.join(OUT, "report.txt"), "w") as f:
        f.write(txt)
    with open(os.path.join(OUT, "checks.txt"), "w") as f:
        f.write("\n".join(chk) + "\n")
    print(txt[:6000])
    print("checks: %d, failures %d" % (len(chk), sum(1 for c in chk if c.startswith("FAIL"))))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
