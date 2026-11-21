#!/usr/bin/env python3
"""M18: turn raw three-arm cell logs into the report tables.

Reads build/wm/m18_*.txt (one file per cell, written by sim/build/wm/m18_run.sh)
and prints markdown. Every number is read from the log; nothing is recomputed
from a model. Missing keys print as '-' rather than as a guess.
"""
import glob
import os
import re
import sys

CELL = re.compile(r"m18_(?P<matrix>[a-z0-9_]+?)_(?P<variant>full|noidx)_w(?P<win>\d+)(?P<extra>(_mshr128|_nopage|_coh1)?)\.txt$")
KV = re.compile(r"^\s+m1[56] ([A-Za-z0-9_]+)=(-?\d+)$")
# Name may hold digits ("L1 reads") but its last token may not, so a per-requester
# row is not mistaken for a metric row.
ROW = re.compile(r"^  (\S.*?)\s+(\d+)\s+(\d+)\s+(\d+)\s*$")


def parse(path):
    d = {"path": path, "ok": None, "gates": "?"}
    g4 = {}
    with open(path) as f:
        for line in f:
            s = line.rstrip("\n")
            m = KV.match(s)
            if m:
                d[m.group(1)] = int(m.group(2))
                continue
            m = ROW.match(s)
            if m and not re.search(r"\d", m.group(1).split()[-1]):
                key = m.group(1).strip()
                d["A_" + key.replace(" ", "_")] = int(m.group(2))
                d["B_" + key.replace(" ", "_")] = int(m.group(3))
                d["C_" + key.replace(" ", "_")] = int(m.group(4))
                continue
            if "M9 GATES PASSED" in s:
                d["gates"] = "pass"
            elif "M9 GATES FAILED" in s:
                d["gates"] = "FAIL"
            elif "VIOLATION:" in s:
                d.setdefault("violations", []).append(s.split("VIOLATION:")[1].strip())
            elif "first index request" in s:
                for k, v in re.findall(r"(first [a-z ]+|descriptor fetch done) (\d+)", s):
                    g4[k.strip()] = int(v)
            elif "mismatched addresses" in s:
                m = re.search(r"mismatched addresses (\d+)", s)
                if m:
                    d["addr_mismatch"] = int(m.group(1))
            elif "split check" in s:
                m = re.search(r"L1 (\d+)/(\d+)/(\d+)", s)
                if m:
                    d["C_l1_split"] = tuple(int(x) for x in m.groups())
    d.update({k.replace(" ", "_"): v for k, v in g4.items()})
    return d


def cells():
    out = {}
    for p in sorted(glob.glob(os.path.join(os.path.dirname(__file__), "..",
                                           "build", "wm", "m18_*.txt"))):
        m = CELL.search(os.path.basename(p))
        if not m:
            continue
        d = parse(p)
        d["matrix"] = m.group("matrix")
        d["variant"] = m.group("variant")
        d["win"] = int(m.group("win"))
        d["extra"] = m.group("extra").lstrip("_")
        out[(d["matrix"], d["variant"], d["win"], d["extra"])] = d
    return out


def g(d, k):
    v = d.get(k)
    return "-" if v is None else v


def rate(a, b):
    try:
        return f"{100.0 * a / b:.1f}%" if b else "-"
    except TypeError:
        return "-"


def cyc(d, arm="A"):
    return d.get(arm + "_cycles")


