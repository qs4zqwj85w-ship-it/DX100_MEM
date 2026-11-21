#!/usr/bin/env python3
"""M19 independent recomputation.

Deliberately does NOT import m18_extract.py, m18_metrics.py or m18a_audit.py:
its parsers are written from scratch against the raw file formats, so agreement
with the M18 tables is evidence and not a shared bug.

Subcommands
  cells                 recompute the primary metrics of every M18 cell
  streams <trace>       per-stream / per-class request decomposition from trace
  verify                cross-check trace-derived requests against the simulator
"""
import json
import os
import re
import struct
import sys

MAGIC = b"M18TRC1\x00"
SIMT, BULK = 0, 1
LINE = 64

# --------------------------------------------------------------------------- #
# raw cell log parsing (own regexes; no shared code with the M18 tooling)
# --------------------------------------------------------------------------- #
NUM = r"(\d+)"
RE_CYC = re.compile(r"^  cycles\s+" + NUM + r"\s+" + NUM + r"\s+" + NUM + r"\s*$")
RE_REQ = re.compile(r"^  line requests\s+" + NUM + r"\s+" + NUM + r"\s+" + NUM + r"\s*$")
RE_IDX = re.compile(r"^  index lines\s+" + NUM + r"\s+" + NUM + r"\s+" + NUM + r"\s*$")
RE_TGT = re.compile(r"^  target requests\s+" + NUM + r"\s+" + NUM + r"\s+" + NUM + r"\s*$")
RE_DRAM = re.compile(r"^  DRAM reads\s+" + NUM + r"\s+" + NUM + r"\s+" + NUM + r"\s*$")
RE_WALK = re.compile(r"^  page walks\s+" + NUM + r"\s+" + NUM + r"\s+" + NUM + r"\s*$")
RE_DESC = re.compile(r"^  descriptors\s+" + NUM + r"\s+" + NUM + r"\s+" + NUM + r"\s*$")
RE_M15 = re.compile(r"^  m15 ([a-z0-9_]+)=(-?\d+)\s*$")
RE_CMAP = re.compile(r"class map:\s*(\d+) SIMT \+ (\d+) bulk instructions -> (\d+) \+ (\d+) elements")


def read_cell(path):
    d = {"path": path, "m15": {}, "cd": {}, "gate": None}
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = RE_M15.match(line)
            if m:
                d["m15"][m.group(1)] = int(m.group(2))
                continue
            m = RE_CMAP.search(line)
            if m:
                d["simt_instr"], d["bulk_instr"] = int(m.group(1)), int(m.group(2))
                d["simt_elems"], d["bulk_elems"] = int(m.group(3)), int(m.group(4))
                continue
            for key, rx in (("cyc", RE_CYC), ("req", RE_REQ), ("idx", RE_IDX),
                            ("tgt", RE_TGT), ("dram", RE_DRAM), ("walk", RE_WALK),
                            ("desc", RE_DESC)):
                m = rx.match(line)
                if m:
                    d[key] = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
                    break
            if "M9 GATES" in line and d["gate"] is None:
                d["gate"] = "PASS" if "PASSED" in line else "FAIL"
    return d


def cell_rules(d):
    """Recompute the two M18 comparisons from arm counters only."""
    a_cyc, b_cyc, c_cyc = d["cyc"]
    a_req, b_req, c_req = d["req"]
    a_dram, b_dram, c_dram = d["dram"]
    a_walk, b_walk, c_walk = d["walk"]
    return {
        "tag": os.path.basename(d["path"]).replace(".txt", ""),
        "A_cycles": a_cyc, "C_cycles": c_cyc,
        "A_requests": a_req, "C_requests": c_req,
        "A_dram": a_dram, "C_dram": c_dram,
        "A_walks": a_walk, "C_walks": c_walk,
        "engine_idx_lines": d["idx"][2],
        "engine_idx_bytes": d["idx"][2] * LINE,
        "engine_tgt_req": d["tgt"][2],
        "descriptors": d["desc"][2],
        "simt_elems": d.get("simt_elems"), "bulk_elems": d.get("bulk_elems"),
        "elements_total": (d.get("simt_elems") or 0) + (d.get("bulk_elems") or 0),
        "gate": d["gate"],
        "m15": d["m15"],
    }


