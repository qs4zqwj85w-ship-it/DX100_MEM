#!/usr/bin/env python3
"""M23-G1 — generate isolated request-generation traces (M18TRC1 format).

Writes the exact binary layout the existing M18 file-trace loader reads
(sim/src/workload.cpp, build_trace_file):

    char  magic[8]  "M18TRC1\\0"
    u32   lanes
    u32   elem_bytes
    u64   n_instr
    u64   n_acc
    u8    cls[n_instr]                      0 = SIMT, 1 = Bulk
    struct { u64 addr; u32 instr; u32 lane; } acc[n_acc]

Nothing existing is read or rewritten: every file this script produces is new,
under build/wm/m23g1/traces/. Historical M18 traces are untouched.

Patterns (each instruction issues ``lanes`` accesses, one per lane):

  A  contiguous   lane l -> base + i*lanes + l
                  ``lanes`` consecutive 64 B lines per instruction: maximum
                  coalescing-free independence inside one 1 KiB bank row.
  B  bank-spread  lane l -> base + (i*lanes + l) * 33
                  odd stride: every consecutive access flips bank and advances
                  the row, so both banks are exercised with conflict-heavy rows.
  C  random       uniform distinct lines over a 4 M-line (256 MiB) space:
                  no row locality, footprint far larger than L2 (1 MiB).
  E  streams      lane l -> base + l*sep + i, sep = instrs*2
                  one independent sequential stream per lane, disjoint regions.
  F  hot          lane l -> (i*lanes + l) mod 64
                  a 64-line window that is L1-resident after warm-up. This is the
                  generation-only control: the memory system answers every request
                  from the L1, so the measured rate is the front-end's own
                  consumption and request-formation rate, with no DRAM in the way.
                  It is a control, not one of the five requested patterns.

Usage:
  python3 tools/m23g1_mktrace.py --pattern A --lanes 4 --instrs 50000 \
      --out build/wm/m23g1/traces/g1_A_l4.trc
"""
import argparse
import os
import random
import struct
import sys

ELEM = 4
LINE = 64
BASE_LINE = 1 << 20          # VA line offset, so pattern math stays positive
RANDOM_SPACE = 4_000_000     # distinct lines for pattern C (256 MiB)


def build(pattern, lanes, n_instr):
    acc = []                 # (addr, instr, lane)
    if pattern == "A":
        for i in range(n_instr):
            for l in range(lanes):
                acc.append(((BASE_LINE + i * lanes + l) * LINE + l * ELEM, i, l))
    elif pattern == "B":
        for i in range(n_instr):
            for l in range(lanes):
                k = i * lanes + l
                acc.append(((BASE_LINE + k * 33) * LINE + l * ELEM, i, l))
    elif pattern == "C":
        rng = random.Random(12345 + lanes)
        seen = set()
        for i in range(n_instr):
            for l in range(lanes):
                while True:
                    line = rng.randrange(RANDOM_SPACE)
                    if line not in seen:
                        seen.add(line)
                        break
                acc.append(((BASE_LINE + line) * LINE + l * ELEM, i, l))
    elif pattern == "E":
        sep = n_instr * 2
        for i in range(n_instr):
            for l in range(lanes):
                acc.append(((BASE_LINE + l * sep + i) * LINE + l * ELEM, i, l))
    elif pattern == "F":
        hot = 64
        for i in range(n_instr):
            for l in range(lanes):
                acc.append(((BASE_LINE + (i * lanes + l) % hot) * LINE + l * ELEM, i, l))
    else:
        raise SystemExit(f"unknown pattern {pattern}")
    return acc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pattern", required=True, choices=list("ABCEF"))
    ap.add_argument("--lanes", type=int, default=4)
    ap.add_argument("--instrs", type=int, default=50000)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    acc = build(args.pattern, args.lanes, args.instrs)
    n_instr = args.instrs
    lines = {a[0] // LINE for a in acc}
    pages = {a[0] // 4096 for a in acc}

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(b"M18TRC1\0")
        f.write(struct.pack("<IIQQ", args.lanes, ELEM, n_instr, len(acc)))
        f.write(bytes(n_instr))                       # all class 0 (SIMT)
        for addr, instr, lane in acc:
            f.write(struct.pack("<QII", addr, instr, lane))

    print(f"{args.out}: pattern={args.pattern} lanes={args.lanes} "
          f"instrs={n_instr} accesses={len(acc)} lines={len(lines)} pages={len(pages)} "
          f"footprint_MiB={len(lines) * 64 / 2**20:.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
