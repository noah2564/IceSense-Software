"""The firmware's C++ port must reproduce the Python reference on every test vector."""
import os
import shutil
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(ROOT, "firmware", "lib", "icedetect")
TEST_CPP = os.path.join(ROOT, "algorithm", "cpp", "icedetect_test.cpp")
VECTORS = os.path.join(ROOT, "algorithm", "testvectors.csv")


@pytest.mark.skipif(shutil.which("g++") is None, reason="g++ not installed")
def test_cpp_port_matches_python_reference(tmp_path):
    exe = str(tmp_path / "icedetect_test")
    subprocess.run(["g++", "-std=c++17", "-O1", "-Wall", "-Wextra", f"-I{LIB}", TEST_CPP,
                    os.path.join(LIB, "icedetect.cpp"), "-o", exe], check=True)
    res = subprocess.run([exe, VECTORS], capture_output=True, text=True)
    assert res.returncode == 0, res.stderr + res.stdout
    assert "0 mismatches" in res.stdout


def test_test_vectors_are_up_to_date_with_the_python_reference(tmp_path, monkeypatch):
    """Fails if icedetect.py changed but testvectors.csv was not regenerated."""
    import importlib

    gen = importlib.import_module("algorithm.gen_testvectors")
    out = tmp_path / "vectors.csv"
    monkeypatch.setattr(gen, "OUT", str(out))
    gen.main()
    assert out.read_text() == open(VECTORS).read(), "run: python -m algorithm.gen_testvectors"
