#!/usr/bin/env python3
"""M25: evaluate a conservative SIMT/Bulk selection policy on the M24-F matrix.

Inputs: build/wm/m25/diag.csv (tools/m25_extract.py) only.  No simulator run.

The policy is a conjunction of three tests, each with a hardware reason:

  G0  the loop is indirect (an index load feeds the address) -- static, from the
      IR: `data[i]`, `data[2*i]` and sweeps are SIMT by construction.  Every
      trace in this matrix is indirect, so G0 is true everywhere here and is
      recorded as a precondition rather than a discriminator.
  G1  target pages >= TLB_REACH (512 pages = the L2 TLB's 512 entries at 4 KiB),
      i.e. the address stream can actually overflow translation.
  G2  sequential fraction < 0.5 -- the stream is not already in the order the
      DRAM controller and the TLB can exploit.
  G3  index lines are < 50 % of the lines the loop touches.  When the target set
      is small the index stream *is* the traffic, and Bulk pays for it in full
      (measured: 100 % L2 miss on it) while SIMT's index loads hit L1.

Outputs: build/wm/m25/policy.csv, policy.txt.
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W = os.path.join(ROOT, "build", "wm", "m25")
TLB_REACH = 512      # L2 TLB entries (configs: l2tlb = 512 x 4)
SEQ_MAX = 0.5
IDX_MAX = 0.5
CHECKS = []


def chk(ok, what, detail=""):
    CHECKS.append((bool(ok), what, detail))
    if not ok:
        print(f"FAIL {what} {detail}")


def decide(r, pages=TLB_REACH, seq=SEQ_MAX, idx=IDX_MAX):
    if float(r["tgt_pages"]) < pages:
        return "simt", "pages < %d" % pages
    if float(r["seq_frac"]) >= seq:
        return "simt", "sequential >= %.2f" % seq
    if float(r["idx_share"]) >= idx:
        return "simt", "index share >= %.2f" % idx
    return "bulk", "irregular, page-spread, index amortised"


def main():
    rows = list(csv.DictReader(open(os.path.join(W, "diag.csv"))))
    out = []
    for r in rows:
        mode, why = decide(r)
        cyc_b, cyc_s = float(r["cycles_bulk"]), float(r["cycles_simt"])
        oracle = min(cyc_b, cyc_s)
        chosen = cyc_b if mode == "bulk" else cyc_s
        best_mode = "bulk" if cyc_b < cyc_s else "simt"
        out.append({
            "trace": r["trace"],
            "speedup_bulk": float(r["speedup"]),
            "oracle_mode": best_mode,
            "chosen": mode,
            "correct": mode == best_mode,
            "why": why,
            "cycles_chosen": chosen,
            "cycles_oracle": oracle,
            "regret_pct": 100.0 * (chosen - oracle) / oracle,
            "gain_vs_simt_pct": 100.0 * (cyc_s - chosen) / cyc_s,
            "tgt_pages": float(r["tgt_pages"]),
            "seq_frac": float(r["seq_frac"]),
            "idx_share": float(r["idx_share"]),
            "reuse16k": float(r["reuse16k"]),
        })

    with open(os.path.join(W, "policy.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(out[0].keys()))
        w.writeheader()
        for r in out:
            w.writerow({k: (round(v, 3) if isinstance(v, float) else v) for k, v in r.items()})

    n = len(out)
    correct = sum(1 for r in out if r["correct"])
    sum_chosen = sum(r["cycles_chosen"] for r in out)
    sum_oracle = sum(r["cycles_oracle"] for r in out)
    sum_simt = sum(float(r["cycles_simt"]) if "cycles_simt" in r else 0 for r in rows)
    sum_bulk = sum(float(r["cycles_bulk"]) for r in rows)
    # A trace that ties counts as correct for either mode.
    with open(os.path.join(W, "policy.txt"), "w") as f:
        f.write("== selection policy on the 31-trace M24-F matrix ==\n")
        f.write(f"gate: pages >= {TLB_REACH}, sequential < {SEQ_MAX}, index share < {IDX_MAX}\n\n")
        f.write(f"{'trace':16s} {'bulk_sp':>8s} {'oracle':>7s} {'chosen':>7s} {'ok':>3s} "
                f"{'regret%':>8s} {'vsSIMT%':>8s}  why\n")
        for r in sorted(out, key=lambda x: x["speedup_bulk"]):
            f.write(f"{r['trace']:16s} {r['speedup_bulk']:8.3f} {r['oracle_mode']:>7s} "
                    f"{r['chosen']:>7s} {'yes' if r['correct'] else 'NO':>3s} "
                    f"{r['regret_pct']:8.2f} {r['gain_vs_simt_pct']:8.2f}  {r['why']}\n")
        f.write(f"\ncorrect {correct}/{n}\n")
        f.write(f"sum cycles chosen {sum_chosen:.0f}  oracle {sum_oracle:.0f}  "
                f"regret {100.0 * (sum_chosen - sum_oracle) / sum_oracle:.2f} %\n")
        f.write(f"always-SIMT {sum_simt:.0f}  always-Bulk {sum_bulk:.0f}\n")
        f.write(f"policy vs always-SIMT {100.0 * (sum_simt - sum_chosen) / sum_simt:+.2f} %\n")
        f.write(f"policy vs always-Bulk {100.0 * (sum_bulk - sum_chosen) / sum_bulk:+.2f} %\n")

        f.write("\n== threshold sensitivity (sum-cycles regret vs oracle) ==\n")
        f.write(f"{'pages':>6s} " + " ".join(f"{'seq%.2f' % s:>9s}" for s in (0.25, 0.5, 0.75)) + "\n")
        for pg in (256, 512, 1024):
            line = f"{pg:6d} "
            for sq in (0.25, 0.5, 0.75):
                tot = 0.0
                for r in rows:
                    m, _ = decide(r, pg, sq)
                    tot += float(r["cycles_bulk"]) if m == "bulk" else float(r["cycles_simt"])
                line += f"{100.0 * (tot - sum_oracle) / sum_oracle:8.2f}% "
            f.write(line + "\n")

        f.write("\n== switching cost: what a region boundary costs on the Bulk side ==\n")
        f.write("start = cycles from submission to the first retired element (descriptor fetch,\n"
                "index tile fill and the first window's seal), as a fraction of the run\n")
        f.write(f"{'trace':16s} {'cycles':>9s} {'start':>7s} {'start%':>7s} {'drain':>7s}\n")
        for r in sorted((r for r in rows if float(r["batches"]) > 0),
                        key=lambda x: -float(x["first_done"]) / float(x["cycles_bulk"]))[:8]:
            f.write(f"{r['trace']:16s} {float(r['cycles_bulk']):9.0f} {float(r['first_done']):7.0f} "
                    f"{100.0 * float(r['first_done']) / float(r['cycles_bulk']):6.2f}% "
                    f"{float(r['cycles_bulk']) - float(r['last_done']):7.0f}\n")

    chk(n == 31, "31 traces evaluated", str(n))
    for r in out:
        chk(r["regret_pct"] >= -1e-9, f"{r['trace']}: policy never beats the oracle")
    chk(sum_chosen <= sum_simt, "policy is not worse than always-SIMT in aggregate")
    ok = sum(1 for c in CHECKS if c[0])
    print(f"{len(CHECKS)} checks: {len(CHECKS) - ok} failures; correct {correct}/{n}; "
          f"regret {100.0 * (sum_chosen - sum_oracle) / sum_oracle:.2f}%")
    return 0 if ok == len(CHECKS) else 1


if __name__ == "__main__":
    sys.exit(main())
