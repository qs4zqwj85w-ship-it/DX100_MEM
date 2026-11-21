#!/usr/bin/env python3
"""M23-C: build a neighbour-property gather trace from a real SNAP graph.

Dataset
-------
SNAP com-Orkut (`com-orkut.ungraph.txt.gz`), 3 072 441 vertices /
117 185 083 undirected edges, read from the file's own edge list. Nothing about
the graph is generated: the CSR, the subset and the neighbour IDs are the
dataset's.

Subset
------
A BFS-grown induced subgraph from a deterministic seed (the highest-degree
vertex), taking vertices in BFS discovery order until `--vertices` is reached.
The last level is truncated, so the subset is "levels 0..k-1 plus a prefix of
level k" — documented, and the induced edge set keeps every edge between
selected vertices (it is not a truncation of the edge list).

Measured kernel
---------------
One level of indirection:   out[j] = prop[ adj[j] ]   (vertex property gather)
The CSR build and the BFS are **preprocessing**, done here, offline. They are
not in the trace and are not charged to any configuration.

Emission
--------
Per selected vertex, in BFS discovery order, its full adjacency list is gathered
in `lanes`-sized instruction chunks. An instruction is annotated BULK when the
vertex's degree in the subset is >= `--bulk-degree` (a full warp of neighbours,
independent and irregular); smaller lists stay SIMT, where the coalescer already
handles them.

Layout (VA)
    prop      [0, 64*S)                      one 64 B property record per vertex
    rowptr    aligned, (S+1)*4
    colidx    aligned, 4*E_subset
    frontier  aligned, 4*S

Usage
  m23_build_orkut.py --edges data/orkut/com-orkut.ungraph.txt.gz \
      --vertices 65536 --max-accesses 1500000 --policy bfs --out t.bin
"""
import argparse
import gzip
import hashlib
import json
import os
import struct
import sys

import numpy as np

MAGIC = b"M18TRC1\x00"
SIMT, BULK = 0, 1
LINE = 64
PROP_BYTES = 64          # one property record per vertex = one cache line


def align(v, a=64):
    return (v + a - 1) // a * a


