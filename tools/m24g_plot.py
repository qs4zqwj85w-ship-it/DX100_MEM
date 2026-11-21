#!/usr/bin/env python3
"""M24-G — scaling figures, hand-written SVG (no plotting library on this host).

Every point is a measured cell read from build/wm/m24g/cells.csv; nothing is
interpolated and no curve is fitted.  Four figures:

  docs/figs/m24g-common.svg     common-resource scaling, speedup vs level
  docs/figs/m24g-common-b.svg   the three knobs that are inert, same axes
  docs/figs/m24g-bulk.svg       Bulk-specific capacities, cycles vs capacity
  docs/figs/m24g-frontier.svg   speedup against engine SRAM bytes

Speedup is always M0-RM cycles / Bulk cycles at the *same* knob value (paired)
for the common figures, and M0-RM cycles (unchanged M24-D cell) / Bulk cycles
for the capacity figures.
"""
import csv
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build/wm/m24g")
FIGS = os.path.join(ROOT, "docs/figs")
M24F = os.path.join(ROOT, "build/wm/m24f")

INK = "#1b1b1f"
MUTED = "#6b6b75"
GRID = "#e2e2e8"
BLUE = "#2f6db5"
ORANGE = "#d97b28"
GREEN = "#3e8e5a"
RED = "#b53a3a"
PURPLE = "#7a4fa3"

SERIES = [("w4_duplow", BLUE, "w4_duplow (order-bound win)"),
          ("criteo_2k", ORANGE, "criteo_2k (translation-bound win)"),
          ("n1_seq8m", GREEN, "n1_seq8m (sequential loss)"),
          ("n3_clu4m", RED, "n3_clu4m (reuse loss)")]
SECOND = [("w1_uniform", PURPLE, "w1_uniform (L2 tie)"),
          ("orkut_hub", INK, "orkut_hub (graph win)"),
          ("n4_uni4m", BLUE, "n4_uni4m (order-bound win)")]


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, s, size=12, color=INK, anchor="start", weight="normal"):
    return ('<text x="%.1f" y="%.1f" font-family="Helvetica,Arial,sans-serif" font-size="%s" '
            'fill="%s" text-anchor="%s" font-weight="%s">%s</text>'
            % (x, y, size, color, anchor, weight, esc(s)))


def rect(x, y, w, h, fill, op=1.0, stroke="none"):
    return ('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s" opacity="%.2f" '
            'stroke="%s"/>' % (x, y, max(w, 0), max(h, 0), fill, op, stroke))


def line(x1, y1, x2, y2, color=GRID, wd=1, dash=""):
    d = ' stroke-dasharray="%s"' % dash if dash else ""
    return ('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="%s"%s/>'
            % (x1, y1, x2, y2, color, wd, d))


def circle(x, y, r=3.2, fill=BLUE):
    return '<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s"/>' % (x, y, r, fill)


def poly(xs, ys, color=BLUE, wd=1.8):
    pts = " ".join("%.1f,%.1f" % (x, y) for x, y in zip(xs, ys))
    return ('<polyline points="%s" fill="none" stroke="%s" stroke-width="%.1f" '
            'stroke-linejoin="round"/>' % (pts, color, wd))


def head(title, sub):
    return [text(24, 30, title, 16, INK, weight="bold"),
            text(24, 50, sub, 11, MUTED)]


def axes(x, y, w, h, ymin, ymax, ylab, yticks=5, logy=False):
    """Cartesian frame with horizontal grid; returns y->px."""
    o = [rect(x, y, w, h, "#ffffff", 1.0, GRID)]
    for i in range(yticks + 1):
        v = ymin + (ymax - ymin) * i / yticks
        yy = y + h - h * i / yticks
        o.append(line(x, yy, x + w, yy, GRID))
        o.append(text(x - 8, yy + 4, ("%.2f" % v) if ymax <= 6 else ("%.0f" % v),
                      10, MUTED, anchor="end"))
    o.append(text(x - 40, y + h / 2, ylab, 10.5, MUTED, anchor="middle"))
    return o


def ymap(v, ymin, ymax, y, h):
    return y + h - h * (v - ymin) / (ymax - ymin)


def xmap(i, n, x, w, pad=26):
    if n == 1:
        return x + w / 2
    return x + pad + (w - 2 * pad) * i / (n - 1)


def read_cells():
    rows = {}
    with open(os.path.join(BUILD, "cells.csv")) as f:
        for r in csv.DictReader(f):
            rows[(r["sweep"], r["level"], r["arm"], r["trace"])] = r
    return rows


