#!/usr/bin/env python3
"""M23-E: extract the kappa sweep from the simulator logs and draw the figures.

Reads build/wm/m23e_{m0,dx8k,dx16k,dx32k}_k<kappa>_s<seed>.txt (P=512, the
original M17 generator) and the footprint-matched axis
build/wm/m23e_p8k_<cfg>_k<kappa>.txt (P=8192), writes
build/wm/m23e_sweep.csv and docs/figs/m23e-*.svg.

No plotting library: the SVGs are written directly so the figures are
reproducible on a machine with only numpy/python.
"""
import glob
import os
import re
import statistics as st

CFGS = ["m0", "dx8k", "dx16k", "dx32k"]
COL = {"m0": "#444444", "dx8k": "#1f77b4", "dx16k": "#d62728", "dx32k": "#2ca02c"}
LBL = {"m0": "M0 (SIMT only)", "dx8k": "DX-100 2×8K", "dx16k": "DX-100 2×16K",
       "dx32k": "DX-100 2×32K"}
KS = [i / 10 for i in range(11)]
NUM = ("cycles walks pte_fetches l1_hits l1_misses l2_hits l2_misses dram_reads "
       "dram_row_hits dram_row_conflicts dram_row_acts_sum inflight_max "
       "bulk_elems bulk_instr simt_instr bulk_win_occ_max bulk_win_size_mean "
       "bulk_fanout_cycles dram_bytes").split()


def parse(path):
    d = {}
    for line in open(path):
        s = line.strip()
        if "=" not in s:
            continue
        for part in s.split():
            if "=" not in part:
                continue
            k, v = part.split("=", 1)
            if k == "kappa":
                d.setdefault("kappa_real", v)   # realized contiguity fraction
            elif (k in NUM or k in ("rho_line", "lines", "pages",
                                    "conservation", "bulk_addr_mismatch")) and k not in d:
                d[k] = v
    return d


def num(d, k, default=0.0):
    try:
        return float(d.get(k, default))
    except ValueError:
        return default


def load():
    rows = []
    for f in glob.glob("build/wm/m23e_*.txt"):
        m = re.match(r"m23e_(p8k_)?(m0|dx8k|dx16k|dx32k)_k([0-9.]+)(?:_s(\d))?\.txt",
                     os.path.basename(f))
        side, cfg, k = m.group(1) is not None, m.group(2), float(m.group(3))
        seed = int(m.group(4) or 1)
        d = parse(f)
        rows.append(dict(side=side, cfg=cfg, kappa=k, seed=seed, **d))
    return rows


def agg(rows, side, cfg, k):
    sel = [r for r in rows if r["side"] == side and r["cfg"] == cfg
           and abs(r["kappa"] - k) < 1e-9]
    if not sel:
        return None
    a = dict(n=len(sel))
    for key in NUM:
        a[key] = st.mean(num(r, key) for r in sel)
    a["cycles_min"] = min(num(r, "cycles") for r in sel)
    a["cycles_max"] = max(num(r, "cycles") for r in sel)
    a["kappa_real"] = st.mean(float(r.get("kappa_real", 0)) for r in sel)
    a["rho_line"] = st.mean(num(r, "rho_line") for r in sel)
    a["lines"] = st.mean(num(r, "lines") for r in sel)
    a["pages"] = st.mean(num(r, "pages") for r in sel)
    a["rowhit_pct"] = 100 * a["dram_row_hits"] / max(1.0, a["dram_reads"])
    a["l2hit_pct"] = 100 * a["l2_hits"] / max(1.0, a["l2_hits"] + a["l2_misses"])
    a["walk_per_acc"] = a["walks"] / 16384.0
    return a


