#!/usr/bin/env python3
"""M18: one-line-per-cell metric sheet, parsed from the raw logs."""
import glob
import os
import re
import sys

KV = re.compile(r"^\s+m1[56] ([A-Za-z0-9_]+)=(-?\d+)$")
ROW = re.compile(r"^  (\S.*?)\s+(\d+)\s+(\d+)\s+(\d+)\s*$")


def load(p):
    d = {}
    for line in open(p):
        s = line.rstrip()
        m = KV.match(s)
        if m:
            d[m.group(1)] = int(m.group(2))
            continue
        m = ROW.match(s)
        if m and not re.search(r"\d", m.group(1).split()[-1]):
            k = m.group(1).strip().replace(" ", "_")
            d["A_" + k] = int(m.group(2))
            d["C_" + k] = int(m.group(4))
            continue
        m = re.search(r"first index request (\d+), descriptor fetch done (\d+), "
                      r"first admit (\d+), first target issue (\d+)", s)
        if m:
            d["first_idx"] = int(m.group(1))
            d["desc_done"] = int(m.group(2))
            d["first_tgt"] = int(m.group(4))
    return d


def main():
    pat = sys.argv[1] if len(sys.argv) > 1 else "build/wm/m18_*_w8192.txt"
    print("%-34s %9s %9s %7s %9s %9s %7s %7s %7s %7s %7s %6s %6s" % (
        "cell", "A cycles", "C cycles", "S", "A DRAM", "C DRAM", "A rowhit", "C rowhit",
        "A dTLBm", "C dTLBm", "A walks", "C walks", "C dedup"))
    for f in sorted(glob.glob(pat)):
        d = load(f)
        a, c = d.get("A_cycles"), d.get("C_cycles")
        if not a or not c:
            continue

        def rh(arm):
            h = d.get(arm + "_DRAM_row_hits", 0)
            k = d.get(arm + "_DRAM_row_conflicts", 0)
            return 100.0 * h / (h + k) if h + k else 0.0

        print("%-34s %9d %9d %7s %9d %9d %6.1f%% %6.1f%% %7d %7d %7d %7d %6d" % (
            os.path.basename(f)[4:-4], a, c, "%.3fx" % (a / c),
            d.get("A_DRAM_reads", 0), d.get("C_DRAM_reads", 0), rh("A"), rh("C"),
            d.get("A_tlb0_misses", 0), d.get("C_tlb0_misses", 0),
            d.get("A_walks", 0), d.get("C_walks", 0),
            d.get("bulk_dedup_merged", 0)))


if __name__ == "__main__":
    main()
