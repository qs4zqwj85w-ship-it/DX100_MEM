#!/usr/bin/env python3
"""M24-B: recompute the report's quoted figures from the cell logs and check that
each one appears in the document. A mismatch is either a doc typo or a stale log;
both must be resolved before the report is delivered."""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "wm", "m24b")
DOC = os.path.join(ROOT, "docs", "m24-b-gpu-style-hbm-memory-system.md")


def load(name):
    d = {}
    with open(os.path.join(LOG, name)) as f:
        for line in f:
            if "=" in line:
                k, _, rest = line.partition("=")
                v = rest.split()[0] if rest.split() else ""
                try:
                    d[k.strip()] = int(v)
                except ValueError:
                    pass
    return d


def sp(n, sep=" "):
    return f"{n:,}".replace(",", sep)


doc = open(DOC, encoding="utf-8").read()
# normalise: unicode minus, thin spaces, unicode multiplication
norm = doc.replace("\u2212", "-").replace("\u00d7", "x").replace("\u2009", " ")

missing = []
checked = 0


def want(tok):
    global checked
    checked += 1
    t = tok.replace("\u2212", "-").replace("\u00d7", "x")
    if t not in norm:
        missing.append((tok, tok))


def cell(wl, cell, bg, mp, pk):
    b = 16 if bg == 4 else 32
    return load(f"m24b_{wl}_{cell}_{b}banks_map{mp}_p{pk}k.txt")


def cy(wl, c, bg, mp, pk):
    return cell(wl, c, bg, mp, pk)["cycles"]


def pct(a, b):
    return "%+.2f %%" % (100.0 * (a - b) / b)


# ---- four-way tables ------------------------------------------------------
for (wl, bg, mp, pk) in [("orkut", 4, 0, 4), ("orkut", 8, 0, 4), ("orkut", 4, 1, 4),
                         ("orkut", 4, 2, 4), ("orkut", 4, 3, 4),
                         ("livejournal", 4, 0, 4), ("livejournal", 8, 0, 4),
                         ("livejournal", 4, 1, 4), ("livejournal", 8, 1, 4),
                         ("ogbn", 4, 1, 4), ("ogbn", 4, 1, 2048),
                         ("orkut", 4, 1, 64), ("orkut", 4, 1, 2048)]:
    a = cy(wl, "A", bg, mp, pk)
    for c in "ABCD":
        try:
            want(sp(cy(wl, c, bg, mp, pk)))
        except KeyError:
            continue
    b = cy(wl, "B", bg, mp, pk)
    d = cy(wl, "D", bg, mp, pk)
    want(pct(b, a).replace("+", "").replace("-", "\u2212", 1) if b < a else pct(b, a))

# ---- DRAM / channel census ------------------------------------------------
o = cell("orkut", "A", 4, 0, 4)
want(sp(o["hbm_wait_sum"]))
want("%.1f %%" % (100.0 * o["hbm_hits"] / o["hbm_reads"]))
o = cell("orkut", "B", 4, 0, 4)
want(sp(o["hbm_wait_sum"]))
o = cell("orkut", "A", 4, 1, 4)
want(sp(o["hbm_wait_sum"]))
o = cell("orkut", "B", 4, 1, 4)
want(sp(o["hbm_wait_sum"]))
o = cell("orkut", "D", 4, 1, 4)
want(sp(o["hbm_wait_sum"]))

# ---- page-size table ------------------------------------------------------
for pk in (4, 64, 2048):
    a = cell("orkut", "B", 4, 1, pk)
    want(sp(a["cycles"]))
    want("%.1f" % (100.0 * a["hbm_hits"] / a["hbm_reads"]))
    want("%.3f" % (a["hbm_row_acts_sum"] / a["hbm_reads"]))
    want(sp(a["walks"]))
    want(sp(a["l2tlb_hits"]))

# ---- 16 vs 32 banks delta -------------------------------------------------
for (wl, mp, pk, c) in [("orkut", 0, 4, "D"), ("orkut", 0, 4, "A"),
                        ("livejournal", 0, 4, "D"), ("orkut", 1, 4, "D")]:
    x = cy(wl, c, 4, mp, pk)
    y = cy(wl, c, 8, mp, pk)
    want("%+.2f %%" % (100.0 * (y - x) / x))

# ---- anchors --------------------------------------------------------------
want("4 924 checks")
want("6 130 494")
want("644 142")
want("389 893")

print(f"{checked} figures checked, {len(missing)} missing")
for m in missing:
    print("   missing:", m[0])
raise SystemExit(1 if missing else 0)
