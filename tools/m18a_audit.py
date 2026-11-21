#!/usr/bin/env python3
"""M18A: execution-equivalence and index-accounting audit.

Two subcommands:

  equiv  <matrix> --full T --noidx T --mtx M [--max-rows N]
      Reads the two trace binaries and proves the access streams are identical
      except for the column-index region; decodes the index values the engine
      consumes and checks them against the CSR column-index array, then
      recomputes the application output from the decoded values.

  ledger <cell.txt> [...]
      Builds the event ledger from the simulator's own counters: the
      cache_diag per-requester attribution for the hybrid, and the m15/m16
      counter blocks for both arms.

Nothing here recomputes a simulator counter from a byte count.
"""
import json
import os
import re
import struct
import sys

LINE = 64


# --------------------------------------------------------------------------- #
# trace reader
# --------------------------------------------------------------------------- #
def read_trace(path):
    with open(path, "rb") as f:
        blob = f.read()
    magic = blob[:8]
    if magic[:7] != b"M18TRC1":
        raise SystemExit(f"{path}: not an M18TRC1 trace")
    lanes, elem, n_instr, n_acc = struct.unpack_from("<IIQQ", blob, 8)
    off = 8 + 4 + 4 + 8 + 8
    cls = blob[off:off + n_instr]
    off += n_instr
    acc = []
    for i in range(n_acc):
        addr, instr, lane = struct.unpack_from("<QII", blob, off + i * 16)
        acc.append((addr, instr, lane))
    return {"lanes": lanes, "elem": elem, "n_instr": n_instr, "cls": cls, "acc": acc}


def read_mtx_rows(path, max_rows=0):
    """CSR rows (list of column lists), symmetric expanded, exactly as the
    trace builder builds them."""
    nrows = None
    sym = False
    entries = []
    with open(path, "r", errors="replace") as f:
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
                nrows = int(p[0])
                continue
            entries.append((int(p[0]) - 1, int(p[1]) - 1))
    rows = [[] for _ in range(nrows)]
    for i, j in entries:
        rows[i].append(j)
        if sym and i != j:
            rows[j].append(i)
    for r in rows:
        r.sort()
    if max_rows:
        rows = rows[:max_rows]
    return rows


def read_mtx_csr(path, max_rows=0):
    return [j for r in read_mtx_rows(path, max_rows) for j in r]


