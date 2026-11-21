#!/usr/bin/env python3
"""M24-F: the workload manifest — every trace in the generalization matrix.

Writes build/wm/m24f/traces.tsv, one row per trace, with the fields that are
*workload* properties rather than architecture resources:

    id  group  trace  elem_bytes  bulk_index_base  bulk_desc_line
    bulk_max_elems  accesses  instructions  max_addr  lanes

elem_bytes and the access/instruction counts come from the M18TRC1 header
itself, not from a report.  looseness: index_base / desc_line / max_elems come
from the trace's own JSON when the builder recorded them (M18 SpMV, M21 synth,
M24-F synth); an M23 graph trace records no descriptor block, and for those the
three fields are left empty, meaning "keep the configuration's value" — which
is what M24-D ran them with.

No architecture field is in this file.  Both arms of every cell are launched from
this same row, so a bulk/SIMT pair differs only in its config.
"""
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W = os.path.join(ROOT, "build", "wm")
OUT = os.path.join(W, "m24f", "traces.tsv")

M21 = ["w1_uniform", "w2_clustered", "w3_duphigh", "w4_duplow", "w5_sequential",
       "w6_zipf", "w7_wssmall", "w8_wslarge", "w9_seqsmall", "w10_runresid"]

# id, group, path relative to build/wm
TRACES = []
for w in M21:
    TRACES.append((w, "synth/s1", f"m21_{w}_s1_full.bin"))
for w in M21:
    # w9_seqsmall was added after the three-seed sweep; it has seed 1 only
    if os.path.exists(os.path.join(W, f"m21_{w}_s2_full.bin")):
        TRACES.append((f"{w}_s2", "synth/s2", f"m21_{w}_s2_full.bin"))
for n, f in (("bcsstk25", "m18_bcsstk25_full.bin"),
             ("qc2534", "m18_qc2534_full.bin"),
             ("lnsp3937", "m18_lnsp3937_full.bin"),
             ("dubcova3", "m18_dubcova3_full.bin")):
    TRACES.append((n, "spmv", f))
for n, f in (("orkut_hub", "m23_orkut_hub.bin"),
             ("orkut_tail", "m23_orkut_tail.bin"),
             ("ogbn_uniform", "m23_ogbn_uniform.bin"),
             ("criteo_2k", "m23_criteo_2k.bin")):
    TRACES.append((n, "graph", f))
for n in ("n1_seq8m", "n2_zipf4m", "n3_clu4m", "n4_uni4m", "n5_seq8m_s4"):
    TRACES.append((n, "new", f"m24f/{n}.bin"))


def header(path):
    with open(path, "rb") as f:
        magic = f.read(8)
        if magic != b"M18TRC1\x00":
            raise SystemExit(f"{path}: not an M18TRC1 trace")
        lanes, elem, ncls, nacc = struct.unpack("<IIQQ", f.read(24))
    return lanes, elem, nacc


rows = []
for tid, group, rel in TRACES:
    path = os.path.join(W, rel)
    if not os.path.exists(path):
        raise SystemExit(f"missing trace {path}")
    lanes, elem, nacc = header(path)
    meta = {}
    jp = path + ".json"
    if os.path.exists(jp):
        meta = json.load(open(jp))
    b = meta.get("bulk") or {}
    lay = meta.get("layout") or {}
    instrs = meta.get("instructions") or ""
    idx = b.get("index_base", "")
    desc = b.get("desc_line", "")
    cnt = b.get("count", "")
    maxe = max(cnt, nacc) if cnt != "" else ""
    max_addr = lay.get("max_addr", "")
    rows.append([tid, group, rel, elem, idx, desc, maxe, nacc, instrs, max_addr, lanes])

with open(OUT, "w") as f:
    f.write("id\tgroup\ttrace\telem_bytes\tbulk_index_base\tbulk_desc_line\t"
            "bulk_max_elems\taccesses\tinstructions\tmax_addr\tlanes\n")
    for r in rows:
        f.write("\t".join(str(x) for x in r) + "\n")

print(f"wrote {OUT}: {len(rows)} traces")
for r in rows:
    print(f"  {r[0]:22s} {r[1]:9s} {str(r[7]):>9s} acc  elem={r[3]}  idx={r[4]}  desc={r[5]}")
