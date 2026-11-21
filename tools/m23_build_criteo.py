#!/usr/bin/env python3
"""M23-B: build an embedding-lookup trace from real Criteo categorical IDs.

Source
------
Criteo 1TB Click Logs (the Criteo Terabyte dataset), the public mirror
`huggingface.co/datasets/criteo/CriteoClickLogs`, one Parquet shard of one day.
The records are the dataset's own; nothing is resampled, permuted or replaced
with a synthetic distribution. We use a *prefix* of one shard: the file and its
SHA-256 are recorded in the JSON so the exact input is identifiable.

Workload
--------
A recommendation model looks up one 16-wide float32 embedding vector (64 B, one
cache line) per categorical field per record. The 26 categorical fields are 26
separate tables, as the model specification requires.

    slot_f   = id_f & (V_f - 1)                     (the hashing trick)
    V_f      = clamp(2^ceil(log2(8 * D_f)), 16, 2^20)  (table size from the
                                                        field's own cardinality)
    addr     = table_base[f] + slot_f * 64

Instruction shape: one warp (lanes = 4) looks up one field for four consecutive
records -- four independent lines inside one table. This keeps field identity at
instruction granularity, which is what makes the compiler's per-operation
decision expressible and the per-field analysis possible.

Classification (the "compiler" annotation)
-----------------------------------------
An operation is annotated SIMT when its table fits in L1 (V_f * 64 <= 16 KiB):
those fields have a handful of distinct IDs, the table is L1-resident after
warm-up, and Bulk can only add descriptor and fanout overhead. Everything else
is annotated Bulk: independent, 64 MiB-scale tables, no spatial locality.

Also emitted: one dense instruction per warp group reading the 13 integer
features of its four records (contiguous, coalesced) -- annotated SIMT, because
it is the regular access a compiler must not send to the accelerator.

Usage
  m23_build_criteo.py --parquet P --records N --out T.bin
"""
import argparse
import collections
import hashlib
import json
import math
import struct
import sys

MAGIC = b"M18TRC1\x00"
SIMT, BULK = 0, 1
LINE = 64
DIM_BYTES = 64          # 16 x float32 embedding row = one cache line
L1_BYTES = 65536        # V_f * 64 <= L1 => the table is L1-resident
VA_LIMIT = 1 << 32

TABLE_REGION = 0x0100_0000      # 16 MiB: tables start here
DENSE_BASE = 0x0000_0000        # 4 MiB of dense integer features at 0
INDEX_BASE = 0x8000_0000        # 2 GiB: clear of the tables
DESC_BASE = 0x7FFF_F000         # just below the index array


def next_pow2(n):
    return 1 << max(0, (n - 1).bit_length())