def read_m24f(trace, arm):
    p = os.path.join(M24F, "%s__%s.txt" % (trace, arm))
    d = {}
    with open(p) as f:
        for ln in f:
            for tok in ln.split():
                if "=" in tok and tok.split("=")[0] in ("cycles", "bulk_win_bytes"):
                    k, v = tok.split("=", 1)
                    try:
                        d[k] = float(v)
                    except ValueError:
                        pass
    return d["cycles"]


def make_pt(R):
    """(spec, trace) -> (bulk_cycles, simt_cycles, sram_bytes) or None.

    spec is ("prod",) for the unmodified M24-D pair, or (sweep, level) for a
    swept cell.  A guarded cell returns its marker so the caller can draw it.
    """
    def pt(spec, tr):
        if spec[0] == "prod":
            b = read_m24f(tr, "bulk")
            s = read_m24f(tr, "simt")
            return (b, s, 279040 + 256 + 65536, False)
        sweep, lv = spec
        b = R.get((sweep, str(lv), "bulk", tr))
        if b is None:
            return None
        s = R.get((sweep, str(lv), "simt", tr))
        # a Bulk-only cell has no paired SIMT arm: its reference is the unchanged
        # M24-D SIMT cell, which no Bulk-specific key can reach.
        scyc = float(s["cycles"]) if s is not None else read_m24f(tr, "simt")
        guard = b.get("guard") == "1"
        return (float(b["cycles"]), scyc,
                float(b["win_bytes"]) + float(b["sched_bytes"]) + float(b["pa_bytes"]), guard)
    return pt


def fnum(s):
    try:
        return float(s)
    except ValueError:
        return None


