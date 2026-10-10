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
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "suites"))


def iter_tests(suite):
    for t in suite:
        if isinstance(t, unittest.TestSuite):
            yield from iter_tests(t)
        else:
            yield t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", choices=["quick", "corpus", "full"], default="quick")
    ap.add_argument("--seeds", type=int, default=None)
    ap.add_argument("-k", dest="pattern", default="")
    ap.add_argument("-v", dest="verbose", action="store_true")
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
    r = unittest.TextTestRunner(verbosity=2 if a.verbose else 1).run(suite)
    sys.exit(0 if r.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
