#!/usr/bin/env python3
"""M23-G5B: neighbour-property gather trace from a second real SNAP graph.

Purpose
-------
M23-C built the Orkut gather; M23-G3 through G4C characterized how the Bulk
organization policies (P0..P3) behave on it. M23-G5B asks whether that behaviour
generalizes, so this builder reproduces M23-C's construction *exactly* on a
different graph. Only the dataset differs; every rule below is M23-C's:

  * the graph is read from the file's own edge list, symmetrized (undirected),
    no sampling, no generation;
  * the measured subset is a BFS-grown **induced** subgraph from the max-degree
    vertex, taking vertices in discovery order until `--vertices` are selected,
    keeping every edge whose two endpoints are both selected;
  * one 64 B property record per selected vertex, one cache line per neighbour
    visit (the same line granularity a GNN feature gather has);
  * per selected vertex, its full adjacency list is gathered in `lanes`-sized
    instruction chunks, BULK when the subset degree is >= `--bulk-degree`;
  * `--max-accesses` caps admitted neighbour slots, so two graphs run with the
    same cap execute the same number of gather accesses.

Measured kernel
---------------
    for each selected vertex v, for each u in adj[v]:   out[j] = prop[u]

Excluded (preprocessing, charged to nobody): edge-list parse, CSR construction,
BFS level expansion, subset selection, frontier bookkeeping.

Layout (VA), identical to M23-C
    prop      [0, 64*S)                      one 64 B property record per vertex
    rowptr    aligned, (S+1)*4
    colidx    aligned, 4*E_subset
    frontier  aligned, 4*S

The dataset identity is a required argument group: this script does not assume
which graph it is reading, so the metadata cannot silently describe Orkut.

Usage
  m23_build_g5b.py --edges data/livejournal/soc-LiveJournal1.txt.gz \
      --dataset "SNAP soc-LiveJournal1" --url https://... \
      --graph-vertices 4847571 --graph-edges-file 68993773 \
      --vertices 1048576 --max-accesses 1200000 --policy bfs --out t.bin
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
    ap.add_argument("--cache", default="/tmp/g5b_edges.npy")
    ap.add_argument("--dataset", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--graph-vertices", type=int, required=True)
    # entries in the dataset file, not undirected edges: soc-LiveJournal1 stores
    # each unordered pair once with a direction, and the builder symmetrizes it.
    ap.add_argument("--graph-edges-file", type=int, required=True)
    ap.add_argument("--vertices", type=int, default=1048576)
    ap.add_argument("--max-accesses", type=int, default=1200000)
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

    # ---- BFS-grown induced subset (M23-C's rule) ---------------------------
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

    # ---- induced CSR (both directions: treated as undirected) --------------
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
    sel = {  # vertex-selection policy for the gather kernel (M23-C)
        "bfs": np.arange(S),
        "hub": np.argsort(-vdeg, kind="stable")[: min(S, 4096)],
        "tail": None,
    }[a.policy]
    if a.policy == "tail":
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

    # DRAM mapping period of the frozen M23 memory system: banks x rows x
    # row_bytes = 2 x 4096 x 1024 = 8 MiB. Coverage says how much of one period
    # the property array spans; the touched set is what the accesses actually hit.
    period = 2 * 4096 * 1024
    dsel = np.array(dsel) if dsel else np.array([0])
    meta = {
        "source": {"dataset": a.dataset, "url": a.url,
                   "file_sha256": edge_sha,
                   "graph_vertices": a.graph_vertices,
                   "graph_edges_in_file": a.graph_edges_file,
                   "graph_edges_after_symmetrization": 2 * a.graph_edges_file,
                   "graph_vertices_in_file": n, "parsed_edges": int(E),
                   "treatment": ("file entries read as an edge list and symmetrized, so each "
                                 "file entry becomes two directed CSR arcs")},
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
        "footprint": {"prop_array_bytes": S * PROP_BYTES,
                      "dram_period_bytes": period,
                      "prop_array_periods": round(S * PROP_BYTES / period, 4),
                      "touched_lines": len(lines),
                      "touched_bytes": len(lines) * LINE,
                      "touched_periods": round(len(lines) * LINE / period, 4),
                      "touched_pages": len(pages)},
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
    print(f"prop_array={S*PROP_BYTES/(1<<20):.1f} MiB ({S*PROP_BYTES/period:.2f} periods) "
          f"touched={len(lines)*LINE/(1<<20):.2f} MiB lines={len(lines)} pages={len(pages)} "
          f"dup_line={1-len(lines)/len(acc):.3f}")
    print(f"deg mean={vdeg.mean():.2f} median={np.median(vdeg):.0f} p90={np.percentile(vdeg,90):.0f} "
          f"p99={np.percentile(vdeg,99):.0f} max={vdeg.max()} | selected mean={dsel.mean():.1f} "
          f"median={np.median(dsel):.0f} max={dsel.max()}")
    print(f"trace sha={meta['trace_sha256'][:16]}")


if __name__ == "__main__":
    main()
