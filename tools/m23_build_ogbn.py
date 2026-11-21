#!/usr/bin/env python3
"""M23-D / M23-G5A: neighbour-feature gather trace from OGB ogbn-products.

Dataset
-------
`ogbn-products` (OGB node-property-prediction benchmark): 2 449 029 nodes,
61 859 140 directed edges, 100 float32 features per node, 47 classes, time-based
split. Files read from the dataset's own archive:
    products/edge.csv.gz           (src,dst) edge list
    products/node-feat.csv.gz      (num_nodes x 100 float32)
    products/split/.../train.csv.gz

Measured kernel
---------------
One level of indirection, what the engine supports:

    for each target node t:  agg[t] = sum over u in N(t) of feat[u][0:dims]

The minibatch selection and the adjacency resolution happen here, offline, and
are **not** in the trace (documented in the report as excluded preprocessing).

Feature width (M23-G5A)
-----------------------
The engine's element is one 4 B value and its unit of work is a cache line, so a
gather of `dims` float32 features is modelled as one access per **64 B line the
feature row actually occupies**:

    dims = 16  -> row_bytes =  64, lines = 1     (M23-D, the original trace)
    dims = 100 -> row_bytes = 400, lines = 7     (M23-G5A, the full vector)

The row is packed exactly as the dataset stores it (`dims * 4` bytes, no padding),
so at dims = 100 a node's 400 B row is line-aligned for only one node in sixteen
and adjacent nodes share boundary lines — as they do in the real `[N, 100]`
tensor. The addresses emitted are `prop_base + u * row_bytes + k * 64`, which is
what the loads touch; nothing is multiplied analytically after the fact.

`--dims 16` regenerates the M23-D trace bit for bit (same accesses, same order,
same cap semantics), which is how the original experiment is preserved.

Emission
--------
Per target node, its neighbours are gathered in `lanes`-sized instruction chunks.
For each chunk and each of the `lines` line offsets, one instruction gathers that
offset's line for the chunk's lanes. An instruction is annotated BULK when the
target's degree is >= `--bulk-degree`; smaller neighbourhoods stay SIMT.

`--max-accesses` bounds the number of neighbour slots admitted, exactly as in
M23-D (where one neighbour was one access). At dims = 100 the same cap therefore
admits the same neighbours and 7x the accesses, which is what makes the 16- and
100-dimensional runs the *same logical workload* over different feature widths.

Usage
  m23_build_ogbn.py --graph g.csv.gz --feat f.csv.gz --split train.csv.gz \
      --targets 65536 --max-accesses 1200000 --dims 100 --out t.bin
"""
import argparse
import gzip
import hashlib
import json
import struct
import sys

import numpy as np

MAGIC = b"M18TRC1\x00"
SIMT, BULK = 0, 1
LINE = 64


def align(v, a=64):
    return (v + a - 1) // a * a


