#!/usr/bin/env python3
"""M28-A: build the five dispatch-only validation traces (M18TRC1 format).

Dispatch-only experiment: these traces are never executed. They are read by
sim/bin/m28a_dispatch, which loads them, builds the ClassMap exactly as the
simulator does (build_op_classes) and prints the decision per instruction.

Every instruction carries the class byte the trace itself declares.  The
declared class is the *expected* selection for that instruction; the harness
also prints what the address-only rule (op_class=pattern) recovers, so the two
can be compared without touching the dispatcher.
"""
import hashlib
import json
import os
import random
import struct
import sys

LANES = 32
ELEM = 8
LINE = 64
SIMT, BULK = 0, 1
MAGIC = b"M18TRC1\x00"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "wm", "m28a")


class Trace:
    def __init__(self):
        self.cls = []
        self.acc = []
        self.kind = []
        self.region = []

    def add(self, addrs, cls, kind, region):
        i = len(self.cls)
        assert len(addrs) == LANES, (i, len(addrs))
        self.cls.append(cls)
        self.kind.append(kind)
        self.region.append(region)
        for lane, a in enumerate(addrs):
            assert a % ELEM == 0
            self.acc.append((a, i, lane))


def contiguous(tr, base, n, region):
    """n contiguous instructions of LANES*ELEM bytes each."""
    span = LANES * ELEM
    for k in range(n):
        lo = base + k * span
        tr.add([lo + l * ELEM for l in range(LANES)], SIMT, "contiguous", region)


def small_gather(tr, rng, base, n, words, region):
    """Irregular gather over a small footprint (words*ELEM bytes), repeated."""
    pool = [base + w * ELEM for w in range(words)]
    for _ in range(n):
        tr.add(rng.sample(pool, LANES), SIMT, "small_irregular_gather", region)


def large_gather(tr, rng, base, n, span, region):
    for _ in range(n):
        tr.add([base + rng.randrange(span // ELEM) * ELEM for _ in range(LANES)],
               BULK, "large_random_gather", region)


def page_clustered(tr, rng, base, n, region):
    """One 4 KiB page per instruction; 16 distinct lines, 2 elements each."""
    lines = list(range(4096 // LINE))                 # 64 lines per 4 KiB page
    for k in range(n):
        pg = base + k * 4096
        slots = rng.sample(lines, LANES // 2)          # 16 distinct lines
        addrs = []
        for s in slots:
            addrs.append(pg + s * LINE)                # two elements inside the line
            addrs.append(pg + s * LINE + ELEM)
        tr.add(addrs, BULK, "page_clustered_gather", region)


def write(tr, path, regions, meta):
    buf = bytearray()
    buf += MAGIC
    buf += struct.pack("<IIQQ", LANES, ELEM, len(tr.cls), len(tr.acc))
    buf += bytes(tr.cls)
    for addr, instr, lane in tr.acc:
        buf += struct.pack("<QII", addr, instr, lane)
    with open(path, "wb") as f:
        f.write(bytes(buf))

    lines, pages, words = set(), set(), set()
    h = hashlib.sha256()
    for addr, instr, lane in tr.acc:
        lines.add(addr // LINE)
        pages.add(addr // 4096)
        words.add(addr)
        h.update(struct.pack("<QII", addr, instr, lane))
    meta.update({
        "path": os.path.relpath(path),
        "lanes": LANES,
        "elem_bytes": ELEM,
        "total_instructions": len(tr.cls),
        "total_accesses": len(tr.acc),
        "declared_simt_instr": sum(1 for c in tr.cls if c == SIMT),
        "declared_bulk_instr": sum(1 for c in tr.cls if c == BULK),
        "unique_lines": len(lines),
        "unique_pages": len(pages),
        "unique_words": len(words),
        "duplicate_word_frac": 1.0 - len(words) / len(tr.acc),
        "regions": regions,
        "per_instruction": [
            {"instr": i, "declared_class": ("simt" if tr.cls[i] == SIMT else "bulk"),
             "pattern_kind": tr.kind[i], "region": tr.region[i]}
            for i in range(len(tr.cls))
        ],
        "trace_sha256": h.hexdigest(),
    })
    with open(path + ".json", "w") as f:
        json.dump(meta, f, indent=2, sort_keys=True)
    return meta


def main():
    os.makedirs(OUT, exist_ok=True)
    rng = random.Random(20280201)
    metas = {}

    # W1 — sequential contiguous
    t = Trace()
    contiguous(t, 0x00100000, 64, 0)
    metas["w1"] = write(t, os.path.join(OUT, "w1_contiguous.trc"), [
        {"region": 0, "instr": "0-63", "kind": "contiguous 256 B per instruction",
         "expected": "SIMT"}], {"name": "W1 sequential contiguous", "expected": "SIMT"})

    # W2 — small working-set irregular gather (512 words = 4 KiB footprint)
    t = Trace()
    small_gather(t, rng, 0x00200000, 64, 512, 0)
    metas["w2"] = write(t, os.path.join(OUT, "w2_small_gather.trc"), [
        {"region": 0, "instr": "0-63", "kind": "32 distinct words of a 512-word (4 KiB) pool, shuffled",
         "expected": "SIMT"}], {"name": "W2 small working-set irregular gather", "expected": "SIMT"})

    # W3 — large random irregular gather over 8 MiB
    t = Trace()
    large_gather(t, rng, 0x00300000, 64, 8 << 20, 0)
    metas["w3"] = write(t, os.path.join(OUT, "w3_large_gather.trc"), [
        {"region": 0, "instr": "0-63", "kind": "32 uniform random words over 8 MiB",
         "expected": "Bulk"}], {"name": "W3 large random irregular gather", "expected": "Bulk"})

    # W4 — page-clustered irregular gather
    t = Trace()
    page_clustered(t, rng, 0x00400000, 64, 0)
    metas["w4"] = write(t, os.path.join(OUT, "w4_page_clustered.trc"), [
        {"region": 0, "instr": "0-63", "kind": "one 4 KiB page per instruction, 16 lines, irregular slots",
         "expected": "Bulk"}], {"name": "W4 page-clustered irregular gather", "expected": "Bulk"})

    # W5 — mixed: three distinct instruction regions
    t = Trace()
    contiguous(t, 0x00500000, 24, 0)
    large_gather(t, rng, 0x00600000, 20, 8 << 20, 1)
    page_clustered(t, rng, 0x00700000, 20, 2)
    metas["w5"] = write(t, os.path.join(OUT, "w5_mixed.trc"), [
        {"region": 0, "instr": "0-23", "kind": "contiguous 256 B", "expected": "SIMT"},
        {"region": 1, "instr": "24-43", "kind": "large random over 8 MiB", "expected": "Bulk"},
        {"region": 2, "instr": "44-63", "kind": "page-clustered 4 KiB", "expected": "Bulk"}],
        {"name": "W5 mixed (contiguous + large random + page-clustered)", "expected": "both"})

    for k, m in metas.items():
        print(f"{k} {m['path']} instr={m['total_instructions']} acc={m['total_accesses']} "
              f"declared_simt={m['declared_simt_instr']} declared_bulk={m['declared_bulk_instr']} "
              f"lines={m['unique_lines']} pages={m['unique_pages']} sha={m['trace_sha256'][:16]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