# ---------------------------------------------------------------- SVG canvas
def svg(path, title, xlab, ylab, series, xvals, yfmt="%.2f", w=760, h=440,
        ylog=False, ymin=None, ymax=None, notes=None):
    """series: list of (label, color, [y values in xvals order])."""
    import math
    ml, mr, mt, mb = 78, 210, 46, 58
    pw, ph = w - ml - mr, h - mt - mb
    ys = [y for _, _, vv in series for y in vv if y is not None]
    lo = min(ys) if ymin is None else ymin
    hi = max(ys) if ymax is None else ymax
    if ylog:
        lo, hi = math.log10(lo), math.log10(hi)
    if hi == lo:
        hi = lo + 1
    pad = (hi - lo) * 0.08
    lo, hi = lo - (0 if ymin is not None else pad), hi + (0 if ymax is not None else pad)

    def X(i):
        return ml + pw * i / (len(xvals) - 1)

    def Y(v):
        v = math.log10(v) if ylog else v
        return mt + ph * (1 - (v - lo) / (hi - lo))

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
         f'viewBox="0 0 {w} {h}" font-family="Helvetica,Arial,sans-serif">',
         f'<rect width="{w}" height="{h}" fill="#ffffff"/>',
         f'<text x="{ml}" y="26" font-size="15" font-weight="bold">{title}</text>']
    for g in range(5):
        v = lo + (hi - lo) * g / 4
        yy = Y(10 ** v if ylog else v)
        lab = f"{10**v:.4g}" if ylog else yfmt % v
        o.append(f'<line x1="{ml}" y1="{yy:.1f}" x2="{ml+pw}" y2="{yy:.1f}" '
                 f'stroke="#e6e6e6" stroke-width="1"/>')
        o.append(f'<text x="{ml-8}" y="{yy+4:.1f}" font-size="11" text-anchor="end" '
                 f'fill="#555">{lab}</text>')
    for i, xv in enumerate(xvals):
        xx = X(i)
        o.append(f'<line x1="{xx:.1f}" y1="{mt}" x2="{xx:.1f}" y2="{mt+ph}" '
                 f'stroke="#f2f2f2" stroke-width="1"/>')
        o.append(f'<text x="{xx:.1f}" y="{mt+ph+18}" font-size="11" '
                 f'text-anchor="middle" fill="#555">{xv:g}</text>')
    o.append(f'<line x1="{ml}" y1="{mt+ph}" x2="{ml+pw}" y2="{mt+ph}" stroke="#999"/>')
    o.append(f'<line x1="{ml}" y1="{mt}" x2="{ml}" y2="{mt+ph}" stroke="#999"/>')
    o.append(f'<text x="{ml+pw/2:.0f}" y="{h-16}" font-size="12" '
             f'text-anchor="middle">{xlab}</text>')
    o.append(f'<text x="18" y="{mt+ph/2:.0f}" font-size="12" text-anchor="middle" '
             f'transform="rotate(-90 18 {mt+ph/2:.0f})">{ylab}</text>')
    for j, (lab, col, vv) in enumerate(series):
        pts = " ".join(f"{X(i):.1f},{Y(y):.1f}" for i, y in enumerate(vv) if y is not None)
        o.append(f'<polyline points="{pts}" fill="none" stroke="{col}" stroke-width="2.4" '
                 f'stroke-linejoin="round"/>')
        for i, y in enumerate(vv):
            if y is not None:
                o.append(f'<circle cx="{X(i):.1f}" cy="{Y(y):.1f}" r="3.2" fill="{col}"/>')
        ly = mt + 8 + 22 * j
        o.append(f'<line x1="{ml+pw+16}" y1="{ly}" x2="{ml+pw+46}" y2="{ly}" '
                 f'stroke="{col}" stroke-width="3"/>')
        o.append(f'<text x="{ml+pw+52}" y="{ly+4}" font-size="11.5">{lab}</text>')
    if notes:
        yy = mt + ph - 8
        for note in reversed(notes):
            o.append(f'<text x="{ml+8}" y="{yy}" font-size="11" fill="#333">{note}</text>')
            yy -= 15
    o.append("</svg>")
    open(path, "w").write("\n".join(o))


