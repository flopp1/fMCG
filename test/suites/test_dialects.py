"""Property suite: engine vs brute-force oracle on KB-scale layered constructions.

Every generator in test/dialects/gen.py produces a random xz construction; the
oracle (test/dialects/oracle.py) expands it fully and parses it with the
standard scanner's semantics. The probe must agree exactly in layered mode
(dedup=1, the walk under test), in standard mode (dedup=0) and with track dedup
(dedup=2) for the dialects the standard decoder can express.
"""
import os
import random
import tempfile
import unittest

import fmcgtest as T
import gen
import oracle

# Mixed-depth constructions (a raw MIDI member next to members whose payload
# is more xz) have no meaning for the standard decoder, which only unwraps a
# stream that STARTS compressed. Only the layered walk defines them.
LAYERED_ONLY = {"tiled_members", "tempo_diff"}


def check_kind(tc, kind, dedup, close_held=0):
    oracle.CLOSE_HELD = bool(close_held)
    tmp = tempfile.mkdtemp(prefix="fmcg_dialect_")
    for seed in range(T.seeds()):
        rng = random.Random(f"{kind}-{seed}")
        blob, desc = gen.GENS[kind](rng)
        try:
            want = oracle.stats_of_file(blob)
        except oracle.ScanError:
            want = None
        except Exception:
            continue                       # oracle cannot define truth
        p = os.path.join(tmp, f"{kind}_{seed}.mid.xz")
        with open(p, "wb") as f:
            f.write(blob)
        got, line = T.run_probe(p, dedup, close_held, timeout=60)
        with tc.subTest(kind=kind, seed=seed, dedup=dedup, close_held=close_held):
            if want is None:
                tc.assertIsNone(got, f"standard scan fails on this stream; probe said {line} ({desc})")
            else:
                tc.assertIsNotNone(got, f"probe failed: {line} ({desc}); kept at {p}")
                keys = [k for k in T.keys_for(got) if k in want]
                bad = {k: (want[k], got.get(k)) for k in keys if got.get(k) != want[k]}
                tc.assertEqual(bad, {}, f"(want, got) per key; {desc}; kept at {p}")
                os.remove(p)
    oracle.CLOSE_HELD = False
    try:
        os.rmdir(tmp)       # empty when every case passed; failing cases stay for inspection
    except OSError:
        pass


class Dialects(unittest.TestCase):
    pass


def _add(kind):
    def layered(self):
        check_kind(self, kind, 1)

    def layered_close_held(self):
        check_kind(self, kind, 1, close_held=1)

    def standard(self):
        if kind in LAYERED_ONLY:
            self.skipTest("mixed-depth construction: layered-only semantics")
        check_kind(self, kind, 0)

    def track(self):   # traditional track dedup over the (nested) decoded stream
        if kind in LAYERED_ONLY:
            self.skipTest("mixed-depth construction: layered-only semantics")
        check_kind(self, kind, 2)

    setattr(Dialects, f"test_{kind}_layered", layered)
    setattr(Dialects, f"test_{kind}_layered_close_held", layered_close_held)
    setattr(Dialects, f"test_{kind}_standard", standard)
    setattr(Dialects, f"test_{kind}_track", track)


for _k in gen.GENS:
    _add(_k)


class RealtimeRunningStatus(unittest.TestCase):
    """A real-time byte (F8..FE) between running-status notes keeps the running
    status; a sysex cancels it. Fixed expectations, not just engine == oracle."""

    def test_counts(self):
        body = (b"\x00\x90\x3c\x40" b"\x00\xf8" b"\x00\x3d\x40" b"\x00\xfe" b"\x00\x3e\x40"
                b"\x00\xf0\x01\xf7" b"\x00\x90\x3f\x40" b"\x00\x40\x40" + gen.EOT)
        f = gen.mthd(480, 1) + gen.mtrk(body)
        self.assertEqual(oracle.stats_of_file(f)["notes"], 5)
        tmp = tempfile.mkdtemp(prefix="fmcg_rt_")
        for name, blob, dedup in (("plain.mid", f, 0), ("layered.mid.xz", gen.xz(f), 1)):
            p = os.path.join(tmp, name)
            with open(p, "wb") as fh:
                fh.write(blob)
            got, line = T.run_probe(p, dedup, timeout=60)
            with self.subTest(name=name):
                self.assertIsNotNone(got, line)
                self.assertEqual(got["notes"], 5, line)
            os.remove(p)
        os.rmdir(tmp)


class RiskyDedup(unittest.TestCase):
    """The opt-in risky fast dedup (goal 8) on constructions where its assumption is
    right, wrong by design, or must not fire. Without the option every case is exact."""

    def _run(self, kind):
        tmp = tempfile.mkdtemp(prefix="fmcg_risky_")
        for seed in range(T.seeds()):
            blob, desc, expect = gen.RISKY[kind](random.Random(f"{kind}-{seed}"))
            want = oracle.stats_of_file(blob)
            p = os.path.join(tmp, f"{kind}_{seed}.mid.xz")
            with open(p, "wb") as f:
                f.write(blob)
            safe, safe_line = T.run_probe(p, 1, timeout=60)
            risky, risky_line = T.run_probe(p, 1, timeout=60, risky=1)
            with self.subTest(kind=kind, seed=seed):
                self.assertIsNotNone(safe, f"probe failed: {safe_line} ({desc})")
                self.assertIsNotNone(risky, f"risky probe failed: {risky_line} ({desc})")
                keys = [k for k in T.keys_for(safe) if k in want]
                self.assertEqual({k: (want[k], safe[k]) for k in keys if safe[k] != want[k]}, {},
                                 f"option off must be exact; {desc}")
                self.assertEqual(safe["assumed"], 0)
                diff = {k: (want[k], risky[k]) for k in keys if risky[k] != want[k]}
                if expect == "exact":
                    self.assertEqual(risky["assumed"], 1, f"assumption should fire; {desc}")
                    self.assertEqual(diff, {}, f"(want, got); {desc}")
                elif expect == "wrong":
                    self.assertEqual(risky["assumed"], 1, f"assumption should fire; {desc}")
                    self.assertNotEqual(diff, {}, f"wrong-by-design case came out exact; {desc}")
                else:
                    self.assertEqual(risky["assumed"], 0, f"assumption must not fire; {desc}")
                    self.assertEqual(diff, {}, f"(want, got); {desc}")
                os.remove(p)
        try:
            os.rmdir(tmp)
        except OSError:
            pass


def _add_risky(kind):
    setattr(RiskyDedup, f"test_{kind}", lambda self: self._run(kind))


for _k in gen.RISKY:
    _add_risky(_k)