# --------------------------------------------------------------------------- #
# equivalence
# --------------------------------------------------------------------------- #
def cmd_equiv(argv):
    path = None
    full = noidx = mtx = None
    max_rows = 0
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--full":
            full = argv[i + 1]; i += 2
        elif a == "--noidx":
            noidx = argv[i + 1]; i += 2
        elif a == "--mtx":
            mtx = argv[i + 1]; i += 2
        elif a == "--max-rows":
            max_rows = int(argv[i + 1]); i += 2
        else:
            path = a; i += 1

    tf = read_trace(full)
    tn = read_trace(noidx)
    jf = json.load(open(full + ".json"))
    jn = json.load(open(noidx + ".json"))
    lay = jf["layout"]
    cbase, cend = lay["colidx_base"], lay["colidx_base"] + jf["nnz_full"] * 4
    idxbase, xbase, ewidth = jf["bulk"]["index_base"], jf["bulk"]["data_base"], 0

    def is_colidx(addr):
        return cbase <= addr < cend

    def renumber(t):
        """Instruction ids are positional, so compare structure, not labels."""
        m, out = {}, []
        for addr, instr, lane in t["acc"]:
            if instr not in m:
                m[instr] = len(m)
            out.append((addr, m[instr], lane))
        return out

    cols_f = [a for a in tf["acc"] if is_colidx(a[0])]
    keep_f = [a for a in tf["acc"] if not is_colidx(a[0])]
    cols_n = [a for a in tn["acc"] if is_colidx(a[0])]
    keep_f_r = renumber({"acc": keep_f})
    noidx_r = renumber(tn)

    bulk_f = [a for a in tf["acc"] if tf["cls"][a[1]] == 1]
    bulk_n = [a for a in tn["acc"] if tn["cls"][a[1]] == 1]

    print(f"### {path}\n")
    print("| Property | value |")
    print("|---|---|")
    print(f"| trace lanes / elem bytes | {tf['lanes']} / {tf['elem']} |")
    print(f"| accesses, full trace | {len(tf['acc'])} |")
    print(f"| accesses, noidx trace | {len(tn['acc'])} |")
    print(f"| accesses in the colidx region, full | {len(cols_f)} |")
    print(f"| accesses in the colidx region, noidx | {len(cols_n)} |")
    print(f"| full − noidx accesses | {len(tf['acc']) - len(tn['acc'])} |")
    print(f"| nnz of the trace | {jf['nnz_full']} |")
    print(f"| full minus colidx == noidx (addr, instruction, lane) | "
          f"{'YES' if keep_f_r == noidx_r else 'NO'} |")
    print(f"| bulk-class (engine) elements, full | {len(bulk_f)} |")
    print(f"| bulk-class (engine) elements, noidx | {len(bulk_n)} |")
    print(f"| gather addresses identical (sequence) | "
          f"{'YES' if [a[0] for a in bulk_f] == [a[0] for a in bulk_n] else 'NO'} |")

    # The engine reads index[j] from memory and forms data_base + index[j]*data_bytes.
    # With data_base = 0 and elem = 8, index[j] = addr/8 for the bulk element -- the
    # value the descriptor must contain for the gather to reproduce the trace.
    decoded = [(a[0] - xbase) // tf["elem"] for a in bulk_f]
    print(f"| decoded index[0..3] | {decoded[:4]} |")
    print(f"| index value domain | min {min(decoded)}, max {max(decoded)} |")

    if mtx:
        csr = read_mtx_csr(mtx, max_rows)
        n = min(len(csr), len(decoded))
        same = csr[:n] == decoded[:n] and len(csr) == len(decoded)
        print(f"| CSR colidx entries | {len(csr)} |")
        print(f"| **decoded index array == CSR colidx** | **{'YES' if same else 'NO'}** |")
        if not same:
            bad = next((k for k in range(n) if csr[k] != decoded[k]), None)
            print(f"| first divergence at | {bad} |")

    # Application output, recomputed from the decoded index values alone.
    if mtx:
        x = [1.0 + (i % 7) * 0.5 for i in range(jf["cols"])]
        rows = read_mtx_rows(mtx, max_rows)
        chk = 0.0
        pos = 0
        for r in rows:
            ln = len(r)
            chk += sum(x[u] for u in decoded[pos:pos + ln])
            pos += ln
        print(f"| output from decoded index values | {chk:.1f} |")
        print(f"| output recorded by the trace builder | {jf['app_output_checksum']:.1f} |")
        print(f"| **output matches** | **{'YES' if abs(chk - jf['app_output_checksum']) < 1e-6 else 'NO'}** |")
    print()


# --------------------------------------------------------------------------- #
# ledger
# --------------------------------------------------------------------------- #
KV = re.compile(r"^\s+m1[56] ([A-Za-z0-9_]+)=(-?\d+)$")
# A three-number metrics row. The name may contain digits ("L1 reads"), but its
# LAST token may not: otherwise a per-requester row ("simt 187062 6250 ...")
# would be read as a metric whose name swallowed its own first ten numbers.
ROW = re.compile(r"^  (\S.*?)\s+(\d+)\s+(\d+)\s+(\d+)\s*$")
CD = re.compile(r"^  (simt|e-idx|e-tgt|walk)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$")


def parse_cell(p):
    d = {"cd": {}, "path": p}
    for line in open(p):
        s = line.rstrip()
        m = KV.match(s)
        if m:
            d[m.group(1)] = int(m.group(2))
            continue
        m = CD.match(s)
        if m:
            d["cd"][m.group(1)] = {
                "l1r": int(m.group(2)), "l1h": int(m.group(3)), "l1m": int(m.group(4)),
                "l2r": int(m.group(5)), "l2h": int(m.group(6)), "l2m": int(m.group(7)),
                "l2ev": int(m.group(8)), "issued": int(m.group(12)), "done": int(m.group(13)),
            }
            continue
        m = ROW.match(s)
        if m and not re.search(r"\d", m.group(1).split()[-1]):
            k = m.group(1).strip().replace(" ", "_")
            d["A_" + k] = int(m.group(2))
            d["C_" + k] = int(m.group(4))
            continue
        m = CD.match(s)  # unreachable: kept for clarity of the fall-through order
        if m:
            d["cd"][m.group(1)] = {
                "l1r": int(m.group(2)), "l1h": int(m.group(3)), "l1m": int(m.group(4)),
                "l2r": int(m.group(5)), "l2h": int(m.group(6)), "l2m": int(m.group(7)),
                "l2ev": int(m.group(8)), "issued": int(m.group(12)), "done": int(m.group(13)),
            }
            continue
        m = re.search(r"first index request (\d+), descriptor fetch done (\d+), "
                      r"first admit (\d+), first target issue (\d+)", s)
        if m:
            d["first_idx"] = int(m.group(1)); d["desc_done"] = int(m.group(2))
            d["first_tgt"] = int(m.group(4))
    return d


def cmd_ledger(argv):
    for p in argv:
        c = parse_cell(p)
        name = os.path.basename(p)[4:-4]
        print(f"\n## `{name}`\n")
        a, cc = c.get("A_cycles", 0), c.get("C_cycles", 0)
        print(f"cycles A={a} C={cc}" + (f" S={a/cc:.3f}x" if cc else ""))
        print()
        hdr = ("| client | L1 rd / hit / miss | L2 rd / hit / miss | DRAM (L2 miss) | issued | done |\n"
               "|---|---|---|---|---:|---:|")
        print(hdr)
        for k in ("simt", "e-idx", "e-tgt", "walk"):
            if k in c["cd"]:
                r = c["cd"][k]
                print(f"| {k} | {r['l1r']} / {r['l1h']} / {r['l1m']} "
                      f"| {r['l2r']} / {r['l2h']} / {r['l2m']} | {r['l2m']} "
                      f"| {r['issued']} | {r['done']} |")
        tot_l2m = sum(r["l2m"] for r in c["cd"].values())
        print(f"| **sum** | | | **{tot_l2m}** (= C DRAM reads {c.get('C_DRAM_reads','-')}) | | |")
        print()
        keys = ["bulk_desc_requests", "bulk_idx_lines_req", "bulk_idx_elems_req",
                "bulk_idx_elems_done", "bulk_tgt_addr_gen", "bulk_tgt_req_gen",
                "bulk_tgt_req_issued", "bulk_dedup_merged", "bulk_lines_issued",
                "bulk_ag_cycles", "bulk_org_cycles", "bulk_fanout_cycles",
                "bulk_slot_stall", "bulk_idx_wait_stall", "bulk_buf_full_stall",
                "bulk_win_full_stall", "bulk_win_occ_max", "bulk_elems_total",
                "bulk_elems_done", "bulk_addr_mismatch", "bulk_addr_faults"]
        print("| engine counter | value |")
        print("|---|---:|")
        for k in keys:
            if k in c:
                print(f"| `{k}` | {c[k]} |")
        for k in ("first_idx", "desc_done", "first_tgt"):
            if k in c:
                print(f"| `{k}` | {c[k]} |")
        print()
        print("| arm A counter | value |")
        print("|---|---:|")
        for k in ("A_L1_reads", "A_L1_hits", "A_L1_misses", "A_L2_reads", "A_L2_hits",
                  "A_L2_misses", "A_DRAM_reads", "A_tlb0_misses", "A_walks",
                  "A_pte_fetches", "A_inflight_max"):
            if k in c:
                print(f"| `{k}` | {c[k]} |")
        print()


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit("usage: m18a_audit.py {equiv|ledger} ...")
    if sys.argv[1] == "equiv":
        cmd_equiv(sys.argv[2:])
    elif sys.argv[1] == "ledger":
        cmd_ledger(sys.argv[2:])
    else:
        raise SystemExit("unknown subcommand")
