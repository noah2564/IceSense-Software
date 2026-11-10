"""The firmware's parameter table and defaults must match the Python reference."""
import os
import re

from algorithm.icedetect import PARAM_LIMITS, Params

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = open(os.path.join(ROOT, "firmware", "src", "main.cpp"), encoding="utf-8").read()
ROW = re.compile(r'\{"(\w+)", "(\w+)", (&P::\w+|nullptr), (&P::\w+|nullptr), (-?[\d.]+), (-?[\d.]+)\}')


def test_param_table_matches_python_limits_and_nvs_keys_fit():
    rows = ROW.findall(SRC)
    assert {r[0] for r in rows} == set(PARAM_LIMITS)
    for name, key, d, i, lo, hi in rows:
        pl, ph, is_int = PARAM_LIMITS[name]
        assert (float(lo), float(hi)) == (float(pl), float(ph)), name
        assert (i != "nullptr") == is_int and (d != "nullptr") == (not is_int), name
        assert len(key) <= 15, f"NVS key too long: {key}"
    assert len({r[1] for r in rows}) == len(rows)


def test_cpp_defaults_match_python_defaults():
    h = open(os.path.join(ROOT, "firmware", "lib", "icedetect", "icedetect.h"), encoding="utf-8").read()
    for k, v in Params().as_dict().items():
        m = re.search(rf"\b{k} = ([-\d.]+);", h)
        assert m and float(m.group(1)) == float(v), k
