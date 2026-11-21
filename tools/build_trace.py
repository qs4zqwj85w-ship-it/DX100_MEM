#!/usr/bin/env python3
"""M18: build an M18TRC1 access trace from a real application dataset.

The trace is an *execution trace*, not a synthetic pattern: it records the
addresses the application issues, in program order, grouped by the instruction
that issues them. Nothing is reordered, compacted or pre-issued. Both front-ends
in the simulator consume this one trace, so they differ only in which client
performs an instruction, never in which work exists.

Layout (byte-addressed virtual space, one array per region):

    x        [0, m*8)                       the gathered vector (doubles)
    rowptr   aligned, (m+1)*4
    colidx   aligned, >= nnz*8 so the engine's index array cannot overlap x
    value    aligned, nnz*8

x starts at 0 on purpose: with data_base = 0 and elem_bytes = 8 the engine's
index value for element j is x_addr/8 = colidx[j], so the index array the engine
reads is *byte-identical* to the CSR column-index array. The engine therefore
reads the real index structure; nothing is pre-materialized for it.

Usage:
  build_trace.py spmv --mtx D.mtx --out t.bin [--variant full|noidx] [--lanes 4]
  build_trace.py bfs  --edges G.txt.gz --out t.bin [--source 0] [--lanes 4]
Writes <out>.json with the integrity record the report cites.
"""
import argparse
import gzip
import hashlib
import json
import struct
import sys

MAGIC = b"M18TRC1\x00"
SIMT, BULK = 0, 1
LINE = 64


def open_text(path):
    return gzip.open(path, "rt", errors="replace") if path.endswith(".gz") else open(path, "r", errors="replace")


def align(v, a=64):
    return (v + a - 1) // a * a


# --------------------------------------------------------------------------- #
# datasets
# --------------------------------------------------------------------------- #
def read_mtx(path):
    """CSR (indptr, indices), symmetric expanded, 0-based, sorted per row."""
    nrows = ncols = None
    sym = False
    entries = []
    with open_text(path) as f:
        for line in f:
            if line.startswith("%%"):
                low = line.lower()
                if "symmetric" in low or "hermitian" in low:
                    sym = True
                continue
            if line.startswith("%") or not line.strip():
                continue
            p = line.split()
            if nrows is None:
                nrows, ncols = int(p[0]), int(p[1])
                continue
            entries.append((int(p[0]) - 1, int(p[1]) - 1))
    rows = [[] for _ in range(nrows)]
    for i, j in entries:
        rows[i].append(j)
        if sym and i != j:
            rows[j].append(i)
    for r in rows:
        r.sort()
    indptr = [0]
    indices = []
    for r in rows:
        indices.extend(r)
        indptr.append(len(indices))
    return nrows, ncols, indptr, indices, sym, len(entries)


