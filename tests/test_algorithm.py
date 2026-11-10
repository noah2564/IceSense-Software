import random

import pytest

from algorithm.icedetect import Detector, Params, State
from simulator import scenario

NAN = float("nan")


def run(det, n, cap, res, temp):
    out = None
    for _ in range(n):
        out = det.step(cap, res, temp)
    return out


def test_unknown_without_baseline_or_temperature():
    d = Detector()
    assert run(d, 5, NAN, 6.9, 3.0).state == State.UNKNOWN
    d = Detector()
    assert run(d, 5, 30.0, 6.4, NAN).state == State.UNKNOWN


def test_clear_when_nothing_on_the_electrode():
    out = run(Detector(), 3, 0.5, 6.9, 3.0)
    assert out.state == State.CLEAR and out.thickness_mm == 0 and not out.defrost_request


def test_liquid_film_above_freezing_is_condensation_not_ice():
    out = run(Detector(), 3, 8.0, 5.0, 3.0)
    assert out.state == State.CONDENSATION and out.thickness_mm == 0


def test_frozen_deposit_frost_then_ice_with_thickness():
    d = Detector()
    assert run(d, 3, 9.0, 6.4, -5.0).state == State.FROST
    out = run(d, 3, 30.0, 6.4, -5.0)
    assert out.state == State.ICE
    assert out.thickness_mm == pytest.approx(3.0)


def test_capacitance_sign_is_ignored():
    assert run(Detector(), 3, -30.0, 6.4, -5.0).state == State.ICE


def test_resistive_channel_is_optional():
    assert run(Detector(), 3, 30.0, NAN, -5.0).state == State.ICE


def test_state_change_needs_confirmation():
    d = Detector()
    run(d, 3, 30.0, 6.4, -5.0)
    assert d.step(0.0, 6.9, -5.0).state == State.ICE      # single glitch window is ignored
    assert d.step(30.0, 6.4, -5.0).state == State.ICE
    run(d, 2, 0.0, 6.9, -5.0)
    assert d.state == State.CLEAR


def test_thickness_is_clamped():
    out = run(Detector(), 3, 500.0, 6.4, -5.0)
    assert out.thickness_mm == pytest.approx(Params().max_mm)


def test_defrost_request_latches_and_releases_with_hysteresis():
    d = Detector()
    run(d, 3, 9.0, 6.4, -5.0)                              # frost, 0.9 mm: no request
    assert not d.defrost
    d.step(30.0, 6.4, -5.0); d.step(30.0, 6.4, -5.0)
    assert not d.defrost                                   # needs 3 windows at >= 2 mm
    assert d.step(30.0, 6.4, -5.0).defrost_request
    # thickness drops between the two thresholds (0.5 mm .. 2 mm): request stays on
    assert run(d, 5, 12.0, 6.4, -5.0).defrost_request
    # deposit gone: released after 2 windows
    assert run(d, 4, 0.0, 6.9, -5.0).defrost_request is False


def test_no_defrost_request_while_state_unknown():
    d = Detector()
    run(d, 5, 30.0, 6.4, -5.0)
    assert d.defrost
    assert run(d, 3, 30.0, 6.4, NAN).defrost_request is False   # sensor fault: fail safe, no request


def test_melting_after_ice_when_film_turns_liquid():
    d = Detector()
    run(d, 5, 30.0, 6.4, -5.0)
    out = run(d, 3, 25.0, 4.8, 2.0)
    assert out.state == State.MELTING and out.thickness_mm > 0


def test_synthetic_cycle_walks_through_all_states_in_order():
    rng = random.Random(1)
    d = Detector()
    seen = []
    on_windows = 0
    for i in range(300):
        v = scenario.at(i * 2.0, 600, rng)
        out = d.step(v["cap_delta_pct"], v["log10_ohm"], v["temp_c"])
        if not seen or seen[-1] != out.state:
            seen.append(out.state)
        on_windows += out.defrost_request
    assert seen == [State.UNKNOWN, State.CLEAR, State.CONDENSATION, State.FROST, State.ICE,
                    State.MELTING, State.CONDENSATION, State.CLEAR]
    assert 100 < on_windows < 160            # defrost requested for the second half of the cycle only


def test_state_numbers_are_stable():
    # part of the output concept (docs/outputs.md); changing them breaks integrations
    assert [int(s) for s in State] == [0, 1, 2, 3, 4, 5]
    assert [s.label for s in State] == ["unknown", "clear", "condensation", "frost", "ice", "melting"]
