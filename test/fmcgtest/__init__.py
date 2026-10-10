"""Shared helpers for the fMCG test suites (stdlib only).

Builds the headless probe / C++ test binaries with g++ (w64devkit on Windows,
system g++ + libarchive on Linux/macOS) into build/test/, runs them, and parses
the probe's one-line result.

Environment:
  FMCG_TEST_FILES  corpus folder (default: ../FMCG_TEST_FILES next to the repo)
  FMCG_TEST_LEVEL  quick | corpus | full   (set by run_tests.py)
  FMCG_SEEDS       seeds per dialect (default 30)
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TEST_DIR = os.path.join(ROOT, "test")
BUILD = os.path.join(ROOT, "build", "test")
CACHE = os.path.join(BUILD, "cache")
IS_WIN = os.name == "nt"
EXE = ".exe" if IS_WIN else ""
TEST_FILES = os.path.abspath(os.environ.get("FMCG_TEST_FILES") or os.path.join(ROOT, "..", "FMCG_TEST_FILES"))
LAYERED_DIR = os.path.join(TEST_FILES, "LAYERED_COMPRESSED")

LEVELS = {"quick": 0, "corpus": 1, "full": 2}


def level():
    return LEVELS.get(os.environ.get("FMCG_TEST_LEVEL", "quick"), 0)


def seeds():
    return int(os.environ.get("FMCG_SEEDS", "30"))


CORE = ["src/fmcg_midi.cpp", "src/fmcg_format.cpp", "src/fmcg_util.cpp", "src/fmcg_path.cpp",
        "src/fmcg_engine.cpp", "src/fmcg_layered.cpp"]
CORE_FULL = CORE + ["src/fmcg_fonts.cpp", "src/fmcg_render.cpp"]


def _newest(paths):
    t = 0.0
    for p in paths:
        t = max(t, os.path.getmtime(p))
    return t


def _deps():
    out = []
    for d in ("src", "test", "test/cpp"):
        full = os.path.join(ROOT, d)
        out += [os.path.join(full, f) for f in os.listdir(full) if f.endswith((".h", ".cpp"))]
    out.append(os.path.join(ROOT, "fMCG_core.h"))
    return out


def build(name, sources, extra_flags=()):
    """Compile sources (repo-relative) into build/test/<name>; rebuilds when stale."""
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, name + EXE)
    if os.path.exists(out) and os.path.getmtime(out) >= _newest(_deps()):
        return out
    cmd = ["g++", "-std=c++17", "-O2", "-I.", "-Isrc"] + list(extra_flags)
    if IS_WIN:
        cmd += ["-Ivendor/libarchive"]
    cmd += sources + ["-o", out]
    if IS_WIN:
        cmd += ["vendor/libarchive/lib/libarchive.dll.a", "-lpthread", "-Wl,--stack,0x10000000"]
        dll = os.path.join(BUILD, "libarchive.dll")
        if not os.path.exists(dll):
            shutil.copy(os.path.join(ROOT, "vendor", "libarchive", "bin", "libarchive.dll"), dll)
    else:
        cmd += ["-larchive", "-lpthread"]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"build of {name} failed:\n{r.stderr[-4000:]}")
    return out


def probe():
    return build("probe", CORE + ["test/fmcg_probe.cpp"])


KEYS = ["notes", "ticks", "ppqn", "peak_poly", "peak_nps"]


def run_probe(path, dedup, close_held=0, timeout=3600, fps=60):
    """Returns (dict of ints incl. 'lossy', raw line) or (None, line) on FAIL/timeout."""
    try:
        r = subprocess.run([probe(), path, str(fps), str(dedup), "0", str(close_held)],
                           capture_output=True, text=True, timeout=timeout)
        line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "FAIL"
    except subprocess.TimeoutExpired:
        return None, "TIMEOUT"
    if not line.startswith("OK"):
        return None, line
    return {k: int(float(v)) for k, v in re.findall(r"(\w+)=([\d.]+)", line)}, line


def have_corpus():
    return os.path.isdir(TEST_FILES)


sys.path.insert(0, os.path.join(TEST_DIR, "dialects"))
