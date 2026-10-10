#!/usr/bin/env python3
"""fMCG test runner -- one entry point for every suite (stdlib unittest only).

  python -I test/run_tests.py                 # quick: dialects + C++ tests + plain corpus differential
  python -I test/run_tests.py --level corpus  # + shrunk layered corpus (layered == standard), ~2 min
  python -I test/run_tests.py --level full    # + full-size layered corpus vs expected values, ~3 min
  python -I test/run_tests.py -k dialect      # only tests whose id contains 'dialect'
  python -I test/run_tests.py --seeds 200     # more random constructions per dialect

The probe and C++ tests are (re)built automatically into build/test/ with g++.
Corpus location: $FMCG_TEST_FILES (default ../FMCG_TEST_FILES next to the repo).
"""
import argparse
import os
import sys
import time
import unittest

sys.dont_write_bytecode = True   # leave no __pycache__ folders in the tree
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "suites"))


def iter_tests(suite):
    for t in suite:
        if isinstance(t, unittest.TestSuite):
            yield from iter_tests(t)
        else:
            yield t


class TimedResult(unittest.TextTestResult):
    """Records wall time per test so the slowest ones can be reported."""
    times = []

    def startTest(self, test):
        self._t0 = time.perf_counter()
        super().startTest(test)

    def stopTest(self, test):
        TimedResult.times.append((time.perf_counter() - self._t0, test.id()))
        super().stopTest(test)


def report_times(fmcgtest, top):
    rows = sorted(TimedResult.times, reverse=True)
    total = sum(t for t, _ in rows)
    builds = sum(fmcgtest.BUILD_TIMES.values())
    print(f"\nTiming: {total:.0f}s in tests (of which {builds:.0f}s compiling C++ binaries)")
    for t, name in rows[:top]:
        print(f"  {t:7.1f}s  {name.split('.', 1)[-1]}")
    if fmcgtest.BUILD_TIMES:
        print("  compile: " + ", ".join(f"{k} {v:.0f}s" for k, v in sorted(fmcgtest.BUILD_TIMES.items(), key=lambda kv: -kv[1])))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", choices=["quick", "corpus", "full"], default="quick")
    ap.add_argument("--seeds", type=int, default=None)
    ap.add_argument("-k", dest="pattern", default="")
    ap.add_argument("-v", dest="verbose", action="store_true")
    ap.add_argument("--times", type=int, default=10, help="show the N slowest tests (0 = off)")
    a = ap.parse_args()
    os.environ["FMCG_TEST_LEVEL"] = a.level
    if a.seeds:
        os.environ["FMCG_SEEDS"] = str(a.seeds)

    import fmcgtest
    print(f"building probe ({'Windows' if fmcgtest.IS_WIN else 'POSIX'}) ...", flush=True)
    fmcgtest.probe()

    loader = unittest.TestLoader()
    suite = loader.discover(os.path.join(HERE, "suites"), pattern="test_*.py", top_level_dir=os.path.join(HERE, "suites"))
    if a.pattern:
        suite = unittest.TestSuite(t for t in iter_tests(suite) if a.pattern.lower() in t.id().lower())
    r = unittest.TextTestRunner(verbosity=2 if a.verbose else 1, resultclass=TimedResult).run(suite)
    if a.times:
        report_times(fmcgtest, a.times)
    sys.exit(0 if r.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
