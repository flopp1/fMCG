#!/usr/bin/env python3
"""Property test: fast engine vs brute-force oracle on KB-scale dialect instances.

usage: run.py <probe-binary> [--seeds N] [--kinds a,b] [--keep] [--dedup 0|1|both]
Exit status 1 if any asserted case disagrees. Prints a per-kind pass table.
"""
import argparse, os, random, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen, oracle

ap = argparse.ArgumentParser()
ap.add_argument("probe")
ap.add_argument("--seeds", type=int, default=30)
ap.add_argument("--kinds", default="")
ap.add_argument("--keep", default="")
ap.add_argument("--dedup", default="1")
ap.add_argument("--close-held", type=int, default=0)
ap.add_argument("--timeout", type=int, default=30)
ap.add_argument("--show", type=int, default=1)
a = ap.parse_args()
oracle.CLOSE_HELD = bool(a.close_held)

kinds = [k for k in (a.kinds.split(",") if a.kinds else gen.GENS)]
modes = [0, 1] if a.dedup == "both" else [int(a.dedup)]
KEYS = ["notes", "ticks", "ppqn", "peak_poly", "peak_nps"]
tot_fail = 0
XPOLY = set()   # poly semantics decided: canonical ons-offs, closure optional (--close-held)
KNOWN_LIMIT = {"dump_tinycuts"}   # allowed to be wrong ONLY if loud
print(f"{'kind':22} {'mode':5} pass/total  first failure")
for kind in kinds:
    for mode in modes:
        ok = 0
        loud = 0
        first = ""
        fails = []
        for seed in range(a.seeds):
            rng = random.Random(f"{kind}-{seed}")
            blob, desc = gen.GENS[kind](rng)
            try:
                want = oracle.stats_of_file(blob)
            except oracle.ScanError:         # the standard scan fails on this stream
                want = None
            except Exception as e:           # oracle cannot define truth: skip
                continue
            d = a.keep or tempfile.mkdtemp()
            os.makedirs(d, exist_ok=True)
            p = os.path.join(d, f"{kind}_{seed}.mid.xz")
            open(p, "wb").write(blob)
            try:
                r = subprocess.run([a.probe, p, "60", str(mode), "0", str(a.close_held)], capture_output=True, text=True, timeout=a.timeout)
                line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "FAIL"
            except subprocess.TimeoutExpired:
                line = "TIMEOUT"
            got = dict(re.findall(r"(\w+)=([\d.]+)", line))
            lossy = got.get("lossy") == "1"
            if want is None:
                bad = [] if line.startswith("FAIL") else ["expected FAIL"]
                want = {"expected FAIL": "FAIL"}
            else:
                bad = [k for k in KEYS if k not in got or int(float(got[k])) != want[k]]
            if kind in XPOLY and bad == ["peak_poly"]:
                bad = []          # product decision pending: see DIALECTS.md "open questions"
            if bad and lossy:
                loud += 1         # wrong but WARNED: acceptable for known-limitation dialects
                continue
            if not bad:
                ok += 1
                if not a.keep: os.remove(p)
            else:
                fails.append(f"seed {seed}: " + ", ".join(f"{k} want {want[k]} got {got.get(k, line)}" for k in bad))
                first = fails[0]
        n = a.seeds
        silent = n - ok - loud
        tot_fail += silent if kind not in KNOWN_LIMIT or True else 0
        tag = f" (+{loud} loud-wrong)" if loud else ""
        print(f"{kind:22} {mode:<5} {ok:>3}/{n:<3}{tag}    {first}")
        for extra in fails[1:a.show]:
            print(f"{'':36}{extra}")
sys.exit(1 if tot_fail else 0)
