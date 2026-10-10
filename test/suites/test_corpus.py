"""Real-corpus tests (need FMCG_TEST_FILES).

quick : plain corpus MIDIs, xz-compressed on the fly -> layered == standard.
corpus: every layered corpus file SHRUNK (same unique tiles, <= K repeats per
        run; test/tools/shrink_layered.py) -> layered == standard. This checks
        the walk on the real tile shapes/phases at a size the standard scan
        can still expand.
full  : the full-size layered files vs test/expected/corpus_layered.json.
"""
import json
import lzma
import os
import subprocess
import sys
import unittest

import fmcgtest as T

SHRINK_SKIP = {"When Systematic & Inferno has BSOD.mid.xz.xz.xz": "deferred (damaged members)"}
# Files whose tiles break the MIDI tick spec (> 2^28 ticks): the standard
# reference MUST come from the two-pass path (the single-pass dense arrays
# would need ~29 GB for NoiseMidi), so only the two-pass-exact stats compare.
SPEC_BREAKING = {"NoiseMidi.mid.xz.xz.xz"}


def same(tc, lay, std, l1, l0):
    tc.assertIsNotNone(std, l0)
    tc.assertIsNotNone(lay, l1)
    keys = T.keys_for(lay, std)
    tc.assertEqual({k: lay[k] for k in keys}, {k: std[k] for k in keys}, f"layered {l1} vs standard {l0}")


def cached(name):
    os.makedirs(T.CACHE, exist_ok=True)
    return os.path.join(T.CACHE, name)


@unittest.skipUnless(T.have_corpus(), f"corpus not found at {T.TEST_FILES}")
class PlainDifferential(unittest.TestCase):
    def test_plain_as_xz(self):
        mids = sorted(f for f in os.listdir(T.TEST_FILES) if f.lower().endswith(".mid"))
        self.assertTrue(mids, "no plain .mid files in the corpus root")
        for i, f in enumerate(mids):
            src = os.path.join(T.TEST_FILES, f)
            dst = cached(f"plain_{i}.mid.xz")
            if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
                with open(src, "rb") as fi, open(dst, "wb") as fo:
                    fo.write(lzma.compress(fi.read(), format=lzma.FORMAT_XZ, preset=1))
            std, l0 = T.run_probe(dst, 0)
            lay, l1 = T.run_probe(dst, 1)
            with self.subTest(file=f):
                same(self, lay, std, l1, l0)


@unittest.skipUnless(T.have_corpus() and T.level() >= 1, "corpus level")
class ShrunkLayered(unittest.TestCase):
    K = 3

    def test_shrunk(self):
        files = sorted(os.listdir(T.LAYERED_DIR))
        for f in files:
            with self.subTest(file=f):
                if f in SHRINK_SKIP:
                    self.skipTest(SHRINK_SKIP[f])
                src = os.path.join(T.LAYERED_DIR, f)
                k = 1 if f in SPEC_BREAKING else self.K
                dst = cached(f"shrunk{k}_" + f.replace(" ", "_"))
                if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
                    subprocess.run([sys.executable, "-I", os.path.join(T.TEST_DIR, "tools", "shrink_layered.py"),
                                    src, dst, str(k), "60"], check=True, capture_output=True)
                std, l0 = T.run_probe(dst, 0)
                lay, l1 = T.run_probe(dst, 1)
                if f in SPEC_BREAKING:
                    self.assertEqual(std and std.get("twopass"), 1, f"standard reference must be two-pass: {l0}")
                same(self, lay, std, l1, l0)


@unittest.skipUnless(T.have_corpus() and T.level() >= 2, "full level")
class FullLayered(unittest.TestCase):
    def test_full(self):
        with open(os.path.join(T.TEST_DIR, "expected", "corpus_layered.json")) as fh:
            spec = json.load(fh)
        for name, e in spec["files"].items():
            with self.subTest(file=name):
                if e.get("skip"):
                    self.skipTest(e["skip"])
                p = os.path.join(T.LAYERED_DIR, name)
                self.assertTrue(os.path.exists(p), f"missing corpus file {p}")
                got, line = T.run_probe(p, 1)
                self.assertIsNotNone(got, line)
                want = {k: e[k] for k in T.KEYS}
                self.assertEqual({k: got[k] for k in T.KEYS}, want, line)
                self.assertEqual(got.get("lossy"), 0, "walk reported uncounted data")
