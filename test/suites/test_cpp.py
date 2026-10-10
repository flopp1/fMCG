"""C++ engine tests (test/cpp/*.cpp), built against the core sources and run.

Each binary returns 0 on success. They are run from the repo root (some write
scratch files under test/_*, which is gitignored).
"""
import os
import subprocess
import unittest

import fmcgtest as T


def run(tc, name, args=(), cwd=None, sources=None, flags=(), timeout=900):
    exe = T.build(name, (sources or T.CORE_FULL) + [f"test/cpp/{name}.cpp"], flags)
    r = subprocess.run([exe] + list(args), cwd=cwd or T.ROOT, capture_output=True, text=True, timeout=timeout)
    tc.assertEqual(r.returncode, 0, f"{name} failed:\n{(r.stdout + r.stderr)[-3000:]}")
    return r


class Cpp(unittest.TestCase):
    def test_newopts(self):
        run(self, "test_newopts")

    def test_twopass_tiny(self):
        run(self, "test_twopass", [os.path.join(T.TEST_DIR, "data", "tiny.mid")])

    def test_twopass_test_mid(self):
        run(self, "test_twopass", [os.path.join(T.TEST_DIR, "data", "test.mid")])

    def test_parallel(self):
        run(self, "test_parallel")

    def test_dedup(self):
        run(self, "test_dedup")

    def test_burst_window(self):
        run(self, "test_burst_window")

    def test_burst_fuzz(self):
        ref = T.build("test_burst_fuzz_ref", T.CORE_FULL + ["test/cpp/test_burst_fuzz.cpp"], ["-DFMCG_NO_BURST"])
        run(self, "test_burst_fuzz", [ref, "40"])

    def test_harness(self):
        run(self, "test_harness", cwd=os.path.join(T.TEST_DIR, "data"))