def vault(s):
    """Categorical value -> integer ID. Empty (missing) maps to 0."""
    if not s:
        return None
    return int(s, 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parquet", required=True)
    ap.add_argument("--records", type=int, default=16384)
    ap.add_argument("--lanes", type=int, default=4)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.lanes != 4:
        sys.exit("M23 fixes lanes = 4 (the established experimental width)")

    import pyarrow.parquet as pq
    t = pq.read_table(a.parquet)
    if t.num_rows < a.records:
        sys.exit(f"parquet has {t.num_rows} rows, need {a.records}")
    t = t.slice(0, a.records)

    cats = [f"categorical_feature_{i}" for i in range(1, 27)]
    ints = [f"integer_feature_{i}" for i in range(1, 14)]

    # ---- 1. per-field cardinality, table size and class --------------------
    ids = []
    fields = []
    for f, c in enumerate(cats):
        raw = t.column(c).to_pylist()
        col = [vault(v) for v in raw]
        ids.append(col)
        distinct = len({v for v in col if v is not None})
        missing = sum(1 for v in col if v is None)
        V = max(16, min(1 << 20, next_pow2(max(1, 8 * distinct))))
        cls = SIMT if V * DIM_BYTES <= L1_BYTES else BULK
        cnt = collections.Counter(v for v in col if v is not None)
        top = cnt.most_common(1)[0] if cnt else (None, 0)
        fields.append({
            "field": f + 1,
            "name": c,
            "distinct_ids": distinct,
            "missing": missing,
            "table_rows": V,
            "table_bytes": V * DIM_BYTES,
            "class": "simt" if cls == SIMT else "bulk",
            "top1_id_share": round(top[1] / a.records, 6),
            "reuse_frac": round(1.0 - distinct / a.records, 6),
        })

    # ---- 2. lay the tables out, one contiguous region per field -------------
    base = TABLE_REGION
    for f in fields:
        f["base"] = base
        # 2 MiB allocation granularity: separate tables never share a page, and
        # the spans stay the sum of the real table sizes rather than a padded
        # stride that would fake a larger footprint.
        base = (base + f["table_bytes"] + (1 << 21) - 1) // (1 << 21) * (1 << 21)
    table_end = base
    if table_end >= INDEX_BASE:
        sys.exit(f"tables end at {table_end:#x}, over the index base {INDEX_BASE:#x}")

    # ---- 3. instruction stream ---------------------------------------------
    cls = []
    acc = []           # (addr, instr, lane)
    per_field_counts = [0] * 26
    bulk_elems = 0
    simt_elems = 0
    dense_bytes = a.records * 13 * 4
    if DENSE_BASE + dense_bytes > TABLE_REGION:
        sys.exit("dense block would overlap the table region")

    ngroups = a.records // a.lanes
    instr = 0
    for g in range(ngroups):
        r0 = g * a.lanes
        # 26 embedding lookups, one instruction per field
        for f in range(26):
            cls.append(fields[f]["class"] == "bulk")
            for lane in range(a.lanes):
                rid = ids[f][r0 + lane]
                slot = 0 if rid is None else (rid & (fields[f]["table_rows"] - 1))
                acc.append((fields[f]["base"] + slot * DIM_BYTES, instr, lane))
                per_field_counts[f] += 1
                if fields[f]["class"] == "bulk":
                    bulk_elems += 1
                else:
                    simt_elems += 1
            instr += 1
        # one dense instruction: 13 integer features x 4 records
        cls.append(False)
        for lane in range(a.lanes):
            for k in range(13):
                addr = DENSE_BASE + ((r0 + lane) * 13 + k) * 4
                acc.append((addr, instr, lane))
                simt_elems += 1
        instr += 1

    # ---- 4. write the trace in the M18TRC1 layout --------------------------
    with open(a.out, "wb") as fh:
        fh.write(MAGIC)
        fh.write(struct.pack("<IIQQ", a.lanes, 4, len(cls), len(acc)))
        fh.write(bytes(1 if c else 0 for c in cls))
        buf = bytearray()
        for addr, ins, lane in acc:
            buf += struct.pack("<QII", addr, ins, lane)
        fh.write(bytes(buf))

    lines = {ad // LINE for ad, _, _ in acc}
    pages = {ad // (LINE * 64) for ad, _, _ in acc}
    h = hashlib.sha256()
    for ad, ins, lane in acc:
        h.update(struct.pack("<QII", ad, ins, lane))

    with open(a.parquet, "rb") as fh:
        src_sha = hashlib.sha256(fh.read()).hexdigest()

    meta = {
        "source": {
            "dataset": "Criteo 1TB Click Logs (Criteo Terabyte)",
            "mirror": "huggingface.co/datasets/criteo/CriteoClickLogs",
            "file": a.parquet,
            "file_sha256": src_sha,
            "file_rows": t.num_rows,
            "rows_used": a.records,
            "subset": "prefix of one shard of one day, in file order",
        },
        "embedding": {"dim": 16, "dtype": "float32", "row_bytes": DIM_BYTES,
                      "one_row_per_cache_line": True},
        "layout": {
            "dense_base": DENSE_BASE, "dense_bytes": dense_bytes,
            "table_region_start": TABLE_REGION, "table_region_end": table_end,
            "table_span_bytes": table_end - TABLE_REGION,
            "index_base": INDEX_BASE, "desc_line": DESC_BASE // LINE,
            "data_base": 0, "elem_bytes": 4,
            "index_bytes": 4,
        },
        "fields": fields,
        "totals": {
            "records": a.records,
            "groups": ngroups,
            "instructions": len(cls),
            "instructions_bulk": sum(1 for c in cls if c),
            "instructions_simt": sum(1 for c in cls if not c),
            "accesses": len(acc),
            "bulk_elems": bulk_elems,
            "simt_elems": simt_elems,
            "distinct_lines": len(lines),
            "distinct_pages": len(pages),
            "working_set_bytes": len(lines) * LINE,
            "duplicate_line_frac": round(1.0 - len(lines) / len(acc), 6),
            "per_field_accesses": per_field_counts,
        },
        "trace_sha256": h.hexdigest(),
    }
    with open(a.out + ".json", "w") as fh:
        json.dump(meta, fh, indent=2, sort_keys=True)

    print(f"records={a.records} instr={len(cls)} acc={len(acc)} "
          f"bulk={bulk_elems} simt={simt_elems}")
    print(f"lines={len(lines)} pages={len(pages)} "
          f"ws={len(lines) * LINE / (1 << 20):.2f} MiB "
          f"tables={((table_end - TABLE_REGION) / (1 << 20)):.1f} MiB")
    for f in fields:
        print(f"  f{f['field']:>2} D={f['distinct_ids']:>6} V={f['table_rows']:>7} "
              f"{f['table_bytes']/1024:>8.0f} KiB {f['class']:>4} "
              f"top1={f['top1_id_share']:.3f} reuse={f['reuse_frac']:.3f}")


if __name__ == "__main__":
    main()
