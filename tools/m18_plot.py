#!/usr/bin/env python3
"""M18: speedup / locality / BFS-ceiling chart as hand-written SVG.

No plotting library is available on this host (no matplotlib, no numpy), so the
figure is emitted directly. Every number is a measured value transcribed from
the raw cell logs; nothing is interpolated.
"""
import os

W, H = 1080, 760
INK = "#1b1b1f"
MUTED = "#6b6b75"
GRID = "#e2e2e8"
BLUE = "#2f6db5"
ORANGE = "#d97b28"
GREEN = "#3e8e5a"
RED = "#b53a3a"

MATRICES = ["bcsstk25", "qc2534", "lnsp3937", "dubcova3"]
# (same-trace 8K, faithful 8K, faithful 16K)
SPEED = [(0.938, 1.182, 1.177), (0.825, 1.047, 1.025), (1.180, 1.236, 1.237), (0.878, 1.179, 1.175)]
ROWHIT = [(80.0, 86.8), (87.2, 88.5), (74.8, 87.7), (70.2, 83.8)]
BFS = [("facebook_combined", 142264, 113931), ("email-Enron", 376124, 277115)]


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, s, size=12, color=INK, anchor="start", weight="normal"):
    return (f'<text x="{x}" y="{y}" font-family="Helvetica,Arial,sans-serif" font-size="{size}" '
            f'fill="{color}" text-anchor="{anchor}" font-weight="{weight}">{esc(s)}</text>')


def rect(x, y, w, h, fill, op=1.0):
    return f'<rect x="{x:.1f}" y="{y:.1f}" width="{max(w,0):.1f}" height="{max(h,0):.1f}" fill="{fill}" opacity="{op}"/>'


def line(x1, y1, x2, y2, color=GRID, wd=1, dash=""):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{color}" stroke-width="{wd}"{d}/>'


def panel(x, y, w, h, title, subtitle=""):
    out = [rect(x, y, w, h, "#ffffff"), rect(x, y, w, h, "none")]
    out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="none" stroke="{GRID}"/>')
    out.append(text(x + 12, y + 20, title, 13, INK, weight="bold"))
    if subtitle:
        out.append(text(x + 12, y + 37, subtitle, 10.5, MUTED))
    return out