def read_csv_int(path):
    """Comma- or newline-separated integers, parsed in C (numpy's text scanner)."""
    with gzip.open(path, "rb") as f:
        raw = f.read()
    txt = raw.decode("ascii").replace("\r", "").replace("\n", ",").rstrip(",")
    import warnings
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        a = np.fromstring(txt, dtype=np.int64, sep=",")
    if a.size == 0:
        raise SystemExit(f"{path}: no integers parsed")
    return a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--graph", required=True)
    ap.add_argument("--feat", required=True)
    ap.add_argument("--split", default="")
    ap.add_argument("--num-nodes", type=int, default=2449029)
    ap.add_argument("--targets", type=int, default=65536)
    ap.add_argument("--max-accesses", type=int, default=1200000)
    ap.add_argument("--bulk-degree", type=int, default=8)
    ap.add_argument("--lanes", type=int, default=4)
    ap.add_argument("--seed", type=int, default=1)
    # M23-G5A: feature width. 16 keeps the M23-D trace bit-identical.
    ap.add_argument("--dims", type=int, default=16)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.lanes != 4:
        sys.exit("M23 fixes lanes = 4")
    if a.dims <= 0:
        sys.exit("--dims must be positive")

    dims = a.dims
    row_bytes = dims * 4                     # packed, as the dataset stores it
    lines = (row_bytes + LINE - 1) // LINE   # 64 B lines one row occupies

    # ---- 1. edges ----------------------------------------------------------
    e = read_csv_int(a.graph)
    src, dst = e[0::2], e[1::2]
    E = src.size
    n = a.num_nodes
    deg_out = np.bincount(src, minlength=n)

    # ---- 2. target selection ----------------------------------------------
    pool = np.arange(n)
    split_note = "uniform over all nodes"
    if a.split:
        pool = np.unique(read_csv_int(a.split))
        split_note = "uniform over the archive's sales_ranking TRAIN split"
    rng = np.random.default_rng(a.seed)
    T = min(a.targets, pool.size)
    targets = rng.choice(pool, size=T, replace=False)

    # ---- 3. resolve adjacency for the selected targets only (preprocessing)
    m = np.isin(src, targets)
    s_sel, d_sel = src[m], dst[m]
    order = np.argsort(s_sel, kind="stable")
    s_sel, d_sel = s_sel[order], d_sel[order]
    indptr = np.zeros(n + 1, dtype=np.int64)
    np.add.at(indptr, s_sel + 1, 1)
    indptr = np.cumsum(indptr)
    del m, order

    # ---- 4. features for the touched nodes ---------------------------------
    # Only the touched rows are kept: the full matrix is ~1 GB at dims = 100 and
    # nothing outside `used` is ever read. Values are exactly the file's.
    used = np.unique(d_sel)
    want = np.zeros(n, dtype=bool)
    want[used] = True
    feat_rows = {}
    seen = np.zeros(n, dtype=bool)
    with gzip.open(a.feat, "rt") as f:
        for i, line in enumerate(f):
            if i >= n:
                break
            if want[i]:
                feat_rows[i] = np.fromstring(line, dtype=np.float32, sep=",")[:dims]
                seen[i] = True
    missing = int(used.size - seen[used].sum())
    if missing:
        sys.exit(f"{missing} touched nodes have no feature row")
    del want, seen

    # ---- 5. trace ----------------------------------------------------------
    prop_base = 0
    cls, acc = [], []
    instr = 0
    nsel = 0
    access = 0          # neighbour slots admitted (the cap's unit, as in M23-D)
    degs = []
    agg = {}
    stop = False
    for t in targets:
        b, e2 = indptr[t], indptr[t + 1]
        d = int(e2 - b)
        if d == 0:
            continue
        nsel += 1
        degs.append(d)
        k = BULK if d >= a.bulk_degree else SIMT
        for j in range(b, e2, a.lanes):
            hi = min(j + a.lanes, e2)
            for kk in range(lines):
                cls.append(k)
                for lane, jj in enumerate(range(j, hi)):
                    u = int(d_sel[jj])
                    acc.append((prop_base + u * row_bytes + kk * LINE, instr, lane))
                instr += 1
            access += hi - j
            if access >= a.max_accesses:
                stop = True
                break
        # the aggregation the kernel would perform, from the real features
        rows = [feat_rows[int(u)] for u in d_sel[b:e2]]
        agg[int(t)] = np.sum(rows, axis=0, dtype=np.float64)
        if stop:
            break

    cls_b = sum(1 for c in cls if c == BULK)
    with open(a.out, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<IIQQ", a.lanes, 4, len(cls), len(acc)))
        f.write(bytes(cls))
        buf = bytearray()
        for ad, ins, lane in acc:
            buf += struct.pack("<QII", ad, ins, lane)
        f.write(bytes(buf))

    lines_touched = {ad // LINE for ad, _, _ in acc}
    pages = {ad // (LINE * 64) for ad, _, _ in acc}
    h = hashlib.sha256()
    for ad, ins, lane in acc:
        h.update(struct.pack("<QII", ad, ins, lane))
    ah = hashlib.sha256()
    for t in sorted(agg):
        ah.update(struct.pack("<Q", t))
        ah.update(agg[t].astype(np.float32).tobytes())
    with open(a.graph, "rb") as fh:
        gsha = hashlib.sha256(fh.read()).hexdigest()

    degs = np.array(degs) if degs else np.array([0])
    meta = {
        "source": {"dataset": "OGB ogbn-products",
                   "graph_sha256": gsha,
                   "nodes": n, "edges_directed": int(E),
                   "features_per_node": 100, "feature_dtype": "float32",
                   "split_file": a.split},
        "sampling": {"policy": split_note, "seed": a.seed,
                     "targets_requested": int(T), "targets_used": int(nsel),
                     "max_accesses_cap": a.max_accesses},
        "feature_layout": {"dims_gathered": dims, "row_bytes": row_bytes,
                           "lines_per_row": lines, "packed": True,
                           "rows": n, "region_bytes": n * row_bytes,
                           "note": ("first 16 of 100 float32 features, one line per node"
                                    if dims == 16 else
                                    f"all {dims} float32 features, {lines} lines per node, "
                                    "rows packed at dims*4 bytes with no padding")},
        "kernel": {"accesses": len(acc), "instructions": len(cls),
                   "instructions_bulk": int(cls_b),
                   "instructions_simt": int(len(cls) - cls_b),
                   "bulk_degree_threshold": a.bulk_degree},
        "degree": {"mean": float(deg_out.mean()), "max": int(deg_out.max()),
                   "selected_mean": float(degs.mean()), "selected_max": int(degs.max()),
                   "selected_median": int(np.median(degs))},
        "layout": {"prop_base": prop_base, "index_base": 0x80000000,
                   "desc_line": 0x7FFFF000 // 64, "elem_bytes": 4, "data_base": 0},
        "totals": {"unique_lines": len(lines_touched), "unique_pages": len(pages),
                   "working_set_bytes": len(lines_touched) * LINE,
                   "duplicate_line_frac": round(1 - len(lines_touched) / len(acc), 6)},
        "aggregation_sha256": ah.hexdigest(),
        "excluded_from_measurement": [
            "target minibatch sampling",
            "adjacency resolution (CSR rows for the sampled targets)",
            "feature-vector aggregation arithmetic",
        ],
        "trace_sha256": h.hexdigest(),
    }
    with open(a.out + ".json", "w") as f:
        json.dump(meta, f, indent=2, sort_keys=True)

    print(f"targets={nsel}/{T} acc={len(acc)} instr={len(cls)} bulk={cls_b} simt={len(cls)-cls_b}")
    print(f"lines={len(lines_touched)} pages={len(pages)} "
          f"ws={len(lines_touched)*LINE/(1<<20):.2f} MiB "
          f"dup={1-len(lines_touched)/len(acc):.3f}")
    print(f"deg graph mean={deg_out.mean():.1f} max={deg_out.max()} | selected "
          f"mean={degs.mean():.1f} median={np.median(degs):.0f} max={degs.max()}")
    print(f"aggregation sha={meta['aggregation_sha256'][:16]} trace sha={meta['trace_sha256'][:16]}")


if __name__ == "__main__":
    main()
