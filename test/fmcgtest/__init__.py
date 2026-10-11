"""Shared helpers for the fMCG test suites (stdlib only).

Builds the headless probe / C++ test binaries with g++ (w64devkit on Windows,
system g++ + libarchive on Linux/macOS) into build/test/, runs them, and parses
the probe's one-line result.

Environment:
  FMCG_TEST_FILES  corpus folder (default: ../FMCG_TEST_FILES next to the repo)
  FMCG_TEST_LEVEL  quick | corpus | full   (set by run_tests.py)
  FMCG_SEEDS       seeds per dialect (default 30)
"""
import concurrent.futures
import os
import re
import hashlib
import subprocess
import sys
import threading
import time

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
CORE_FULL = CORE + ["src/fmcg_fonts.cpp"]
# In-built renderer: HarfBuzz is compiled from the vendored amalgamation on
# Windows and comes from the system (pkg-config) elsewhere.
TEXT = ["src/fmcg_text.cpp", "src/fmcg_video.cpp"] + (["vendor/harfbuzz/src/harfbuzz.cc"] if IS_WIN else [])


def _pkg(args):
    r = subprocess.run(["pkg-config"] + args + ["harfbuzz", "x264"], capture_output=True, text=True)
    return r.stdout.split() if r.returncode == 0 else []


if IS_WIN:
    # libarchive and its codecs are static (bootstrap.bat -> build_deps.sh): no DLL.
    CFLAGS = ["-DLIBARCHIVE_STATIC", "-Ivendor/libarchive", "-Ivendor/harfbuzz/src", "-Ivendor/x264/include"]
    LIBS = ["vendor/libarchive/lib/libarchive.a", "vendor/libarchive/lib/libzstd.a",
            "vendor/libarchive/lib/liblzma.a", "vendor/libarchive/lib/liblz4.a",
            "vendor/libarchive/lib/libbz2.a", "vendor/libarchive/lib/libzs.a", "-lbcrypt",
            "vendor/x264/lib/libx264.a", "-lpthread", "-Wl,--stack,0x10000000"]
else:
    CFLAGS = _pkg(["--cflags"])
    LIBS = ["-larchive"] + _pkg(["--libs"]) + ["-lpthread"]


BUILD_TIMES = {}   # name -> seconds spent compiling/linking (this run)
_OBJ_LOCK = threading.Lock()
_OBJ_DONE = {}     # object path -> True once checked/built this run


def _obj_stale(obj, dep):
    """gcc -MMD depfile: stale if the object/depfile is missing or any dependency is newer."""
    if not (os.path.exists(obj) and os.path.exists(dep)):
        return True
    t = os.path.getmtime(obj)
    text = open(dep).read().replace("\\\n", " ")
    deps = text.split("obj:", 1)[1].split() if "obj:" in text else [dep + ".missing"]
    for d in deps:
        p = d if os.path.isabs(d) else os.path.join(ROOT, d)
        if not os.path.exists(p) or os.path.getmtime(p) > t:
            return True
    return False


def _compile(src, flags):
    """Compile one source to a cached object (per flag set); returns the object path."""
    tag = "_".join(f.strip("-").replace("=", "") for f in flags) or "default"
    # per platform (WSL shares this checkout) and per shared CFLAGS (a changed
    # define must not reuse objects compiled without it)
    cf = hashlib.sha1(" ".join(CFLAGS).encode()).hexdigest()[:8]
    obj_dir = os.path.join(BUILD, "obj", sys.platform, cf, tag)
    stem = os.path.splitext(src.replace("/", "_").replace("\\", "_"))[0]
    obj = os.path.join(obj_dir, stem + ".o")
    dep = os.path.join(obj_dir, stem + ".d")
    with _OBJ_LOCK:
        if _OBJ_DONE.get(obj):
            return obj
    if _obj_stale(obj, dep):
        os.makedirs(obj_dir, exist_ok=True)
        cmd = ["g++", "-std=c++17", "-O2", "-I.", "-Isrc"] + list(flags) + CFLAGS
        cmd += ["-c", src, "-o", obj, "-MMD", "-MF", dep, "-MT", "obj"]   # fixed target: no drive-letter colon
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"compile of {src} failed:\n{r.stderr[-4000:]}")
    with _OBJ_LOCK:
        _OBJ_DONE[obj] = True
    return obj


def build(name, sources, extra_flags=()):
    """Build build/test/<name> from repo-relative sources. Objects are cached per
    source and flag set (shared by every binary) and compiled in parallel; only
    sources whose dependencies changed recompile; the binary relinks when any
    object is newer."""
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, name + EXE)
    t0 = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        objs = list(ex.map(lambda s: _compile(s, tuple(extra_flags)), sources))
    if not os.path.exists(out) or os.path.getmtime(out) < max(os.path.getmtime(o) for o in objs):
        cmd = ["g++", "-o", out] + objs + LIBS
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"link of {name} failed:\n{r.stderr[-4000:]}")
    BUILD_TIMES[name] = BUILD_TIMES.get(name, 0.0) + time.perf_counter() - t0
    return out


def probe():
    return build("probe", CORE + ["test/fmcg_probe.cpp"])


KEYS = ["notes", "ticks", "ppqn", "peak_poly", "peak_nps"]
# The two-pass fallback (used whenever a standard scan crosses the MIDI-spec
# tick limit, 2^28) bins peaks; only these stats are exact there.
TWOPASS_KEYS = ["notes", "ticks", "ppqn", "frames"]


def keys_for(*results):
    """Stats that are comparable between the given probe results."""
    return TWOPASS_KEYS if any(r and r.get("twopass") for r in results) else KEYS


def run_probe(path, dedup, close_held=0, timeout=3600, fps=60, risky=0):
    """Returns (dict of ints incl. 'lossy' / 'assumed', raw line) or (None, line) on FAIL/timeout.
    risky=1 enables the opt-in risky fast dedup (never used by the regular suites)."""
    try:
        # spec=1: a standard scan that breaks the tick spec restarts in two-pass
        # mode instead of allocating multi-GB dense tick arrays.
        r = subprocess.run([probe(), path, str(fps), str(dedup), "0", str(close_held), "1", str(risky)],
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