def load_edges(path, cache):
    if os.path.exists(cache):
        return np.load(cache)
    with gzip.open(path, "rb") as f:
        raw = f.read()
    i = 0
    while raw[i:i + 1] == b"#":
        i = raw.index(b"\n", i) + 1
    tok = raw[i:].split()
    a = np.fromiter((int(t) for t in tok), dtype=np.int32, count=len(tok))
    del raw, tok
    np.save(cache, a)
    return a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--edges", required=True)
    ap.add_argument("--cache", default="/tmp/orkut_edges.npy")
    ap.add_argument("--vertices", type=int, default=65536)
    ap.add_argument("--max-accesses", type=int, default=1500000)
    ap.add_argument("--bulk-degree", type=int, default=8)
    ap.add_argument("--policy", default="bfs", choices=["bfs", "hub", "tail"])
    ap.add_argument("--lanes", type=int, default=4)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.lanes != 4:
        sys.exit("M23 fixes lanes = 4")

    e = load_edges(a.edges, a.cache)
    U, V = e[0::2], e[1::2]
    E = U.size
    n = int(max(U.max(), V.max())) + 1
    deg = (np.bincount(U, minlength=n) + np.bincount(V, minlength=n)).astype(np.int64)
    seed = int(np.argmax(deg))

    # ---- BFS-grown induced subset -----------------------------------------
    visited = np.zeros(n, dtype=bool)
    visited[seed] = True
    order = [seed]
    frontier = np.array([seed], dtype=np.int64)
    while len(order) < a.vertices and frontier.size:
        m_u = np.isin(U, frontier)
        m_v = np.isin(V, frontier)
        nb = np.unique(np.concatenate([V[m_u], U[m_v]]))
        nb = nb[~visited[nb]]
        if nb.size == 0:
            break
        take = nb[: a.vertices - len(order)]
        visited[take] = True
        order.extend(int(x) for x in take)
        frontier = take
    S = len(order)
    order_arr = np.array(order, dtype=np.int64)

    # ---- induced CSR (both directions: the graph is undirected) ------------
    keep = visited[U] & visited[V]
    su, sv = U[keep], V[keep]
    newid = np.full(n, -1, dtype=np.int64)
    newid[order_arr] = np.arange(S)
    r = np.concatenate([newid[su], newid[sv]]).astype(np.int32)
    c = np.concatenate([newid[sv], newid[su]]).astype(np.int32)
    o = np.argsort(r, kind="stable")
    r, c = r[o], c[o]
    indptr = np.zeros(S + 1, dtype=np.int64)
    np.add.at(indptr, r.astype(np.int64) + 1, 1)
    indptr = np.cumsum(indptr)
    nbr = c
    del r, c, o, su, sv, keep, newid

    vdeg = np.diff(indptr)
    sel = {  # vertex-selection policy for the gather kernel
        "bfs": np.arange(S),
        "hub": np.argsort(-vdeg, kind="stable")[: min(S, 4096)],
        "tail": None,
    }[a.policy]
    if a.policy == "tail":
        # lowest-degree vertices that still have neighbours: the operations a
        # compiler should keep on SIMT
        idx = np.argsort(vdeg, kind="stable")
        idx = idx[vdeg[idx] >= 2]
        sel = idx[: min(S, 4096)]

    # ---- layout ------------------------------------------------------------
    prop_base = 0
    rowptr_base = align(S * PROP_BYTES)
    colidx_base = align(rowptr_base + (S + 1) * 4)
    frontier_base = align(colidx_base + nbr.size * 4)
    end = align(frontier_base + S * 4)

    # ---- instruction stream ----------------------------------------------
    cls, acc = [], []
    instr = 0
    nsel = 0
    dsel = []
    access = 0
    for v in sel:
        if access >= a.max_accesses:
            break
        b, e2 = indptr[v], indptr[v + 1]
        d = int(e2 - b)
        if d == 0:
            continue
        nsel += 1
        dsel.append(d)
        k = BULK if d >= a.bulk_degree else SIMT
        for j in range(b, e2, a.lanes):
            cls.append(k)
            hi = min(j + a.lanes, e2)
            for lane, jj in enumerate(range(j, hi)):
                u = int(nbr[jj])
                acc.append((prop_base + u * PROP_BYTES, instr, lane))
            instr += 1
            access += hi - j
            if access >= a.max_accesses:
                break

    cls_b = sum(1 for c_ in cls if c_ == BULK)
    with open(a.out, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<IIQQ", a.lanes, 4, len(cls), len(acc)))
        f.write(bytes(cls))
        buf = bytearray()
        for ad, ins, lane in acc:
            buf += struct.pack("<QII", ad, ins, lane)
        f.write(bytes(buf))

    lines = {ad // LINE for ad, _, _ in acc}
    pages = {ad // (LINE * 64) for ad, _, _ in acc}
    h = hashlib.sha256()
    for ad, ins, lane in acc:
        h.update(struct.pack("<QII", ad, ins, lane))
    with open(a.edges, "rb") as fh:
        edge_sha = hashlib.sha256(fh.read()).hexdigest()

    dsel = np.array(dsel) if dsel else np.array([0])
    meta = {
        "source": {"dataset": "SNAP com-Orkut (com-orkut.ungraph.txt.gz)",
                   "url": "https://snap.stanford.edu/data/bigdata/communities/com-orkut.ungraph.txt.gz",
                   "file_sha256": edge_sha,
                   "graph_vertices": 3072441, "graph_edges_undirected": 117185083,
                   "graph_vertices_in_file": n, "parsed_edges": int(E)},
        "subset": {"policy": "BFS-grown induced subgraph from the max-degree vertex",
                   "seed": seed, "seed_degree": int(deg[seed]),
                   "vertices": S, "induced_edges_undirected": int(nbr.size // 2),
                   "edges_directed_in_csr": int(nbr.size)},
        "kernel": {"policy": a.policy, "vertices_selected": int(nsel),
                   "accesses": len(acc), "instructions": len(cls),
                   "instructions_bulk": int(cls_b),
                   "instructions_simt": int(len(cls) - cls_b),
                   "bulk_degree_threshold": a.bulk_degree,
                   "max_accesses_cap": a.max_accesses},
        "degree": {"mean": float(vdeg.mean()), "max": int(vdeg.max()),
                   "median": int(np.median(vdeg)),
                   "p90": int(np.percentile(vdeg, 90)),
                   "p99": int(np.percentile(vdeg, 99)),
                   "selected_mean": float(dsel.mean()),
                   "selected_max": int(dsel.max()),
                   "selected_median": float(np.median(dsel))},
        "layout": {"prop_base": prop_base, "prop_bytes": S * PROP_BYTES,
                   "rowptr_base": rowptr_base, "colidx_base": colidx_base,
                   "frontier_base": frontier_base, "max_addr": end,
                   "index_base": 0x80000000, "desc_line": 0x7FFFF000 // 64,
                   "elem_bytes": 4, "data_base": 0},
        "totals": {"unique_lines": len(lines), "unique_pages": len(pages),
                   "working_set_bytes": len(lines) * LINE,
                   "duplicate_line_frac": round(1 - len(lines) / len(acc), 6)},
        "excluded_from_measurement": [
            "edge-list parse and CSR construction",
            "BFS level expansion and subset selection",
            "the second-level dependent gather dist[adj[j]] (the engine cannot chain)",
            "frontier / worklist bookkeeping",
        ],
        "trace_sha256": h.hexdigest(),
    }
    with open(a.out + ".json", "w") as f:
        json.dump(meta, f, indent=2, sort_keys=True)

    print(f"subset S={S} induced_edges={nbr.size//2} seed={seed} (deg {deg[seed]})")
    print(f"kernel {a.policy}: vertices={nsel} acc={len(acc)} instr={len(cls)} "
          f"bulk={cls_b} simt={len(cls)-cls_b}")
    print(f"lines={len(lines)} pages={len(pages)} ws={len(lines)*LINE/(1<<20):.2f} MiB "
          f"dup_line={1-len(lines)/len(acc):.3f}")
    print(f"deg mean={vdeg.mean():.1f} max={vdeg.max()} p90={np.percentile(vdeg,90):.0f} "
          f"p99={np.percentile(vdeg,99):.0f} | selected mean={dsel.mean():.1f} "
          f"max={dsel.max()}")


if __name__ == "__main__":
    main()
