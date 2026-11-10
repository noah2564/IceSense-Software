"""Writes the synthetic sensor trace the host harness replays (t_s, temp_c, cap_pct, log10_ohm)."""
import random, sys
sys.path.insert(0, "../..")
from simulator import scenario
rng = random.Random(1)
with open("trace.csv", "w") as fh:
    for i in range(6500):
        v = scenario.at(i * 0.1, 600, rng)
        fh.write(f"{v['temp_c']:.3f},{v['cap_delta_pct']:.3f},{v['log10_ohm']:.4f}\n")