def main():
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">']
    o.append(rect(0, 0, W, H, "#fafafc"))
    o.append(text(24, 30, "M18 — real-workload results: page-aware Bulk engine vs Vortex-calibrated SIMT",
                  16, INK, weight="bold"))
    o.append(text(24, 50, "SpMV over four SuiteSparse matrices, BFS over two SNAP graphs. "
                          "Trace-driven, reads only, identical hierarchy in every arm.", 11, MUTED))

    # ---- panel 1: SpMV speedup ----
    px, py, pw, ph = 24, 70, 500, 300
    o += panel(px, py, pw, ph, "SpMV speedup vs SIMT",
               "same-trace = identical instruction stream (index charged twice to the hybrid)")
    gx, gy, gw, gh = px + 46, py + 62, pw - 70, ph - 120
    lo, hi = 0.75, 1.30

    def yy(v):
        return gy + gh - (v - lo) / (hi - lo) * gh

    for g in (0.8, 0.9, 1.0, 1.1, 1.2, 1.3):
        o.append(line(gx, yy(g), gx + gw, yy(g), GRID if abs(g - 1.0) > 1e-9 else MUTED, 1,
                      "" if abs(g - 1.0) > 1e-9 else "4 3"))
        o.append(text(gx - 8, yy(g) + 4, f"{g:.1f}×", 9.5, MUTED, "end"))
    o.append(text(gx + gw, yy(1.0) - 6, "parity", 9.5, MUTED, "end"))
    bw = gw / (len(MATRICES) * 3.6)
    for i, (m, s) in enumerate(zip(MATRICES, SPEED)):
        base = gx + i * gw / len(MATRICES) + 10
        for j, (v, col) in enumerate(zip(s, (BLUE, ORANGE, GREEN))):
            x = base + j * bw * 1.15
            o.append(rect(x, yy(v), bw, yy(lo) - yy(v), col))
            o.append(text(x + bw / 2, yy(v) - 4, f"{v:.2f}", 8.5, INK, "middle"))
        o.append(text(base + bw * 1.7, gy + gh + 15, m, 10, INK, "middle"))
    # legend
    lx = px + 60
    for col, lab in ((BLUE, "same trace, 2×8K"), (ORANGE, "faithful, 2×8K"), (GREEN, "faithful, 2×16K")):
        o.append(rect(lx, py + ph - 24, 10, 10, col))
        o.append(text(lx + 15, py + ph - 15, lab, 9.5, MUTED))
        lx += 150

    # ---- panel 2: row-hit rate ----
    px2, py2, pw2, ph2 = 540, 70, 240, 300
    o += panel(px2, py2, pw2, ph2, "Gather-side DRAM row hits", "the mechanism page ordering targets")
    gx2, gy2, gw2, gh2 = px2 + 40, py2 + 62, pw2 - 60, ph2 - 120
    for g in (0, 25, 50, 75, 100):
        y = gy2 + gh2 - g / 100 * gh2
        o.append(line(gx2, y, gx2 + gw2, y, GRID))
        o.append(text(gx2 - 6, y + 4, f"{g}%", 9, MUTED, "end"))
    bw2 = gw2 / (len(MATRICES) * 3.0)
    for i, (m, (a, c)) in enumerate(zip(MATRICES, ROWHIT)):
        base = gx2 + i * gw2 / len(MATRICES) + 8
        for j, (v, col) in enumerate(((a, BLUE), (c, ORANGE))):
            x = base + j * bw2 * 1.2
            top = gy2 + gh2 - v / 100 * gh2
            o.append(rect(x, top, bw2, gy2 + gh2 - top, col))
            o.append(text(x + bw2 / 2, top - 3, f"{v:.0f}", 8, INK, "middle"))
        o.append(text(base + bw2 * 1.1, gy2 + gh2 + 14, m[:6], 8.5, INK, "middle"))
    o.append(text(px2 + 40, py2 + ph2 - 15, "SIMT", 9.5, BLUE))
    o.append(text(px2 + 88, py2 + ph2 - 15, "2×8K hybrid", 9.5, ORANGE))

    # ---- panel 3: where the time goes ----
    px3, py3, pw3, ph3 = 24, 386, 500, 330
    o += panel(px3, py3, pw3, ph3, "Where the hybrid's gain comes from",
               "fraction of baseline cycles: index-stream cost vs the engine's own added cost")
    gx3, gy3, gw3, gh3 = px3 + 120, py3 + 66, pw3 - 150, ph3 - 110
    for g in (0, 10, 20, 30, 40, 50):
        x = gx3 + g / 50 * gw3
        o.append(line(x, gy3, x, gy3 + gh3, GRID))
        o.append(text(x, gy3 + gh3 + 14, f"{g}%", 9, MUTED, "middle"))
    bh = gh3 / (len(MATRICES) * 2.4)
    IDX = [33.1, 41.4, 34.4, 30.1]
    ENG = [17.7, 36.9, 15.2, 15.0]
    NET = [1.182, 1.047, 1.236, 1.179]
    for i, m in enumerate(MATRICES):
        yb = gy3 + i * gh3 / len(MATRICES) + 6
        o.append(text(gx3 - 8, yb + bh * 1.1, m, 10, INK, "end"))
        o.append(rect(gx3, yb, IDX[i] / 50 * gw3, bh, ORANGE))
        o.append(text(gx3 + IDX[i] / 50 * gw3 + 5, yb + bh * 0.8, f"index {IDX[i]:.0f}%", 9, INK))
        o.append(rect(gx3, yb + bh * 1.3, ENG[i] / 50 * gw3, bh, RED))
        o.append(text(gx3 + ENG[i] / 50 * gw3 + 5, yb + bh * 2.1, f"engine cost {ENG[i]:.0f}%", 9, INK))
        o.append(text(gx3 + gw3, yb + bh * 2.1, f"net {NET[i]:.2f}×", 9.5, MUTED, "end"))
    o.append(text(px3 + 12, py3 + ph3 - 8,
                  "benefit = index offload − engine cost; both are measured, not modelled", 9.5, MUTED))

    # ---- panel 4: BFS ceiling ----
    px4, py4, pw4, ph4 = 540, 386, 500, 330
    o += panel(px4, py4, pw4, ph4, "BFS: the ceiling for a single-level engine",
               "the dist-probe stream removed; no implementation can do this")
    gx4, gy4, gw4, gh4 = px4 + 40, py4 + 80, pw4 - 70, ph4 - 150
    for i, (g, full, noprobe) in enumerate(BFS):
        y = gy4 + i * gh4 / len(BFS) + 10
        h = 34
        o.append(rect(gx4, y, gw4 * noprobe / full, h, BLUE))
        o.append(rect(gx4 + gw4 * noprobe / full, y, gw4 * (full - noprobe) / full, h, ORANGE))
        o.append(text(gx4 - 8, y + h * 0.7, g, 10, INK, "end"))
        o.append(text(gx4 + 6, y + h * 0.7, f"{noprobe:,} cyc", 9.5, "#ffffff"))
        pct = 100.0 * (full - noprobe) / full
        o.append(text(gx4 + gw4 * noprobe / full + 6, y + h * 0.7, f"probe {pct:.0f}%", 9.5, INK))
    o.append(text(gx4, gy4 + gh4 + 6, "everything else (adjacency, frontier, loop)", 9.5, BLUE))
    o.append(text(gx4, gy4 + gh4 + 22, "dist probe — the only stream a one-level engine could address", 9.5, ORANGE))
    o.append(text(px4 + 12, py4 + ph4 - 12,
                  "BFS is not evaluable by this engine: chained gather + writes (M18 BFS report §1)", 9.5, MUTED))

    o.append("</svg>")
    out = os.path.join(os.path.dirname(__file__), "..", "docs", "m18-results.svg")
    with open(out, "w") as f:
        f.write("\n".join(o))
    print("wrote", os.path.normpath(out))


if __name__ == "__main__":
    main()