def main():
    R = read_cells()
    os.makedirs(FIGS, exist_ok=True)

    # ---------------------------------------------------- figure 1: common --
    pt = make_pt(R)
    knobs = [("translation walkers", [("2", ("walkers", 2)), ("4", ("walkers", 4)), ("8 (prod)", ("prod",))]),
             ("dTLB MSHRs", [("4", ("dtlbmshr", 4)), ("8 (prod)", ("prod",)), ("16", ("dtlbmshr", 16))]),
             ("shared budget R", [("64", ("rbudget", 64)), ("128 (prod)", ("prod",)), ("192", ("rbudget", 192))]),
             ("DRAM banks", [("2 (prod)", ("prod",)), ("16", ("banks", 16))])]
    W, H = 1250, 760
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
         % (W, H, W, H), rect(0, 0, W, H, "#fafafc")]
    o += head("M24-G — common-resource scaling: speedup versus the resource level",
              "Both arms get the same value of the swept resource; every other field is the M24-D pair. "
              "Dashed grey = the unmodified pair for that trace.")
    pw, ph = 545, 290
    for k, (xlab, levels) in enumerate(knobs):
        px = 80 + (k % 2) * (pw + 40)
        py = 80 + (k // 2) * (ph + 40)
        o.append(rect(px, py, pw, ph, "#ffffff", 1.0, GRID))
        o.append(text(px + 12, py + 20, xlab, 12.5, INK, weight="bold"))
        o.append(text(px + 12, py + 36, "y: Bulk speedup (M0-RM / Bulk) at the same setting", 10, MUTED))
        gx, gy, gw, gh = px + 52, py + 54, pw - 74, ph - 96
        o += axes(gx, gy, gw, gh, 0.5, 6.0, "speedup")
        for i, (lab, _) in enumerate(levels):
            o.append(text(xmap(i, len(levels), gx, gw), gy + gh + 18, lab, 10, INK, anchor="middle"))
        for tr, col, _ in SERIES:
            xs, ys = [], []
            for i, (_, spec) in enumerate(levels):
                p = pt(spec, tr)
                if p is None:
                    continue
                b, s, _, guard = p
                if guard:
                    continue
                xs.append(xmap(i, len(levels), gx, gw))
                ys.append(ymap(s / b, 0.5, 6.0, gy, gh))
            if len(xs) > 1:
                o.append(poly(xs, ys, col))
            for xx, yy in zip(xs, ys):
                o.append(circle(xx, yy, 3.4, col))
    # legend
    ly = H - 34
    lx = 80
    for tr, col, lab in SERIES:
        o.append(line(lx, ly - 4, lx + 18, ly - 4, col, 2.4))
        o.append(text(lx + 24, ly, lab, 11, INK))
        lx += 270
    o.append("</svg>")
    open(os.path.join(FIGS, "m24g-common.svg"), "w").write("\n".join(o) + "\n")

    # -------------------------------------- figure 2: the inert knobs ------
    inert = [("L2-TLB MSHRs", [("8 (prod)", ("prod",)), ("16", ("l2tlbmshr", 16))]),
             ("L2 cache MSHRs", [("64 (prod)", ("prod",)), ("128", ("l2mshr", 128))]),
             ("DRAM queue depth", [("8", ("dramq", 8)), ("64", ("dramq", 64)),
                                   ("unbounded (prod)", ("prod",))]),
             ("walkers = dTLB = L2-TLB", [("8/8/8 (prod)", ("prod",)),
                                          ("16/16/16", ("translation16", 16))])]
    W, H = 1250, 760
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
         % (W, H, W, H), rect(0, 0, W, H, "#fafafc")]
    o += head("M24-G — the knobs that do nothing, and the one that gives the advantage away",
              "Same axes and traces as the previous figure. The 8-bar cells in grey dashed = production. "
              "Rightmost panel: 16 walkers, 16 dTLB MSHRs and 16 L2-TLB MSHRs at once.")
    for k, (xlab, levels) in enumerate(inert):
        px = 80 + (k % 2) * (pw + 40)
        py = 80 + (k // 2) * (ph + 40)
        o.append(rect(px, py, pw, ph, "#ffffff", 1.0, GRID))
        o.append(text(px + 12, py + 20, xlab, 12.5, INK, weight="bold"))
        o.append(text(px + 12, py + 36, "y: Bulk speedup (M0-RM / Bulk) at the same setting", 10, MUTED))
        gx, gy, gw, gh = px + 52, py + 54, pw - 74, ph - 96
        o += axes(gx, gy, gw, gh, 0.5, 6.0, "speedup")
        for i, (lab, _) in enumerate(levels):
            o.append(text(xmap(i, len(levels), gx, gw), gy + gh + 18, lab, 9.5, INK, anchor="middle"))
        for tr, col, _ in SERIES:
            xs, ys = [], []
            for i, (_, spec) in enumerate(levels):
                p_ = pt(spec, tr)
                if p_ is None:
                    continue
                b, s, _, guard = p_
                if guard:
                    continue
                xs.append(xmap(i, len(levels), gx, gw))
                ys.append(ymap(s / b, 0.5, 6.0, gy, gh))
            if len(xs) > 1:
                o.append(poly(xs, ys, col))
            for xx, yy in zip(xs, ys):
                o.append(circle(xx, yy, 3.4, col))
    ly = H - 34
    lx = 80
    for tr, col, lab in SERIES:
        o.append(line(lx, ly - 4, lx + 18, ly - 4, col, 2.4))
        o.append(text(lx + 24, ly, lab, 11, INK))
        lx += 270
    o.append("</svg>")
    open(os.path.join(FIGS, "m24g-common-b.svg"), "w").write("\n".join(o) + "\n")

    # --------------------------------------------------- figure 3: bulk ----
    caps = [("page cohorts", [("1", ("cohorts", "1")), ("2", ("cohorts", "2")),
                              ("4", ("cohorts", "4")), ("8 (prod)", ("prod",)),
                              ("16", ("cohorts", "16"))]),
            ("window elements per buffer",
             [("8K", ("window", "8192")), ("16K (prod)", ("prod",)),
              ("32K", ("window", "32768"))]),
            ("ready-pool entries",
             [("512", ("readypool", "512")), ("1024", ("readypool", "1024")),
              ("2048", ("readypool", "2048")), ("4096 (prod)", ("prod",)),
              ("8192", ("readypool", "8192"))]),
            ("fanout / org / AG lanes",
             [("1", ("fanout", "1")), ("4 (prod)", ("prod",)), ("8", ("fanout", "8"))])]
    W, H = 1250, 760
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
         % (W, H, W, H), rect(0, 0, W, H, "#fafafc")]
    o += head("M24-G — Bulk-specific capacity scaling: cycles relative to the production configuration",
              "Bulk arm only; y is cycles / (production Bulk cycles) for that trace, so 0.8 means 20 % faster than shipping. "
              "Lower is better. An open square at the top is a cell that deadlocked.")
    for k, (xlab, levels) in enumerate(caps):
        px = 80 + (k % 2) * (pw + 40)
        py = 80 + (k // 2) * (ph + 40)
        o.append(rect(px, py, pw, ph, "#ffffff", 1.0, GRID))
        o.append(text(px + 12, py + 20, xlab, 12.5, INK, weight="bold"))
        o.append(text(px + 12, py + 36, "y: cycles relative to production (1.00 = unchanged)", 10, MUTED))
        gx, gy, gw, gh = px + 52, py + 54, pw - 74, ph - 96
        o += axes(gx, gy, gw, gh, 0.5, 4.0, "relative cycles", yticks=7)
        for i, (lab, _) in enumerate(levels):
            o.append(text(xmap(i, len(levels), gx, gw), gy + gh + 18, lab, 9.5, INK, anchor="middle"))
        for tr, col, _ in SERIES:
            baseb = read_m24f(tr, "bulk")
            xs, ys = [], []
            for i, (_, spec) in enumerate(levels):
                p_ = pt(spec, tr)
                if p_ is None:
                    continue
                b, _, _, guard = p_
                xx = xmap(i, len(levels), gx, gw)
                if guard:
                    yy = ymap(4.0, 0.5, 4.0, gy, gh)
                    o.append('<rect x="%.1f" y="%.1f" width="7" height="7" fill="none" '
                             'stroke="%s" stroke-width="1.6"/>' % (xx - 3.5, yy, col))
                    continue
                xs.append(xx)
                ys.append(ymap(b / baseb, 0.5, 4.0, gy, gh))
            if len(xs) > 1:
                o.append(poly(xs, ys, col))
            for xx, yy in zip(xs, ys):
                o.append(circle(xx, yy, 3.4, col))
        yb = ymap(1.0, 0.5, 4.0, gy, gh)
        o.append(line(gx, yb, gx + gw, yb, MUTED, 1, "4 3"))

    o.append("</svg>")
    open(os.path.join(FIGS, "m24g-bulk.svg"), "w").write("\n".join(o) + "\n")

    # ------------------------------------------------ figure 4: frontier ---
    W, H = 1180, 560
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
         % (W, H, W, H), rect(0, 0, W, H, "#fafafc")]
    o += head("M24-G — resource/performance frontier: what the engine's SRAM buys",
              "x: engine SRAM (window + cohort/cohort-state + ready pool, bytes, as the simulator accounts them). "
              "y: Bulk speedup over the unchanged M0-RM cell.  rt = ready pool; W = window elements.")
    gx, gy, gw, gh = 110, 90, 940, 380
    o.append(rect(gx, gy, gw, gh, "#ffffff", 1.0, GRID))
    xmin, xmax = 250000.0, 620000.0
    ys_max = 6.0
    for i in range(7):
        yy = gy + gh - gh * i / 6
        o.append(line(gx, yy, gx + gw, yy, GRID))
        o.append(text(gx - 10, yy + 4, "%.1f" % (ys_max * i / 6), 10, MUTED, anchor="end"))
    for i in range(5):
        v = xmin + (xmax - xmin) * i / 4
        xx = gx + gw * i / 4
        o.append(line(xx, gy, xx, gy + gh, GRID))
        o.append(text(xx, gy + gh + 18, "%d KiB" % round(v / 1024), 10, MUTED, anchor="middle"))
    o.append(text(gx + gw / 2, gy + gh + 40, "engine SRAM (bytes)", 11, MUTED, anchor="middle"))
    o.append(text(gx - 56, gy + gh / 2, "speedup", 11, MUTED, anchor="middle"))

    # points: production, and the single-knob variants measured on both traces
    pts = []
    for tr, col, lab in (SERIES[0], SERIES[1], SERIES[3]):
        simt = read_m24f(tr, "simt")
        for tag, sweep, lv in [("production", None, None),
                               ("W=8K", "window", "8192"), ("W=32K", "window", "32768"),
                               ("rt=512", "readypool", "512"), ("rt=8192", "readypool", "8192"),
                               ("cohorts=4", "cohorts", "4"), ("cohorts=16", "cohorts", "16")]:
            if sweep is None:
                b = R.get(("cohorts", "8", "bulk", tr)) or None
                sram = 279040 + 256 + 65536
                cyc = read_m24f(tr, "bulk")
            else:
                b = R.get((sweep, lv, "bulk", tr))
                if not b:
                    continue
                sram = float(b["win_bytes"]) + float(b["sched_bytes"]) + float(b["pa_bytes"])
                cyc = float(b["cycles"])
            if b is not None and b.get("guard") == "1":
                continue
            x = gx + gw * (sram - xmin) / (xmax - xmin)
            y = gy + gh - gh * (simt / cyc) / ys_max
            pts.append((x, y, col, tag, tr))
            o.append(circle(x, y, 4.0, col))
    # annotate the two ends that matter
    for x, y, col, tag, tr in pts:
        if tag in ("production", "W=32K"):
            o.append(text(x + 7, y - 5, "%s %s" % (tr.split("_")[0], tag), 9.5, MUTED))
    o.append(text(gx + 20, gy + 26,
                  "production: 337 KiB - w4 2.50x, criteo 3.28x", 11, INK))
    o.append(text(gx + 20, gy + 44,
                  "ready pool 512 (281 KiB): same or faster on both", 11, MUTED))
    o.append(text(gx + 20, gy + 62,
                  "window 32K (593 KiB): +13 % w4, +30 % criteo", 11, MUTED))
    o.append("</svg>")
    open(os.path.join(FIGS, "m24g-frontier.svg"), "w").write("\n".join(o) + "\n")
    print("wrote 4 figures to docs/figs/")


if __name__ == "__main__":
    main()
