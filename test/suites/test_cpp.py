"""C++ engine tests (test/cpp/*.cpp), built against the core sources and run.

Each binary returns 0 on success. They run from the repo root and write
scratch files (repo root, test/, test/data/, next to the exe); every file a
test creates in those folders is deleted afterwards, pass or fail.
"""
import os
import subprocess
import unittest

import fmcgtest as T


SCRATCH_DIRS = [T.ROOT, T.TEST_DIR, os.path.join(T.TEST_DIR, "data"), T.BUILD]


def _files():
    out = set()
    for d in SCRATCH_DIRS:
        if os.path.isdir(d):
            out |= {os.path.join(d, f) for f in os.listdir(d) if os.path.isfile(os.path.join(d, f))}
    return out


def run(tc, name, args=(), cwd=None, sources=None, flags=(), timeout=900):
    exe = T.build(name, (sources or (T.CORE_FULL + T.TEXT)) + [f"test/cpp/{name}.cpp"], flags)
    before = _files()
    try:
        r = subprocess.run([exe] + list(args), cwd=cwd or T.ROOT, capture_output=True, text=True, timeout=timeout)
    finally:
        for f in _files() - before:      # scratch files the test created, pass or fail
            try:
                os.remove(f)
            except OSError:
                pass
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
        ref = T.build("test_burst_fuzz_ref", T.CORE_FULL + T.TEXT + ["test/cpp/test_burst_fuzz.cpp"], ["-DFMCG_NO_BURST"])
        run(self, "test_burst_fuzz", [ref, "40"])

    def test_text(self):
        run(self, "test_text", sources=T.CORE_FULL + T.TEXT)

    def test_video(self):
        run(self, "test_video", [T.BUILD], sources=T.CORE_FULL + T.TEXT)

    def test_render(self):
        run(self, "test_render", [T.BUILD, "test/data/tempo2.mid"], sources=T.CORE_FULL + T.TEXT)

    def test_harness(self):
        run(self, "test_harness", cwd=os.path.join(T.TEST_DIR, "data"))
