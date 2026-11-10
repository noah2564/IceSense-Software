"""Replay a recorded CSV through the reference algorithm with different parameters, and fit the
thickness factor from reference measurements.

    python lab/replay.py data/windows.csv --set ice_pct=25 --set confirm_windows=3
    python lab/replay.py data/windows.csv --reference-col ruler_mm     # least-squares mm_per_pct

The CSV needs the columns cap_delta_pct,res_log10,temp_c (device CSV or logger output). Optionally add
a column with manually measured thickness (e.g. ruler_mm, empty where not measured).
"""
from __future__ import annotations

import argparse
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from algorithm.icedetect import PARAM_LIMITS, Detector, Params, State, validate_params  # noqa: E402


def num(x):
    return float("nan") if x in ("", None) else float(x)


def replay(rows, params: Params):
    det = Detector(params)
    return [det.step(num(r["cap_delta_pct"]), num(r["res_log10"]), num(r["temp_c"])) for r in rows]


def fit_mm_per_pct(rows, ref_col):
    """Least squares through the origin: mm = k * |cap_delta_pct| for rows with a reference thickness."""
    pairs = [(abs(num(r["cap_delta_pct"])), num(r[ref_col])) for r in rows
             if r.get(ref_col) not in ("", None) and r["cap_delta_pct"] not in ("", None)]
    den = sum(x * x for x, _ in pairs)
    if len(pairs) < 2 or den == 0:
        raise SystemExit("need at least 2 rows with a reference thickness")
    return sum(x * y for x, y in pairs) / den, len(pairs)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--set", action="append", default=[], metavar="NAME=VALUE", help=f"one of {sorted(PARAM_LIMITS)}")
    ap.add_argument("--reference-col")
    a = ap.parse_args()
    rows = list(csv.DictReader(open(a.csv, encoding="utf-8")))
    over = {k: float(v) for k, v in (s.split("=", 1) for s in a.set)}
    err = validate_params(over)
    if err:
        raise SystemExit(err)
    p = Params(**{k: (int(v) if PARAM_LIMITS[k][2] else v) for k, v in over.items()})
    if a.reference_col:
        k, n = fit_mm_per_pct(rows, a.reference_col)
        print(f"mm_per_pct = {k:.4f} (least squares over {n} reference points)")
        p.mm_per_pct = k
    outs = replay(rows, p)
    changes = [(i, o.state) for i, o in enumerate(outs) if i == 0 or o.state != outs[i - 1].state]
    print(f"{len(rows)} windows; state changes: " + " > ".join(f"{State(s).label}@{i}" for i, s in changes))
    print(f"defrost requested in {sum(o.defrost_request for o in outs)} windows; max thickness {max(o.thickness_mm for o in outs):.2f} mm")


if __name__ == "__main__":
    main()
