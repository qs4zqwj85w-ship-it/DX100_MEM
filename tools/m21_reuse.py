#!/usr/bin/env python3
"""M21: offline structure metrics for one M18TRC1 trace.

Computes, per stream (index array / target vector / row pointers / values):
  * bytes and accesses,
  * line requests at the trace's lane width (same rule as the coalescer: one
    request per distinct 64 B line inside one instruction),
  * unique lines and unique 4 KiB pages,
  * duplicate rates at word and line granularity,
  * the target working-set size,
  * a target reuse-distance histogram (access-granular stack distance, exact for
    a fully associative buffer counted in target accesses),
  * the fraction of target accesses whose line was touched within the last W
    target accesses -- the quantity a bulk window of W elements can exploit.

Usage: m21_reuse.py <trace.bin> [--window 8192] [--json]
"""
import json
import struct
import sys
from collections import Counter

LINE = 64
PAGE = 4096


def load(path):
    with open(path, "rb") as f:
        magic = f.read(8)
        assert magic == b"M18TRC1\x00", magic
        lanes, elem, ncls, nacc = struct.unpack("<IIQQ", f.read(24))
        cls = f.read(ncls)
        recs = []
        for _ in range(nacc):
            addr, instr, lane = struct.unpack("<QII", f.read(16))
            recs.append((addr, instr, lane))
    return lanes, elem, cls, recs


def main():
    path = sys.argv[1]
    win = 8192
    as_json = "--json" in sys.argv
    if "--window" in sys.argv:
        win = int(sys.argv[sys.argv.index("--window") + 1])
    meta = json.load(open(path + ".json"))
    lay = meta["layout"]
    lanes, elem, cls, recs = load(path)
    x_end, rowptr, colidx_base, value_base = (
        lay["x_base"], lay["rowptr_base"], lay["colidx_base"], lay["value_base"])

    def stream_of(addr):
        if addr < rowptr:
            return "target"
        if addr < colidx_base:
            return "rowptr"
        if addr < value_base:
            return "index"
        return "value"

    per = {k: {"acc": 0, "lines": set(), "pages": set(), "words": set(),
               "instr_lines": 0, "last_instr": -1, "seen": set()}
           for k in ("target", "index", "value", "rowptr")}
    hist = Counter()
    last = {}
    reuse_win = 0
    tgt_seq = 0
    for addr, instr, lane in recs:
        s = stream_of(addr)
        d = per[s]
        d["acc"] += 1
        ln = addr // LINE
        d["lines"].add(ln)
        d["pages"].add(addr // PAGE)
        d["words"].add(addr)
        if s == "target":
            prev = last.get(ln)
            if prev is None:
                hist["cold"] += 1
            else:
                dist = tgt_seq - prev
                if dist <= 16:
                    hist["1-16"] += 1
                elif dist <= 128:
                    hist["17-128"] += 1
                elif dist <= 1024:
                    hist["129-1K"] += 1
                elif dist <= 8192:
                    hist["1K-8K"] += 1
                elif dist <= 65536:
                    hist["8K-64K"] += 1
                else:
                    hist[">64K"] += 1
                if dist <= win:
                    reuse_win += 1
            last[ln] = tgt_seq
            tgt_seq += 1
    # line requests per instruction: distinct lines inside the instruction
    cur_instr, cur_lines = -1, set()
    for addr, instr, lane in recs:
        if instr != cur_instr:
            if cur_instr >= 0:
                per[stream_of(prev_addr)]["instr_lines"] += len(cur_lines)
            cur_instr, cur_lines = instr, set()
        cur_lines.add(addr // LINE)
        prev_addr = addr
    if cur_instr >= 0:
        per[stream_of(prev_addr)]["instr_lines"] += len(cur_lines)

    out = {"trace": path.split("/")[-1], "lanes": lanes,
           "pattern": meta.get("pattern"), "seed": meta.get("seed"),
           "ops": meta.get("logical_ops") or meta.get("nnz_full"),
           "matrix": meta.get("dataset"), "streams": {}, "structures": {}}
    for k, d in per.items():
        if not d["acc"]:
            continue
        out["streams"][k] = {
            "accesses": d["acc"],
            "bytes": d["acc"] * (8 if k in ("target", "value") else 4),
            "requests": d["instr_lines"],
            "unique_lines": len(d["lines"]),
            "unique_pages": len(d["pages"]),
            "dup_line_frac": 1.0 - len(d["lines"]) / d["acc"],
            "dup_word_frac": 1.0 - len(d["words"]) / d["acc"],
        }
    t = per["target"]
    n_t = t["acc"]
    out["structures"] = {
        "target_unique_lines": len(t["lines"]),
        "target_unique_pages": len(t["pages"]),
        "target_working_set_bytes": len(t["lines"]) * LINE,
        "target_dup_line_frac": 1.0 - len(t["lines"]) / n_t,
        "target_dup_word_frac": 1.0 - len(t["words"]) / n_t,
        "target_reuse_within_window": reuse_win / n_t,
        "reuse_distance_hist": dict(hist),
        "index_unique_lines": len(per["index"]["lines"]),
        "index_requests_over_unique": (per["index"]["instr_lines"] /
                                       max(1, len(per["index"]["lines"]))),
    }
    if as_json:
        print(json.dumps(out, indent=1))
        return
    print("| metric | %s |" % out["trace"])
    print("|---|---:|")
    for k in ("index", "target", "value", "rowptr"):
        if k in out["streams"]:
            s = out["streams"][k]
            print("| %s: accesses / bytes | %d / %d |" % (k, s["accesses"], s["bytes"]))
            print("| %s: requests (l%d) / unique lines | %d / %d |" % (
                k, lanes, s["requests"], s["unique_lines"]))
            print("| %s: dup rate (line / word) | %.3f / %.3f |" % (
                k, s["dup_line_frac"], s["dup_word_frac"]))
    st = out["structures"]
    print("| target unique pages | %d |" % st["target_unique_pages"])
    print("| target working set | %d B (%d lines) |" % (
        st["target_working_set_bytes"], st["target_unique_lines"]))
    print("| target reuse within %d accesses | %.3f |" % (win, st["target_reuse_within_window"]))
    print("| reuse-distance hist | %s |" % json.dumps(st["reuse_distance_hist"]))


if __name__ == "__main__":
    main()
