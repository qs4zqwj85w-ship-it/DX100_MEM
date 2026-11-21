#!/usr/bin/env python3
"""M23-G6H — required invariants, checked on every cell.

  ./tools/m23g6h_check.py

Cells: the G6H G0r/P2F logs and the frozen G6G arm-1 logs. Prints one line per
cell and exits non-zero on any violation. Nothing here is a performance metric:
these are the identities the brief makes mandatory (visibility invariant,
conservation, no duplicate issue/completion, resource limits).
"""
import glob
import os
import sys

CAP = 4096          # configs/m23g6b_base.cfg: g6b_pa_capacity
L2_MSHR = 64        # configs/m23g6b_base.cfg: l2_mshr
DRAM_Q = 64         # dram_q = 0 (unbounded model) -> the modelled max is R = 128;
                    # the measured peak must stay inside the queue's own bound.


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


def arm_of(path):
    """The release arm, from the cell name (the config echo packs several keys on
    one line, so `sched_group` is not a counter of its own)."""
    b = os.path.basename(path)
    if b.startswith("g6h_G0r_"):
        return 0
    if b.startswith("g6h_P2F_"):
        return 3
    if b.startswith("g6h_G1r_"):
        return 1
    return -1


def check(path):
    kv = parse(path)
    arm = os.path.basename(path)
    grp = arm_of(path)
    fails = []
    if grp < 0:
        return True

    def req(cond, msg):
        if not cond:
            fails.append(msg)

    req(kv.get("bulk_g6b_pa_capacity", 0) == CAP, "capacity is not %d" % CAP)
    req(kv.get("bulk_g6b_ready_peak", 0) <= CAP, "ready peak above capacity")
    req(kv.get("bulk_g6g_sel_mismatch", 0) == 0, "picked cell != released cell")
    req(kv.get("bulk_tgt_req_gen") == kv.get("bulk_tgt_req_issued"),
        "generated != issued")
    req(kv.get("bulk_tgt_req_issued") == kv.get("bulk_tgt_req_done"),
        "issued != completed")
    req(kv.get("bulk_elems_done") == kv.get("bulk_elems_total"), "elements lost")
    req(kv.get("cc3_issue") == kv.get("cc3_done"), "cc3 issue != done")
    req(kv.get("conservation_ok") == 1, "conservation_ok != 1")
    req(kv.get("conservation_violations") == 0, "conservation violations")
    req(kv.get("bulk_residency_violations") == 0, "residency violations")
    req(kv.get("l2_mshr_max", 0) <= L2_MSHR, "L2 MSHR file exceeded")
    req(kv.get("dram_max_q", 0) <= DRAM_Q, "DRAM queue bound exceeded")
    req(kv.get("dram_q_full", 1) == 0, "DRAM queue was full")
    req(kv.get("dram_missed_hit", 1) == 0, "an open-row hit was missed")
    req(kv.get("requests_unissued_end", 1) == 0, "requests left unissued")

    if grp == 0:
        # The control does not enumerate: it pops the heap front, so the
        # organizer counters are absent by construction, not by a limit.
        req(kv.get("bulk_g6h_decisions", 0) == 0, "G0 ran the group enumerator")
    else:
        d = kv.get("bulk_g6h_decisions", 0)
        req(d > 0, "no organization decision recorded")
        req(kv.get("bulk_g6h_inv_viol", 1) == 0, "visibility invariant violated")
        req(kv.get("bulk_g6h_stale_sum", 1) == 0,
            "scan skipped released entries (heap retained them)")
        req(kv.get("bulk_g6h_exam_sum", 0) == kv.get("bulk_g6h_elig_sum", -1),
            "entries examined != eligible entries")
        req(kv.get("bulk_g6h_exam_max", 0) <= CAP, "examined more than capacity")
        req(kv.get("bulk_g6h_elig_max", 0) <= kv.get("bulk_g6h_exam_max", 0),
            "eligible above examined")
        req(kv.get("bulk_g6h_groups_max", 0) <= CAP, "more cells than entries")
        req(kv.get("bulk_g6h_untr_max", 0) < kv.get("bulk_win_window", 1 << 30),
            "untranslated count outside the window")

    status = "PASS" if not fails else "FAIL"
    print("%-28s grp=%d %s  cap=%d peak=%d dec=%d exam_max=%d elig_max=%d "
          "groups_max=%d inv=%d" %
          (arm, grp, status, kv.get("bulk_g6b_pa_capacity", 0),
           kv.get("bulk_g6b_ready_peak", 0), kv.get("bulk_g6h_decisions", 0),
           kv.get("bulk_g6h_exam_max", 0), kv.get("bulk_g6h_elig_max", 0),
           kv.get("bulk_g6h_groups_max", 0), kv.get("bulk_g6h_inv_viol", 0)))
    for m in fails:
        print("    - %s" % m)
    return not fails


def main():
    # The frozen build/wm/m23g6g/g6g_G1_*.txt logs predate the G6H counters and
    # are checked by reproduction instead (tools/m23g6h_extract.py compares them
    # key by key against g6h_G1r_*).
    paths = sorted(glob.glob("build/wm/m23g6h/g6h_G0r_*.txt") +
                   glob.glob("build/wm/m23g6h/g6h_G1r_*.txt") +
                   glob.glob("build/wm/m23g6h/g6h_P2F_*.txt"))
    if not paths:
        print("no cells found", file=sys.stderr)
        return 1
    ok = True
    for p in paths:
        ok &= check(p)
    print("%d cells, %s" % (len(paths), "ALL PASS" if ok else "FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