def main():
    rows = load()
    os.makedirs("docs/figs", exist_ok=True)
    A = {(cfg, k): agg(rows, False, cfg, k) for cfg in CFGS for k in KS}
    P8 = {(cfg, k): agg(rows, True, cfg, k) for cfg in CFGS for k in (0.0, 0.25, 0.5, 0.75, 1.0)}

    # CSV
    with open("build/wm/m23e_sweep.csv", "w") as f:
        cols = ["side", "kappa", "cfg", "n", "cycles", "cycles_min", "cycles_max",
                "walks", "walk_per_acc", "l2hit_pct", "rowhit_pct", "rho_line",
                "lines", "pages", "bulk_elems", "bulk_instr", "simt_instr",
                "bulk_win_occ_max", "bulk_win_size_mean", "bulk_fanout_cycles",
                "dram_reads", "dram_bytes", "inflight_max"]
        f.write(",".join(cols) + "\n")
        for side, tab in ((False, A), (True, P8)):
            for (cfg, k), a in tab.items():
                if a is None:
                    continue
                f.write(",".join(["p8k" if side else "p512", f"{k:g}", cfg] +
                                 [f"{a[c]:.4f}" if isinstance(a[c], float) else str(a[c])
                                  for c in cols[3:]]) + "\n")

    base = {k: A[("m0", k)]["cycles"] for k in KS}
    svg("docs/figs/m23e-speedup.svg",
        "Speedup over M0 versus κ (N=16 384, P=512, mean of 3 seeds)",
        "κ", "speedup vs M0  (cycles_M0 / cycles)",
        [(LBL[c], COL[c], [base[k] / A[(c, k)]["cycles"] for k in KS]) for c in CFGS],
        KS, ymin=0.90, yfmt="%.2f",
        notes=["all four curves meet at κ = 1 (pure SIMT, 40 281 cycles)",
               "DX dips below 1.0 at κ = 0.9: the Bulk residue no longer pays for the engine"])
    svg("docs/figs/m23e-cycles.svg",
        "Execution cycles versus κ", "κ", "cycles",
        [(LBL[c], COL[c], [A[(c, k)]["cycles"] for k in KS]) for c in CFGS],
        KS, yfmt="%.0f", ylog=True)
    svg("docs/figs/m23e-walks.svg",
        "Translation behaviour versus κ", "κ", "page walks per access",
        [(LBL[c], COL[c], [A[(c, k)]["walk_per_acc"] for k in KS]) for c in CFGS],
        KS, yfmt="%.3f")
    svg("docs/figs/m23e-locality.svg",
        "Cache and DRAM locality versus κ", "κ", "percent",
        [("M0 L2 hit %", "#7f7f7f", [A[("m0", k)]["l2hit_pct"] for k in KS]),
         ("DX-16K L2 hit %", "#d62728", [A[("dx16k", k)]["l2hit_pct"] for k in KS]),
         ("M0 DRAM row hit %", "#1f1f1f", [A[("m0", k)]["rowhit_pct"] for k in KS]),
         ("DX-16K DRAM row hit %", "#ff9896", [A[("dx16k", k)]["rowhit_pct"] for k in KS])],
        KS, yfmt="%.0f")
    svg("docs/figs/m23e-window.svg",
        "Context-window utilisation versus κ", "κ", "window fill / capacity (mean)",
        [(LBL[c], COL[c], [A[(c, k)]["bulk_win_size_mean"] / (8192 if c == "dx8k" else
                                                             16384 if c == "dx16k" else 32768)
                           for k in KS]) for c in CFGS[1:]],
        KS, yfmt="%.2f", ymin=0.0, ymax=1.0,
        notes=["fill falls as κ routes more instructions to SIMT",
               "all three converge once Bulk elements fall below 8 192"])

    print("wrote docs/figs/m23e-*.svg and build/wm/m23e_sweep.csv")
    for cfg in CFGS:
        print(cfg, " ".join(f"{base[k]/A[(cfg,k)]['cycles']:.3f}" for k in KS))


if __name__ == "__main__":
    main()