def read_edgelist(path):
    """Undirected CSR from a SNAP 'u v' edge list, duplicate edges merged."""
    adj = {}
    with open_text(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            p = line.split()
            if len(p) < 2:
                continue
            u, v = int(p[0]), int(p[1])
            if u == v:
                continue
            adj.setdefault(u, set()).add(v)
            adj.setdefault(v, set()).add(u)
    n = max(adj) + 1 if adj else 0
    indptr = [0]
    indices = []
    for i in range(n):
        indices.extend(sorted(adj.get(i, ())))
        indptr.append(len(indices))
    return n, indptr, indices


# --------------------------------------------------------------------------- #
# trace writer
# --------------------------------------------------------------------------- #
class Trace:
    def __init__(self, lanes):
        self.lanes = lanes
        self.acc = []          # (addr, instr, lane)
        self.cls = []          # per instruction
        self.by_type = {}
        self.ops = 0

    def instr(self, kind, addrs):
        """Emit one instruction covering `addrs` (already lane-ordered)."""
        i = len(self.cls)
        self.cls.append(BULK if kind.endswith("_gather") else SIMT)
        for lane, a in enumerate(addrs):
            self.acc.append((a, i, lane))
        self.by_type[kind] = self.by_type.get(kind, 0) + len(addrs)
        self.ops += 1

    def chunks(self, seq, kind, base, stride):
        """Emit `seq` as lane-grouped instructions at base + seq[j]*stride."""
        for k in range(0, len(seq), self.lanes):
            self.instr(kind, [base + seq[j] * stride
                              for j in range(k, min(k + self.lanes, len(seq)))])


def write_trace(tr, out, elem_bytes, meta):
    with open(out, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<IIQQ", tr.lanes, elem_bytes, len(tr.cls), len(tr.acc)))
        f.write(bytes(tr.cls))
        buf = bytearray()
        for addr, instr, lane in tr.acc:
            buf += struct.pack("<QII", addr, instr, lane)
        f.write(bytes(buf))
    # integrity record
    lines, pages, words = set(), set(), set()
    for addr, _, _ in tr.acc:
        ln = addr // LINE
        lines.add(ln)
        words.add(addr)
        pages.add(ln // 64)
    h = hashlib.sha256()
    for addr, instr, lane in tr.acc:
        h.update(struct.pack("<QII", addr, instr, lane))
    meta.update({
        "trace_bytes": len(tr.acc),
        "instructions": len(tr.cls),
        "instructions_bulk": sum(1 for c in tr.cls if c == BULK),
        "instructions_simt": sum(1 for c in tr.cls if c == SIMT),
        "accesses": len(tr.acc),
        "by_type": tr.by_type,
        "unique_lines": len(lines),
        "unique_words": len(words),
        "unique_pages": len(pages),
        "duplicate_line_frac": 1.0 - len(lines) / len(tr.acc),
        "write_accesses": 0,
        "read_write_ratio": "read-only trace",
        "trace_sha256": h.hexdigest(),
    })
    with open(out + ".json", "w") as f:
        json.dump(meta, f, indent=2, sort_keys=True)
    return meta


# --------------------------------------------------------------------------- #
# CSR emission shared by spmv and synth
# --------------------------------------------------------------------------- #
def emit_csr(m, n, indptr, colidx, lanes, variant, meta):
    """Emit the M18TRC1 instruction stream for a CSR-shaped single-level gather.

    Extracted verbatim from build_spmv in M21 so that the synthetic workloads
    (tools ``build_trace.py synth``) have exactly the same instruction stream
    shape, descriptors, layout and accounting as the real matrices. Program
    order: rowptr, colidx, value, x_gather per row, lane-grouped.
    """
    nnz = len(colidx)
    x_base = 0
    rowptr_base = align(max(m, n) * 8)
    colidx_base = max(align(rowptr_base + (m + 1) * 4), align(nnz * 8) + 65536)
    value_base = align(colidx_base + nnz * 4)
    end = align(value_base + nnz * 8)

    tr = Trace(lanes)
    rows_by_len = []
    for i in range(m):
        b, e = indptr[i], indptr[i + 1]
        rows_by_len.append(e - b)
        tr.instr("rowptr", [rowptr_base + i * 4, rowptr_base + (i + 1) * 4])
        if b == e:
            continue
        if variant == "full":
            for k in range(b, e, lanes):
                tr.instr("colidx", [colidx_base + j * 4 for j in range(k, min(k + lanes, e))])
        for k in range(b, e, lanes):
            tr.instr("value", [value_base + j * 8 for j in range(k, min(k + lanes, e))])
        for k in range(b, e, lanes):
            tr.instr("x_gather", [x_base + colidx[j] * 8 for j in range(k, min(k + lanes, e))])

    meta.update({
        "rows": m, "cols": n, "nnz_full": nnz,
        "row_len_min": min(rows_by_len), "row_len_max": max(rows_by_len),
        "row_len_mean": sum(rows_by_len) / m if m else 0.0,
        "unique_column_refs": len(set(colidx)),
        "refs_per_unique_column": nnz / len(set(colidx)) if colidx else 0.0,
        "layout": {"x_base": x_base, "rowptr_base": rowptr_base,
                   "colidx_base": colidx_base, "value_base": value_base,
                   "max_addr": end,
                   "dram_rows_needed_16bank_1kib": (end + 16 * 1024 - 1) // (16 * 1024)},
        "bulk": {"data_base": x_base, "index_base": colidx_base,
                 "desc_line": colidx_base // LINE - 1, "count": nnz},
    })
    return tr


# --------------------------------------------------------------------------- #
# SpMV
# --------------------------------------------------------------------------- #
def build_spmv(args):
    m, n, indptr, colidx, sym, stored = read_mtx(args.mtx)
    full_rows, full_nnz = m, len(colidx)
    # A row prefix keeps a very large matrix inside the cycle budget. It is the
    # same logical work in both arms (rows 0..k-1 of the product), the x vector
    # stays complete, and the prefix is reported as a fraction of the matrix.
    if args.max_rows and args.max_rows < m:
        m = args.max_rows
        indptr = indptr[: m + 1]
        colidx = colidx[: indptr[m]]
    meta = {
        "kind": "spmv", "dataset": args.mtx, "variant": args.variant,
        "nnz_stored": stored, "matrix_rows_total": full_rows,
        "matrix_nnz_total": full_nnz, "row_prefix_fraction": m / full_rows,
        "symmetric_expanded": sym,
    }
    tr = emit_csr(m, n, indptr, colidx, args.lanes, args.variant, meta)

    # application output: y = A*x, a fixed deterministic x
    x = [1.0 + (i % 7) * 0.5 for i in range(n)]
    checksum = 0.0
    for i in range(m):
        s = 0.0
        for j in range(indptr[i], indptr[i + 1]):
            s += x[colidx[j]]
        checksum += s
    meta["app_output_checksum"] = checksum
    meta["app_output_x"] = "x[i] = 1 + (i mod 7) * 0.5"
    return tr, meta


# --------------------------------------------------------------------------- #
# BFS
# --------------------------------------------------------------------------- #
def build_bfs(args):
    n, indptr, indices = read_edgelist(args.edges)
    lanes = args.lanes
    dist_base = 0
    # M19 correction: `dist` is 8 B per vertex (dist_base + u*8 below), so the
    # gap after it must be sized by 8n. `align(n*4)` put the row-pointer array
    # inside the distance array on every BFS graph. M18 BFS numbers were
    # all-SIMT observations; the corrected traces are reported separately.
    rowptr_base = align(n * 8)
    adj_base = align(rowptr_base + (n + 1) * 4)
    nbr_base = align(adj_base + len(indices) * 4)      # frontier + worklist
    end = align(nbr_base + n * 4)

    # execution: real level-synchronous BFS, recording the accesses it issues.
    # source default = highest-degree vertex: deterministic, and it keeps the
    # traversal from degenerating on a graph whose id 0 happens to be isolated
    # (the SNAP id space is not guaranteed contiguous).
    src = args.source
    if src < 0:
        src = max(range(n), key=lambda v: indptr[v + 1] - indptr[v])
    dist = [-1] * n
    dist[src] = 0
    frontier = [src]
    tr = Trace(lanes)
    level_sizes = []
    probes = 0
    dup_probes = 0
    while frontier:
        level_sizes.append(len(frontier))
        dups = 0
        # frontier list itself
        for k in range(0, len(frontier), lanes):
            tr.instr("frontier", [nbr_base + j * 4 for j in range(k, min(k + lanes, len(frontier)))])
        nxt = []
        for v in frontier:
            b, e = indptr[v], indptr[v + 1]
            tr.instr("rowptr", [rowptr_base + v * 4, rowptr_base + (v + 1) * 4])
            if b == e:
                continue
            for k in range(b, e, lanes):
                tr.instr("adj", [adj_base + j * 4 for j in range(k, min(k + lanes, e))])
            # neighbour probe: dist[u], u loaded from adj -- a second-level
            # dependent gather; reported as SIMT because the engine cannot chain.
            # `noprobe` drops this stream so its share of the runtime can be
            # measured: it is an upper bound on what an engine able to consume
            # it could ever save, not a Bulk configuration.
            if args.variant == "noprobe":
                for j in range(b, e):
                    u = indices[j]
                    if dist[u] == -1:
                        dist[u] = dist[v] + 1
                        nxt.append(u)
                continue
            for k in range(b, e, lanes):
                addrs = [dist_base + indices[j] * 8 for j in range(k, min(k + lanes, e))]
                tr.instr("dist_probe", addrs)
                probes += len(addrs)
                for j in range(k, min(k + lanes, e)):
                    u = indices[j]
                    if dist[u] != -1:
                        dups += 1
                    else:
                        dist[u] = dist[v] + 1
                        nxt.append(u)
        dup_probes += dups
        frontier = sorted(set(nxt))

    reached = sum(1 for d in dist if d != -1)
    vdeg = [indptr[i + 1] - indptr[i] for i in range(n)]
    meta = {
        "kind": "bfs", "dataset": args.edges, "source": src,
        "vertices": n, "edges_undirected": len(indices) // 2,
        "degree_mean": len(indices) / n, "degree_max": max(vdeg),
        "levels": len(level_sizes), "frontier_sizes": level_sizes,
        "max_frontier": max(level_sizes) if level_sizes else 0,
        "reached_vertices": reached,
        "dist_probes": probes, "duplicate_probes": dup_probes,
        "duplicate_probe_frac": (dup_probes / probes) if probes else 0.0,
        "layout": {"dist_base": dist_base, "rowptr_base": rowptr_base,
                   "adj_base": adj_base, "frontier_base": nbr_base, "max_addr": end},
        "app_output_checksum": float(sum(dist[i] for i in range(n) if dist[i] > 0)),
        "app_output": "sum of BFS distances",
        "indirection_levels": {
            "frontier->rowptr[v]": 2, "rowptr->adj": 2, "adj->dist[u]": 2,
            "note": "every stream needs at least one gather to reach the next; the "
                    "engine supports exactly one level and no writes",
        },
    }
    return tr, meta


# --------------------------------------------------------------------------- #
# Synthetic single-level indirect access (M21)
# --------------------------------------------------------------------------- #
PATTERNS = ("uniform", "clustered", "dup_high", "dup_low", "sequential", "zipf")


def synth_indices(ops, cols, pattern, seed, cluster_frac, cluster_pages,
                  distinct, stride, zipf_a):
    """Deterministic index vector for one M21 workload.

    Only the VALUES of the index vector differ between patterns. The instruction
    stream, row partition, index-array length, value array and every descriptor
    are identical, so the three arms perform the same logical operations and the
    index stream is byte-identical in length and layout across patterns.
    """
    import bisect
    import random
    rnd = random.Random(seed)
    C = cols
    if pattern == "uniform":
        out = [rnd.randrange(C) for _ in range(ops)]
    elif pattern == "clustered":
        pages = (C + 7) // 8
        hot_pages = max(1, int(round(pages * cluster_pages)))
        hot = min(hot_pages * 8, C)
        out = []
        for _ in range(ops):
            out.append(rnd.randrange(hot) if rnd.random() < cluster_frac
                       else rnd.randrange(C))
    elif pattern == "dup_high":
        d = max(1, min(distinct, C))
        base = [rnd.randrange(C) for _ in range(d)]
        out = [base[rnd.randrange(d)] for _ in range(ops)]
    elif pattern == "dup_low":
        # `distinct` lines, each index placed on its own line: the low-duplicate
        # control. Requires a column space of at least 8 * distinct so that no
        # two indices can share a 64 B line.
        n_lines = distinct
        if 8 * n_lines > C:
            raise SystemExit("dup_low needs --cols >= 8 * --distinct")
        perm = list(range(n_lines))
        rnd.shuffle(perm)
        out = [8 * perm[i % n_lines] for i in range(ops)]
    elif pattern == "sequential":
        out = [(i * stride) % C for i in range(ops)]
    elif pattern == "zipf":
        cum, t = [], 0.0
        for k in range(C):
            t += 1.0 / (k + 1) ** zipf_a
            cum.append(t)
        out = [min(C - 1, bisect.bisect_left(cum, rnd.random() * t)) for _ in range(ops)]
    else:
        raise SystemExit("unknown pattern: " + pattern)
    return out


def build_synth(args):
    ops, rows, C = args.ops, args.rows, args.cols
    if rows <= 0 or ops % rows:
        raise SystemExit("ops must be a positive multiple of rows")
    colidx = synth_indices(ops, C, args.pattern, args.seed, args.cluster_frac,
                           args.cluster_pages, args.distinct, args.stride, args.zipf_a)
    per = ops // rows
    indptr = [i * per for i in range(rows)] + [ops]
    h = hashlib.sha256()
    for c in colidx:
        h.update(struct.pack("<I", c))
    meta = {
        "kind": "synth", "variant": args.variant,
        "dataset": "synthetic single-level indirect access",
        "pattern": args.pattern, "seed": args.seed,
        "generation": {
            "ops": ops, "rows": rows, "nnz_per_row": per, "cols": C,
            "pattern": args.pattern, "seed": args.seed,
            "cluster_frac": args.cluster_frac, "cluster_pages": args.cluster_pages,
            "distinct": args.distinct, "stride": args.stride, "zipf_a": args.zipf_a,
            "elem_bytes": 8, "lanes": args.lanes, "rng": "python random.Random(seed)",
            "sorted_per_row": False,
        },
        "index_vector_sha256": h.hexdigest(),
        "logical_ops": ops,
        "target_working_set_bytes": C * 8,
        "matrix_rows_total": rows, "matrix_nnz_total": ops,
        "row_prefix_fraction": 1.0, "symmetric_expanded": False,
    }
    tr = emit_csr(rows, C, indptr, colidx, args.lanes, args.variant, meta)
    x = [1.0 + (i % 7) * 0.5 for i in range(C)]
    checksum = 0.0
    for i in range(rows):
        s = 0.0
        for j in range(indptr[i], indptr[i + 1]):
            s += x[colidx[j]]
        checksum += s
    meta["app_output_checksum"] = checksum
    meta["app_output_x"] = "x[i] = 1 + (i mod 7) * 0.5"
    return tr, meta


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kind", choices=["spmv", "bfs", "synth"])
    ap.add_argument("--mtx")
    ap.add_argument("--edges")
    ap.add_argument("--out", required=True)
    ap.add_argument("--variant", default="full", choices=["full", "noidx", "noprobe"])
    ap.add_argument("--source", type=int, default=-1,
                    help="start vertex; default = highest degree (deterministic)")
    ap.add_argument("--lanes", type=int, default=4)
    ap.add_argument("--max-rows", type=int, default=0,
                    help="use only the first N rows (0 = whole matrix)")
    # --- synthetic single-level indirect access (M21) ---
    ap.add_argument("--ops", type=int, default=131072, help="elements (= nnz)")
    ap.add_argument("--rows", type=int, default=8192)
    ap.add_argument("--cols", type=int, default=16384,
                    help="column space = target working set (8 B each)")
    ap.add_argument("--pattern", default="uniform", choices=list(PATTERNS))
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cluster-frac", type=float, default=0.9,
                    help="fraction of accesses that hit the hot page range")
    ap.add_argument("--cluster-pages", type=float, default=0.05,
                    help="fraction of the page space that is hot")
    ap.add_argument("--distinct", type=int, default=512,
                    help="distinct indices for pattern=dup_high")
    ap.add_argument("--stride", type=int, default=1,
                    help="index stride for pattern=sequential")
    ap.add_argument("--zipf-a", type=float, default=1.0)
    a = ap.parse_args()
    if a.kind == "spmv":
        tr, meta = build_spmv(a)
    elif a.kind == "bfs":
        tr, meta = build_bfs(a)
    else:
        tr, meta = build_synth(a)
    m = write_trace(tr, a.out, 8, meta)
    print(json.dumps({k: m[k] for k in ("kind", "instructions", "instructions_bulk",
                                        "accesses", "unique_lines", "unique_pages",
                                        "duplicate_line_frac", "app_output_checksum")}, indent=2))


if __name__ == "__main__":
    sys.exit(main())
