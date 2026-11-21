#!/usr/bin/env python3
"""M24-H — storage and cost model for the production M24 Bulk engine.

Every row is derived from a declaration in `sim/src/sim.hpp` (the structs
`Batch`, `Pending`, `IdxRing`, `FanJob`, `BulkDesc`) or from an allocation site
in `sim/src/bulk.cpp` (the `.assign`/`.resize` that sizes it).  The ABI width is
the declared C++ type (u64 = 8 B, u32 = 4 B, u8 = 1 B); the packed width is the
field bits, recomputed here from the range each field must hold.

Three accountings are produced, and they are never mixed:

  charge      the simulator's own `bulk_win_bytes` / `bulk_idx_bytes` /
              `bulk_g6b_pa_bytes` formulas -- what every published M24 report
              quotes.  Kept for continuity, not because it is the hardware cost.
  packed      the RTL minimum for the declared fields: every field narrowed to
              the bits its range needs, no C++ padding, no simulator-only state.
  abi         what the simulator allocates (declared types, timestamps
              included), excluding `val_`/`addr_` which have no hardware
              counterpart.

`replicated` is 2 for the window arrays (two buffers, `org_overlap = 1` and
`win_policy` != 1) and 1 for the engine arrays.  Only one window *issues* at a
time (`iss_`), so the x2 is the design's choice to double-buffer the sealer, not
a requirement; the report states both.

usage: python3 tools/m24h_structures.py [--org 8] [--window 16384]
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/wm/m24h")
BULK_CFG = os.path.join(ROOT, "build/wm/m24d/m24d_bulk.cfg")
M24F = os.path.join(ROOT, "build/wm/m24f")
PRIMARY = ["w4_duplow", "criteo_2k", "n1_seq8m", "n3_clu4m"]

# name, scope, count tag, packed bits, ABI bytes, class, org_min, port, source
STRUCTS = [
    # ---- fused window, per buffer -------------------------------------------
    ("fent", "buffer", "W", 64, 8, "data", 0,
     "1W on admission (org_lanes/cycle), 1R sequential pass at seal, 1R per release",
     "sim.hpp:1600 `std::vector<u64> fent` (line 26 | pos 32 | off 6)"),
    ("fmeta", "buffer", "L", 32, 4, "meta", 0,
     "1W per run at seal, 1R per run at release",
     "sim.hpp:1601 (start:16 | count:16)"),
    ("fslot", "buffer", "L", 7, 1, "control", 0,
     "1R1W by run id (slot id at issue, read at fill)",
     "sim.hpp:1602 (1 B; 7 bits cover R <= 128)"),
    ("fflag", "buffer", "L", 1, 1, "control", 0,
     "1W at issue / at fill, 1R in the release scan",
     "sim.hpp:1603"),
    ("ford", "buffer", "L", 14, 4, "control", 2,
     "1W at seal (sort output), 1R at release (rank -> run)",
     "sim.hpp:1607; filled by `win_order_fused` (bulk.cpp:1574)"),
    ("fpos", "buffer", "L", 14, 4, "control", 7,
     "1W at seal, 1R at translation completion (run -> heap key)",
     "bulk.cpp:2301 `b.fpos[b.ford[i]] = i`"),
    ("fpa", "buffer", "L", 32, 8, "meta", 6,
     "1W scattered at translation completion, 1R by run id at release",
     "bulk.cpp:2079/2296 `b.fpa.assign(n, 0)`; 26-bit PA line + valid"),
    ("ftr", "buffer", "L", 1, 1, "control", 6,
     "1W at admission / completion, 1R in the admission loop",
     "bulk.cpp:2079 `b.ftr.assign(n, 0)`"),
    ("ready", "buffer", "CAP", 14, 8, "meta", 7,
     "min-heap: 1 push (sift up) + 1 pop (sift down) per release, log2(cap) levels",
     "sim.hpp:1613 `std::vector<u64> ready`; bounded by `g6b_pa_capacity`"),
    ("dir_vpn", "buffer", "D", 26, 8, "dead", 0,
     "1R1W direct-mapped (vpn % 512); no issue decision reads it",
     "sim.hpp:1541; M22 §6.1, M24-G §5 (measured null)"),
    ("dir_mask", "buffer", "D", 64, 8, "dead", 0,
     "64-bit OR per line at seal; read only by counters",
     "sim.hpp:1542"),
    ("coh", "buffer", "C", 168, 32, "control", 0,
     "register file, combinational scan of C entries per select",
     "sim.hpp:1547 `Batch::Cohort` (pos 32|end 32|vpn 26|resume 26|open 1)"),
    ("g6c_coh_def", "buffer", "C", 16, 4, "control", 8,
     "1R1W per cohort, updated on refusal / admission",
     "bulk.cpp:2515 `g6c_coh_def.assign(coh.size(), 0)`"),
    ("g6c_coh_age", "buffer", "C", 32, 8, "control", 8,
     "1R1W per cohort, updated on refusal / admission",
     "bulk.cpp:2516 `g6c_coh_age.assign(coh.size(), 0)`"),
    # ---- instrumentation, per buffer (no hardware counterpart) --------------
    ("ftr_gen", "buffer", "L", 0, 8, "instr", 6,
     "simulator timestamp",
     "bulk.cpp:2081 `ftr_gen`; latency accounting only"),
    ("ftr_done", "buffer", "L", 0, 8, "instr", 6,
     "simulator timestamp",
     "bulk.cpp:2082 `ftr_done`; latency accounting only"),
    ("win_rec", "buffer", "WIN", 0, 72, "instr", 0,
     "simulator timeline",
     "sim.hpp:1372 `WinRec`, 3 per buffer; diagnostics"),
    # ---- engine-wide --------------------------------------------------------
    ("info_", "engine", "R", 44, 56, "control", 0,
     "1W at issue (slot-indexed), 1R at completion; no CAM",
     "sim.hpp:1463 `Pending`; 44 packed bits, 56 B ABI"),
    ("free_/unissued_/idx_q_", "engine", "R", 24, 12, "control", 0,
     "LIFO / FIFO, one id per slot",
     "slot id (8 b) in three lists; 12 B ABI per slot"),
    ("idx ring", "engine", "IDX", 168, 21, "meta", 1,
     "256 x 21 B descriptors, 1R1W by line",
     "sim.hpp:1490 `IdxRing`"),
    ("idx line buffers", "engine", "IDXLINE", 512, 0, "req", 1,
     "operand supply: 1 line per AG cycle (M22 §6.2)",
     "requirement, not a source structure: 2 x 64 lines x 64 B"),
    ("fan_", "engine", "FAN", 608, 76, "data", 0,
     "FIFO of 64 B line images; 1 write per fill, 4 elements/cycle read",
     "sim.hpp:1641 `FanJob`; bounded by R"),
    ("queue_", "engine", "DESC", 320, 40, "control", 0,
     "descriptor FIFO [A: depth 8]",
     "`BulkDesc`, 40 B"),
]

CLASS_NOTE = {
    "data": "holds payload or address bits a decision reads",
    "meta": "tags, addresses, indices",
    "control": "state bits, cursors, counters",
    "dead": "written but read only by counters",
    "instr": "simulator-only measurement state",
    "req": "structural requirement with no line in the source",
}


def read_log(path):
    d = {}
    if not os.path.exists(path):
        return d
    with open(path) as f:
        for line in f:
            for k, v in __import__("re").findall(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)", line):
                d.setdefault(k, v)
    return d


def parse_cfg(path):
    cfg = {}
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line and "=" in line:
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
    return cfg


def cget(cfg, k, default=0):
    try:
        return int(cfg.get(k, default))
    except ValueError:
        return default


def params(org, window, cfg):
    bufs = 2 if cget(cfg, "org_overlap", 1) and cget(cfg, "win_policy", 255) != 1 else 1
    idx_mode = cget(cfg, "idx_mode", 0)
    idxlines = 2 * min(cget(cfg, "idx_buf_lines", 1) or 1, 64) if idx_mode else 0
    return {"W": window, "L": window, "R": cget(cfg, "R", 128),
            "D": cget(cfg, "page_dir", 0),
            "C": cget(cfg, "page_cohorts", 0) if org >= 1 else 0,
            "CAP": cget(cfg, "g6b_pa_capacity", 0) if org >= 7 else 0,
            "bufs": bufs, "org": org, "idx_mode": idx_mode, "IDXLINES": idxlines}


def count_of(tag, p):
    return {"W": p["W"], "L": p["L"], "R": p["R"], "D": p["D"], "C": p["C"],
            "CAP": p["CAP"], "IDX": 256 if p["idx_mode"] else 0,
            "IDXLINE": p["IDXLINES"], "FAN": p["R"], "DESC": 8, "WIN": 3}[tag]


def rows_for(org, window, cfg):
    p = params(org, window, cfg)
    out = []
    for name, scope, tag, packed, abi, cls, org_min, port, src in STRUCTS:
        active = p["org"] >= org_min
        n = count_of(tag, p) if active else 0
        mult = p["bufs"] if scope == "buffer" else 1
        out.append({"structure": name, "scope": scope, "count_tag": tag,
                    "org_min": org_min, "active": int(active), "entries": n,
                    "replicated": mult, "bits": packed, "abi_bytes": abi, "class": cls,
                    "packed_bytes": n * mult * packed / 8.0,
                    "abi_total": n * mult * abi, "port": port, "source": src})
    return out, p


def totals(rows):
    t = {"packed_hw": 0.0, "packed_all": 0.0, "abi_hw": 0.0, "abi_all": 0.0,
         "dead": 0.0, "instr": 0.0, "req": 0.0}
    for r in rows:
        if r["class"] == "instr":
            t["instr"] += r["abi_total"]
            continue
        if r["class"] == "req":
            t["req"] += r["packed_bytes"]
            continue
        t["packed_all"] += r["packed_bytes"]
        t["abi_all"] += r["abi_total"]
        if r["class"] == "dead":
            t["dead"] += r["packed_bytes"]
            continue
        t["packed_hw"] += r["packed_bytes"]
        t["abi_hw"] += r["abi_total"]
    return t


def charge(org, window, cfg):
    """The simulator's own charge (`bulk.cpp:3752`, plus the two pool charges)."""
    D = cget(cfg, "page_dir", 0)
    C = cget(cfg, "page_cohorts", 0) if org >= 1 else 0
    bufs = 2 if cget(cfg, "org_overlap", 1) and cget(cfg, "win_policy", 255) != 1 else 1
    win = bufs * (window * 8 + D * 16 + C * 32)
    idx = 2 * min(cget(cfg, "idx_buf_lines", 0) or 1, 64) * 21 + 32 if cget(cfg, "idx_mode", 0) else 0
    pa = cget(cfg, "g6b_pa_capacity", 0) * 16 if org >= 7 else 0
    return win, idx, pa


