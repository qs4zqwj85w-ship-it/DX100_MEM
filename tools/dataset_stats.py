#!/usr/bin/env python3
"""M18: dataset structural statistics.

Selection must be driven by measured structure (row-length distribution, column
reuse, page span), not by what happens to favour the accelerator. Pure stdlib:
the host has no numpy.

Usage: dataset_stats.py <file.mtx|file.txt[.gz]> [...]
"""
import gzip
import math
import sys


def open_text(path):
    if path.endswith(".gz"):
        return gzip.open(path, "rt", errors="replace")
    return open(path, "r", errors="replace")


def read_mtx(path):
    """Return (nrows, ncols, rows) with rows[i] = sorted column list (0-based).

    Symmetric/Hermitian storage is expanded to the full structure, which is what
    an SpMV kernel must touch; the stored-nnz count is kept separately so the
    report can state both.
    """
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
            parts = line.split()
            if nrows is None:
                nrows, ncols, _ = (int(parts[0]), int(parts[1]), int(parts[2]))
                continue
            entries.append((int(parts[0]) - 1, int(parts[1]) - 1))
    rows = [[] for _ in range(nrows)]
    for i, j in entries:
        rows[i].append(j)
        if sym and i != j:
            rows[j].append(i)
    for r in rows:
        r.sort()
    return nrows, ncols, rows, len(entries), sym


def read_edgelist(path):
    """SNAP-style 'u v' edge list, undirected, 0-based, self-loops dropped."""
    adj = {}
    with open_text(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            u, v = int(parts[0]), int(parts[1])
            if u == v:
                continue
            adj.setdefault(u, set()).add(v)
            adj.setdefault(v, set()).add(u)
    n = (max(adj) + 1) if adj else 0
    rows = [sorted(adj.get(i, ())) for i in range(n)]
    return n, n, rows, sum(len(r) for r in rows) // 2, False


def pct(xs, p):
    if not xs:
        return 0
    k = min(len(xs) - 1, max(0, int(round(p * (len(xs) - 1)))))
    return xs[k]


def stats(path, value_bytes=8, index_bytes=4, page_bytes=4096):
    if path.endswith(".mtx"):
        n, m, rows, stored, sym = read_mtx(path)
        kind = "matrix"
    else:
        n, m, rows, stored, sym = read_edgelist(path)
        kind = "graph"
    nnz = sum(len(r) for r in rows)
    lens = sorted(len(r) for r in rows)
    mean = nnz / n if n else 0.0
    var = sum((len(r) - mean) ** 2 for r in rows) / n if n else 0.0
    sd = math.sqrt(var)
    refs = [j for r in rows for j in r]
    uniq_cols = len(set(refs))
    touches = len(refs)
    # Page-level locality: how many distinct 4 KiB pages of the *column index
    # space* a row references, and the mean per-row page span.
    pages_per_row = []
    for r in rows:
        if not r:
            pages_per_row.append(0)
            continue
        pages_per_row.append(len({j * index_bytes // page_bytes for j in r}))
    span_lines = 0
    for r in rows:
        if not r:
            continue
        span_lines += (min(r[-1] * index_bytes // 64, 0xFFFFFFFF) -
                       min(r[0] * index_bytes // 64, 0)) + 1
    rowptr_bytes = (n + 1) * index_bytes
    colidx_bytes = nnz * index_bytes
    val_bytes = nnz * value_bytes
    x_bytes = m * value_bytes
    y_bytes = n * value_bytes
    ws = rowptr_bytes + colidx_bytes + val_bytes + x_bytes + y_bytes
    print(f"== {path}")
    print(f"   kind              {kind}{' (symmetric, expanded)' if sym else ''}")
    print(f"   dims              {n} x {m}")
    print(f"   nnz stored        {stored}")
    print(f"   nnz full          {nnz}")
    print(f"   avg row length    {mean:.2f}   sd {sd:.2f}   cv {sd/mean if mean else 0:.3f}")
    print(f"   row len p0/p50/p90/p99/p100  {lens[0]}/{pct(lens,.5)}/{pct(lens,.9)}/{pct(lens,.99)}/{lens[-1]}")
    print(f"   unique col refs   {uniq_cols}  ({uniq_cols/m*100:.1f}% of columns)")
    print(f"   ref/unique        {touches/uniq_cols:.2f}")
    print(f"   repeat fraction   {1 - uniq_cols/touches:.4f}")
    print(f"   pages/row mean    {sum(pages_per_row)/n:.2f}  max {max(pages_per_row) if pages_per_row else 0}")
    print(f"   bytes rowptr      {rowptr_bytes}")
    print(f"   bytes colidx      {colidx_bytes}")
    print(f"   bytes values      {val_bytes}")
    print(f"   bytes x / y       {x_bytes} / {y_bytes}")
    print(f"   working set       {ws} B ({ws/1048576:.2f} MiB)")
    print()


if __name__ == "__main__":
    for p in sys.argv[1:]:
        try:
            stats(p)
        except Exception as e:  # keep going: one bad file must not hide the rest
            print(f"== {p}\n   ERROR {e}\n")