# --------------------------------------------------------------------------- #
# trace parsing
# --------------------------------------------------------------------------- #
def read_trace(path):
    with open(path, "rb") as f:
        magic = f.read(8)
        if magic != MAGIC:
            raise SystemExit("bad magic in " + path)
        lanes, elem, n_instr, n_acc = struct.unpack("<IIQQ", f.read(24))
        cls = f.read(n_instr)
        raw = f.read(n_acc * struct.calcsize("<QII"))
    acc = [struct.unpack_from("<QII", raw, i * struct.calcsize("<QII"))
           for i in range(n_acc)]
    return lanes, elem, n_instr, n_acc, cls, acc


def stream_of(addr, layout, x_span):
    """Classify by address range; the ranges are disjoint by construction."""
    if addr < x_span:
        return "x_gather"
    if addr < layout["colidx_base"]:
        return "rowptr"
    if addr < layout["value_base"]:
        return "colidx"
    return "value"


def trace_requests(path, layout, x_span):
    """Per-instruction distinct-line requests, per stream and per class."""
    lanes, elem, n_instr, n_acc, cls, acc = read_trace(path)
    per_instr = [[] for _ in range(n_instr)]
    for addr, instr, lane in acc:
        per_instr[instr].append((addr, lane))
    st = {}
    cl = {SIMT: 0, BULK: 0}
    comp = {}
    for i, rows in enumerate(per_instr):
        if not rows:
            continue
        lines = {}
        for addr, _lane in rows:
            s = stream_of(addr, layout, x_span)
            lines.setdefault((addr // LINE, s), None)
            comp.setdefault(s, set()).add(addr // LINE)
        n = len(lines)
        cl[cls[i]] += n
        # attribute the instruction's requests to the stream of its first lane
        s0 = stream_of(rows[0][0], layout, x_span)
        st[s0] = st.get(s0, 0) + n
    # exact per-stream compaction of compulsory lines
    comp = {k: len(v) for k, v in comp.items()}
    return {"lanes": lanes, "elem": elem, "n_instr": n_instr, "n_acc": n_acc,
            "simt_instr": sum(1 for c in cls if c == SIMT),
            "bulk_instr": sum(1 for c in cls if c == BULK),
            "requests_by_stream": st, "requests_by_class": cl,
            "compulsory_lines_by_stream": comp,
            "requests_total": sum(st.values())}


def layout_of(trace_path):
    with open(trace_path + ".json") as f:
        j = json.load(f)
    return j, j["layout"], j["cols"] * 8


# --------------------------------------------------------------------------- #
# commands
# --------------------------------------------------------------------------- #
def cmd_cells(argv):
    pat = re.compile(r"^m18_(.+)_w(8192|16384)(_mshr128)?\.txt$")
    rows = []
    for name in sorted(os.listdir("build/wm")):
        m = pat.match(name)
        if not m:
            continue
        d = read_cell(os.path.join("build/wm", name))
        if "cyc" not in d:
            continue
        r = cell_rules(d)
        r["dataset"], r["window"], r["mshr128"] = m.group(1), int(m.group(2)), bool(m.group(3))
        rows.append(r)
    print("| cell | A cyc | C cyc | S_same | C_noidx | S_faithful | idx lines | tgt req | gate |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---|")
    by = {r["tag"]: r for r in rows}
    cells = []
    for r in rows:
        base = r["tag"]
        if "_noidx" in base:
            continue
        noidx = base.replace("_full", "_noidx")
        if noidx not in by:
            continue
        c_noidx = by[noidx]["C_cycles"]
        s_same = r["A_cycles"] / r["C_cycles"]
        s_f = r["A_cycles"] / c_noidx
        r["C_noidx_cycles"] = c_noidx
        r["S_same"] = s_same
        r["S_faithful"] = s_f
        cells.append(r)
        print("| %s | %d | %d | %.3f | %d | %.3f | %d | %d | %s |" % (
            base, r["A_cycles"], r["C_cycles"], s_same, c_noidx, s_f,
            r["engine_idx_lines"], r["engine_tgt_req"], r["gate"]))
    with open("build/wm/m19_recomputed.json", "w") as f:
        json.dump({"cells": cells, "all": rows}, f, indent=1, sort_keys=True)
    print("\n%d paired cells -> build/wm/m19_recomputed.json" % len(cells))


def cmd_streams(argv):
    path = argv[0]
    j, layout, x_span = layout_of(path)
    r = trace_requests(path, layout, x_span)
    print("## %s  (lanes %d, %d accesses, %d instructions)" % (
        os.path.basename(path), r["lanes"], r["n_acc"], r["n_instr"]))
    print("| stream | accesses | compulsory lines | requests | bytes/request |")
    print("|---|---:|---:|---:|---:|")
    for s in ("rowptr", "colidx", "value", "x_gather"):
        a = j["by_type"].get(s, 0)
        c = r["compulsory_lines_by_stream"].get(s, 0)
        q = r["requests_by_stream"].get(s, 0)
        bpr = (a * (4 if s in ("rowptr", "colidx") else 8)) / q if q else 0
        print("| %s | %d | %d | %d | %.1f |" % (s, a, c, q, bpr))
    print("| **total** | %d | %d | %d | |" % (
        r["n_acc"], sum(r["compulsory_lines_by_stream"].values()), r["requests_total"]))
    print("\nby class: SIMT %d requests over %d instructions, BULK %d requests over %d instructions" % (
        r["requests_by_class"][SIMT], r["simt_instr"],
        r["requests_by_class"][BULK], r["bulk_instr"]))
    with open(path.replace(".bin", "_streams.json"), "w") as f:
        json.dump(r, f, indent=1, sort_keys=True)


RE_OUTCHK = re.compile(r"output check \(C\): value hash (\d+) vs expectation (\d+), "
                       r"mismatched addresses (\d+)")


def cmd_integrity(argv):
    """Per-cell integrity record: raw-log hash, trace hash, partition, output check."""
    import hashlib
    out = {"cells": [], "reports": {}}
    for name in sorted(os.listdir("build/wm")):
        if not name.startswith("m18_") or not name.endswith(".txt"):
            continue
        p = os.path.join("build/wm", name)
        raw = open(p, "rb").read()
        txt = raw.decode("utf-8", "replace")
        rec = {"log": name, "log_sha256": hashlib.sha256(raw).hexdigest(),
               "log_bytes": len(raw),
               "gate": "PASS" if "M9 GATES PASSED" in txt else "FAIL",
               "violations": [l.strip() for l in txt.splitlines() if "VIOLATION" in l]}
        d = read_cell(p)
        if "cyc" in d:
            rec.update({"A_cycles": d["cyc"][0], "C_cycles": d["cyc"][2],
                        "A_requests": d["req"][0], "C_requests": d["req"][2],
                        "A_dram": d["dram"][0], "C_dram": d["dram"][2],
                        "idx_bytes": d["idx"][2], "descriptors": d["desc"][2],
                        "tgt_req": d["tgt"][2],
                        "simt_elems": d.get("simt_elems"), "bulk_elems": d.get("bulk_elems"),
                        "elem_partition_total": (d.get("simt_elems") or 0) + (d.get("bulk_elems") or 0),
                        "bulk_elems_done": d["m15"].get("bulk_elems_done"),
                        "bulk_addr_mismatch": d["m15"].get("bulk_addr_mismatch"),
                        "bulk_elems_total": d["m15"].get("bulk_elems_total")})
        m = RE_OUTCHK.search(txt)
        if m:
            rec["out_hash"] = int(m.group(1))
            rec["out_expect"] = int(m.group(2))
            rec["out_mismatch"] = int(m.group(3))
        out["cells"].append(rec)

    for r in sorted(os.listdir("docs")):
        if r.startswith("m18") and r.endswith(".md"):
            b = open(os.path.join("docs", r), "rb").read()
            out["reports"][r] = {"sha256": hashlib.sha256(b).hexdigest(), "bytes": len(b)}
    # partition must equal the TRACE's own access count, not itself
    for c in out["cells"]:
        tag = c["log"][4:-4]
        for suf in ("_w8192", "_w16384", "_w8192_mshr128", "_w16384_mshr128"):
            if tag.endswith(suf):
                tag = tag[: -len(suf)]
        js = "build/wm/m18_%s.bin.json" % tag
        if os.path.exists(js):
            d = json.load(open(js))
            c["trace_accesses"] = d["accesses"]
            c["partition_matches_trace"] = (c.get("elem_partition_total") == d["accesses"])
    bad = [c for c in out["cells"] if c["gate"] != "PASS"
           or c.get("out_mismatch") not in (None, 0)
           or c.get("partition_matches_trace") is False
           or (c.get("out_hash") is not None and c.get("out_hash") != c.get("out_expect"))]
    print("%d cells recorded, %d with a gate/output/partition anomaly" %
          (len(out["cells"]), len(bad)))
    for c in bad:
        print("  !", c["log"], c["gate"], c.get("out_mismatch"), c["violations"][:2])
    mism = [c for c in out["cells"] if c.get("out_mismatch") not in (None, 0)]
    print("output-hash mismatches: %d" % len(mism))
    done = [c for c in out["cells"] if c.get("bulk_elems_done") and
            c.get("bulk_elems_done") != c.get("bulk_elems_total")]
    print("bulk element shortfalls: %d" % len(done))
    checked = [c for c in out["cells"] if c.get("trace_accesses")]
    print("partition vs trace accesses: %d cells checked, %d mismatched" % (
        len(checked), sum(1 for c in checked if not c.get("partition_matches_trace"))))
    with open("build/wm/m19_integrity.json", "w") as f:
        json.dump(out, f, indent=1, sort_keys=True)
    print("-> build/wm/m19_integrity.json")


def cmd_verify(argv):
    print("| cell | A requests (sim) | trace SIMT+BULK | C simt requests | trace SIMT | ok |")
    print("|---|---:|---:|---:|---:|---|")
    bad = 0
    for m in ("bcsstk25", "qc2534", "lnsp3937", "dubcova3"):
        for v in ("full", "noidx"):
            tp = "build/wm/m18_%s_%s.bin" % (m, v)
            lg = "build/wm/m18_%s_%s_w8192.txt" % (m, v)
            if not (os.path.exists(tp) and os.path.exists(lg)):
                continue
            j, layout, x_span = layout_of(tp)
            r = trace_requests(tp, layout, x_span)
            d = read_cell(lg)
            a_req = d["req"][0]
            c_simt = d["m15"].get("simt_requests_generated",
                                  d["m15"].get("requests_generated"))
            tot_trace = r["requests_total"]
            simt_trace = r["requests_by_class"][SIMT]
            ok = (a_req == tot_trace) and (c_simt == simt_trace)
            if not ok:
                bad += 1
            print("| %s %s | %d | %d | %s | %d | %s |" % (
                m, v, a_req, tot_trace, c_simt, simt_trace, "yes" if ok else "NO"))
    print("\n%d mismatches" % bad)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "cells":
        cmd_cells(sys.argv[2:])
    elif cmd == "streams":
        cmd_streams(sys.argv[2:])
    elif cmd == "verify":
        cmd_verify(sys.argv[2:])
    elif cmd == "integrity":
        cmd_integrity(sys.argv[2:])
    else:
        raise SystemExit(__doc__)