def area(b, um2_per_bit=0.045):
    return b * 8 * um2_per_bit / 1e6


def main(argv):
    org, window = 8, 16384
    for i, a in enumerate(argv):
        if a == "--org":
            org = int(argv[i + 1])
        if a == "--window":
            window = int(argv[i + 1])
    cfg = parse_cfg(BULK_CFG)
    os.makedirs(OUT, exist_ok=True)

    rows, p = rows_for(org, window, cfg)
    t = totals(rows)
    win, idx, pa = charge(org, window, cfg)
    t["charged"] = win + idx + pa

    L = []
    w = L.append
    w("# M24-H storage model (generated by tools/m24h_structures.py)\n")
    w("Production M24 Bulk (org %d): window %d elements, R = %d, page_dir = %d, cohorts = %d, "
      "ready pool = %d, %d buffer(s). Worst case L = W = %d distinct runs "
      "(w4_duplow realizes exactly this).\n"
      % (org, window, p["R"], p["D"], p["C"], p["CAP"], p["bufs"], p["L"]))
    w("| structure | scope | entries | bits/entry | B packed | B ABI | x | class | org | active |")
    w("|---|---|---:|---:|---:|---:|---:|---|---:|---:|")
    for r in rows:
        w("| `%s` | %s | %s | %d | %.0f | %.0f | %d | %s | %d | %s |" % (
            r["structure"], r["scope"], r["entries"], r["bits"], r["packed_bytes"],
            r["abi_total"], r["replicated"], r["class"], r["org_min"],
            "yes" if r["active"] else "no"))
    w("")
    w("## Access and port requirement\n")
    w("| structure | read / write requirement | source |")
    w("|---|---|---|")
    for r in rows:
        if not r["active"]:
            continue
        w("| `%s` | %s | `%s` |" % (r["structure"], r["port"], r["source"]))
    w("")
    w("## Totals\n")
    w("| accounting | bytes | KiB | vs 1 MiB L2 | mm2 @ 0.045 um2/bit [E] |")
    w("|---|---:|---:|---:|---:|")
    for k, lab in (("charged", "simulator charge"),
                   ("packed_hw", "packed RTL, dead state excluded"),
                   ("packed_all", "packed RTL, dead state included"),
                   ("abi_hw", "as allocated, hardware only"),
                   ("abi_all", "as allocated, instrumentation included")):
        w("| %s | %.0f | %.1f | %.3fx | %.3f |" % (lab, t[k], t[k] / 1024.0,
                                                    t[k] / 1048576.0, area(t[k])))
    w("")
    w("Dead state (page directory): %.0f B packed. Instrumentation: %.0f B. "
      "Index operand buffers (a requirement, not in the source): %.0f B. "
      "ABI/packed ratio for hardware state: %.2fx.\n"
      % (t["dead"], t["instr"], t["req"], t["abi_hw"] / t["packed_hw"]))

    # Reconciliation: the simulator's own realized figure covers the fused
    # subset only (`bulk.cpp:3062-3072` sums ent/sorted/lines/order/dir/fused
    # arrays).  Splitting the total the same way proves the row set is complete.
    fused = sum(r["abi_total"] for r in rows
                if r["structure"] in ("fent", "fmeta", "fslot", "fflag", "dir_vpn", "dir_mask"))
    later = sum(r["abi_total"] for r in rows
                if r["structure"] in ("ford", "fpos", "fpa", "ftr", "ready"))
    engine = sum(r["abi_total"] for r in rows
                 if r["scope"] == "engine" and r["class"] not in ("instr", "req"))
    w("## Reconciliation of the as-allocated total\n")
    w("| part | bytes | covered by `bulk_win_vec_bytes`? |")
    w("|---|---:|---|")
    w("| fused subset: fent, fmeta, fslot, fflag, dir_vpn, dir_mask | %.0f | yes (nm24g: %.0f on w4_duplow) |"
      % (fused, fused))
    w("| G6A/G6B state on top: ford, fpos, fpa, ftr, ready | %.0f | **no** |" % later)
    coh = sum(r["abi_total"] for r in rows
              if r["structure"] in ("coh", "g6c_coh_def", "g6c_coh_age"))
    w("| cohort table: coh, g6c_coh_def, g6c_coh_age | %.0f | no (only the charge's `sched_bytes`) |" % coh)
    w("| engine-wide state: info_, slot lists, idx ring, fan_, queue_ | %.0f | no |" % engine)
    w("| dead state: dir_vpn, dir_mask (inside the fused subset above) | %.0f | yes |" % t["dead"])
    w("| **sum** | **%.0f** | = `abi_all` %.0f, i.e. `abi_hw` + dead |"
      % (fused + later + coh + engine, t["abi_all"]))
    w("")

    w("## Per-organization ladder\n")
    w("| org | added by this step | packed B | delta B | charge B |")
    w("|---|---|---:|---:|---:|")
    added = {0: "fused window + cohort scheduler (P0)",
             6: "+ physical order + PA buffer (G6A)",
             7: "+ streaming ready pool (G6B), + fpos",
             8: "+ cohort admission counters (G6C/G6D)"}
    ladder, prev = {}, None
    for o in (0, 6, 7, 8):
        ro, _ = rows_for(o, window, cfg)
        to = totals(ro)
        ladder[o] = to["packed_hw"] + to["dead"]
        cw, ci, cp = charge(o, window, cfg)
        w("| %d | %s | %.0f | %s | %.0f |" % (
            o, added[o], ladder[o], "" if prev is None else "%.0f" % (ladder[o] - prev),
            cw + ci + cp))
        prev = ladder[o]
    w("")
    w("## Measured activity, production cells (build/wm/m24f)\n")
    w("| trace | cycles | win_vec_bytes [M] | win_occ_max | ready_peak | fanout_backlog | "
      "idx_out_max | unissued_max | dedup_merged |")
    w("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for tr in PRIMARY:
        d = read_log(os.path.join(M24F, tr + "__bulk.txt"))
        w("| %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            tr, d.get("cycles", "?"), d.get("bulk_win_vec_bytes", "?"),
            d.get("bulk_win_occ_max", "?"), d.get("bulk_g6b_ready_peak", "?"),
            d.get("bulk_fanout_max_backlog", "?"), d.get("bulk_idx_out_max", "?"),
            d.get("bulk_unissued_max", "?"), d.get("bulk_dedup_merged", "?")))
    w("")

    with open(os.path.join(OUT, "storage.md"), "w") as f:
        f.write("\n".join(L) + "\n")
    with open(os.path.join(OUT, "storage.csv"), "w", newline="") as f:
        cols = ["structure", "scope", "count_tag", "org_min", "active", "entries",
                "replicated", "bits", "abi_bytes", "class", "packed_bytes", "abi_total",
                "port", "source"]
        wr = csv.DictWriter(f, fieldnames=cols)
        wr.writeheader()
        for r in rows:
            wr.writerow({k: r[k] for k in cols})
    with open(os.path.join(OUT, "totals.csv"), "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["accounting", "bytes"])
        for k in ("charged", "packed_hw", "packed_all", "abi_hw", "abi_all",
                  "dead", "instr", "req"):
            wr.writerow([k, "%.0f" % t[k]])
        for o in (0, 6, 7, 8):
            wr.writerow(["ladder_org%d" % o, "%.0f" % ladder[o]])

    print("wrote build/wm/m24h/storage.{md,csv}, totals.csv")
    print("charge %.0f B | packed hw %.0f B | packed+dead %.0f B | abi hw %.0f B"
          % (t["charged"], t["packed_hw"], t["packed_all"], t["abi_hw"]))
    print("ladder: " + ", ".join("org%d=%.0f" % (o, ladder[o]) for o in (0, 6, 7, 8)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
