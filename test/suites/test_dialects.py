"""Property suite: engine vs brute-force oracle on KB-scale layered constructions.

Every generator in test/dialects/gen.py produces a random xz construction; the
oracle (test/dialects/oracle.py) expands it fully and parses it with the
standard scanner's semantics. The probe must agree exactly in layered mode
(dedup=1, the walk under test) and in standard mode (dedup=0) for the dialects
the standard decoder can express.
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
                bad = {k: (want[k], got.get(k)) for k in T.KEYS if got.get(k) != want[k]}
                tc.assertEqual(bad, {}, f"(want, got) per key; {desc}; kept at {p}")
                os.remove(p)
    oracle.CLOSE_HELD = False


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

    setattr(Dialects, f"test_{kind}_layered", layered)
    setattr(Dialects, f"test_{kind}_layered_close_held", layered_close_held)
    setattr(Dialects, f"test_{kind}_standard", standard)


for _k in gen.GENS:
    _add(_k)