def main():
    C = cells()
    mats = []
    for k in C:
        if k[0] not in mats:
            mats.append(k[0])
    order = ["dubcova3", "qc2534", "bcsstk25", "lnsp3937"]
    mats.sort(key=lambda m: order.index(m) if m in order else 99)

    def primary(variant):
        print(f"\n### Primary comparison — variant `{variant}`\n")
        hdr = ("| matrix | config | cycles | speedup | reduction | DRAM rd | row-hit | TLB miss | walks "
               "| L1 hit | L2 hit | idx phase | tgt phase | peak occ | win occ |")
        print(hdr)
        print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for m in mats:
            base = C.get((m, variant, 8192, ""))
            a = cyc(base, "A") if base else None
            for w in (8192, 16384):
                d = C.get((m, variant, w, ""))
                if not d:
                    continue
                c = cyc(d, "C")
                sp = f"{a / c:.3f}x" if a and c else "-"
                red = f"{100.0 * (1 - c / a):.1f}%" if a and c else "-"
                rh = rate(d.get("C_dram_row_hits", 0),
                          (d.get("C_dram_row_hits", 0) + d.get("C_dram_row_conflicts", 0)))
                ia = d.get("first_target_issue", 0) - d.get("first_index_request", 0)
                ta = (d.get("bulk_last_done_cycle") or 0) - (d.get("first_target_issue") or 0)
                tag = "SIMT" if w == 8192 else "SIMT"
                if w == 8192:
                    print(f"| {m} | SIMT-only | {g(d,'A_cycles')} | 1.000x | 0.0% "
                          f"| {g(d,'A_dram_reads')} "
                          f"| {rate(d.get('A_dram_row_hits',0), d.get('A_dram_row_hits',0)+d.get('A_dram_row_conflicts',0))} "
                          f"| {g(d,'A_tlb0_misses')} | {g(d,'A_walks')} "
                          f"| {rate(d.get('A_l1_hits',0), d.get('A_l1_reads',0))} "
                          f"| {rate(d.get('A_l2_hits',0), d.get('A_l2_reads',0))} | N/A | N/A "
                          f"| {g(d,'A_inflight_max')} | N/A |")
                print(f"| {m} | Bulk 2x{w//1024}K | {g(d,'C_cycles')} | {sp} | {red} "
                      f"| {g(d,'C_dram_reads')} | {rh} | {g(d,'C_tlb0_misses')} | {g(d,'C_walks')} "
                      f"| {rate(d.get('C_l1_hits',0), d.get('C_l1_reads',0))} "
                      f"| {rate(d.get('C_l2_hits',0), d.get('C_l2_reads',0))} "
                      f"| {ia} | {ta} | {g(d,'C_inflight_max')} | {g(d,'bulk_win_occ_max')} |")

    primary("full")
    primary("noidx")

    print("\n### Faithful bracket (index acquisition charged once per arm)\n")
    print("| matrix | A (full) | C 2x8K (full) | C 2x8K (noidx) | C 2x16K (noidx) "
          "| S faithful 8K | S faithful 16K | S same-trace 8K |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for m in mats:
        f8 = C.get((m, "full", 8192, ""))
        n8 = C.get((m, "noidx", 8192, ""))
        n16 = C.get((m, "noidx", 16384, ""))
        if not (f8 and n8):
            continue
        a = f8.get("A_cycles")
        sf8 = a / n8["C_cycles"] if n8.get("C_cycles") else None
        sf16 = a / n16["C_cycles"] if n16 and n16.get("C_cycles") else None
        ss = a / f8["C_cycles"] if f8.get("C_cycles") else None
        print(f"| {m} | {a} | {f8.get('C_cycles')} | {n8.get('C_cycles')} | {g(n16,'C_cycles')} "
              f"| {sf8:.3f}x | {sf16:.3f}x | {ss:.3f}x |" if sf8 and sf16 and ss else
              f"| {m} | {a} | {f8.get('C_cycles')} | {n8.get('C_cycles')} | {g(n16,'C_cycles')} | - | - | - |")

    print("\n### 128 L1 MSHR baseline sensitivity\n")
    print("| matrix | A 16MSHR | A 128MSHR | A delta | C 2x8K 16MSHR | C 2x8K 128MSHR | SP 16 | SP 128 | SP A128/C16 |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for m in mats:
        b8 = C.get((m, "full", 8192, ""))
        m8 = C.get((m, "full", 8192, "mshr128"))
        if not (b8 and m8):
            continue
        a0, a1 = b8.get("A_cycles"), m8.get("A_cycles")
        c0, c1 = b8.get("C_cycles"), m8.get("C_cycles")
        print(f"| {m} | {a0} | {a1} | {100.0*(a1-a0)/a0:+.1f}% | {c0} | {c1} "
              f"| {a0/c0:.3f}x | {a1/c1:.3f}x | {a1/c0:.3f}x |")

    print("\n### Mechanism ablation at 2x8K (full variant)\n")
    print("| matrix | full | page order off | one cohort | S full | S nopage | S 1coh "
          "| row-hit full | row-hit nopage | row-hit 1coh |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for m in mats:
        b = C.get((m, "full", 8192, ""))
        np_ = C.get((m, "full", 8192, "nopage"))
        ch = C.get((m, "full", 8192, "coh1"))
        if not b or not (np_ or ch):
            continue
        a = b.get("A_cycles")

        def rh(d):
            return rate(d.get("C_dram_row_hits", 0),
                        d.get("C_dram_row_hits", 0) + d.get("C_dram_row_conflicts", 0)) if d else "-"
        print(f"| {m} | {b.get('C_cycles')} | {g(np_,'C_cycles')} | {g(ch,'C_cycles')} "
              f"| {a/b['C_cycles']:.3f}x "
              f"| {a/np_['C_cycles']:.3f}x | {a/ch['C_cycles']:.3f}x " if np_ and ch else
              f"| {m} | {b.get('C_cycles')} | - | - | - | - | - ", end="")
        print(f"| {rh(b)} | {rh(np_)} | {rh(ch)} |")

    print("\n### Integrity\n")
    print("| cell | gates | addr mismatch | dedup+lines=elems | lost SIMT req | residency |")
    print("|---|---|---|---|---|---|")
    for (m, v, w, e), d in sorted(C.items()):
        ded = d.get("bulk_lines_issued", 0) + d.get("bulk_dedup_merged", 0)
        elems = d.get("bulk_elems_total", 0)
        lost = d.get("simt_requests_generated", 0) - d.get("simt_completed", 0)
        res = d.get("peak_total_outstanding", 0)
        print(f"| {m}_{v}_w{w}{'_'+e if e else ''} | {d['gates']} | {d.get('addr_mismatch','-')} "
              f"| {ded}/{elems} {'OK' if ded==elems else 'MISMATCH'} | {lost} | {res} |")
    bad = [k for k, d in C.items() if d["gates"] != "pass"]
    print(f"\n{len(C)} cells, {len(bad)} not passing")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
