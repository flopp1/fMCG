# AGENTS.md -- read this first (any AI agent / new contributor)

Companion docs in the repo root: `DESIGN.md` (layered-dedup design), `RENDER_PLAN.md` (goal 3 renderer plan + decisions), `DIALECTS.md`
(test constructions), `OPEN_QUESTIONS.md` (decisions + unresolved files), `README.md`
(user-facing build/usage). `CLAUDE.md` just imports this file.

## 1. What this project is
**fMCG = Fast MIDI Counter Generator.** A C++17 Dear ImGui desktop app (Windows primary,
Linux/macOS supported) that takes a MIDI file plus user settings and produces a video of
a live statistics overlay (notes played, NPS, polyphony, BPM, time, CC, ticks...) for
"Black MIDIs" -- MIDI files with millions to *trillions* of notes.

Pipeline today:
1. **Scan** (`src/fmcg_engine.cpp`, `fmcg_stream::process_streaming`): one streaming pass over
   the file (mmap, or libarchive streaming for compressed inputs; decompressed bytes are never
   materialised). Note events go into per-tick cells (`TickData`), tempo events into a list.
2. **Sweep** (`sweep_to_frames`): tick space -> per-frame `FrameStats` (cumulative notes,
   NPS over a 1 s window, polyphony, peaks, BPM, CC).
3. **Format** (`src/fmcg_format.cpp`): layout template (`{nc}`, `{nps}`, ... see README) ->
   per-frame text -> an ASS subtitle file.
4. **Render** (`src/fmcg_render.cpp`, `gui/jobs.cpp`): spawns FFmpeg (lavfi colour bg +
   `subtitles` filter + libx264). Live preview (`gui/preview.cpp`) draws the text with ImGui.

## 2. Repository map
| path | role |
|---|---|
| `src/fmcg_engine.{h,cpp}` | engine (~3.2k lines): data sources, byte streams, MIDI event parser (`parse_track_body`), standard scan, parallel scan, two-pass fallback, tick->frame sweep, `process_streaming` entry |
| `src/fmcg_layered.cpp` | **layered dedup** (~1.4k lines): xz grammar discovery + memoized byte-level transducer (see DESIGN.md) |
| `src/fmcg_engine_internal.h` | engine-private shared types (`TickData`, `TrackSummary`, `scan_layered`) |
| `src/fmcg_midi.{h,cpp}` | `FrameStats`, `ProgressCallbacks`, format detection, mmap reader |
| `src/fmcg_format.{h,cpp}` | template tokens, comma/pad/BPM options, ASS generation |
| `src/fmcg_render.{h,cpp}` | FFmpeg argv construction |
| `src/fmcg_fonts.{h,cpp}` | system font discovery (sfnt name/OS2 parsing) |
| `src/fmcg_path.*`, `src/fmcg_util.*` | path + misc helpers; `src/xxhash64.h` vendored hash |
| `fMCG_core.h` | umbrella header over `src/` |
| `gui/main.cpp` | ImGui window, all settings widgets |
| `gui/jobs.cpp` | worker threads: process (scan + ASS) and render (FFmpeg) |
| `gui/preview.cpp` | in-app live preview |
| `gui/settings_store.*`, `gui/app_state.*` | persisted settings, shared cross-thread state |
| `test/run_tests.py` | **single test entry point** (stdlib unittest; builds everything with g++) |
| `test/suites/` | `test_dialects.py` (oracle property tests), `test_cpp.py` (C++ tests), `test_corpus.py` (plain differential, shrunk + full layered corpus) |
| `test/fmcgtest/` | shared helpers: build, probe runner, paths, levels |
| `test/fmcg_probe.cpp` | headless CLI over the engine (used by all automated tests) |
| `test/dialects/` | generators (`gen.py`), brute-force oracle (`oracle.py`), table CLI (`run.py`) |
| `test/cpp/` | C++ engine tests (newopts, twopass, parallel, dedup, burst_window, burst_fuzz, harness) + benchmarks (bench_throughput, test_tickspace) + ffmpeg/GUI tests not in the runner (test_ffspawn needs imgui, test_double_render needs ffmpeg) |
| `test/tools/` | `shrink_layered.py` (rebuild a layered file with <= K repeats per run), `layered_inspect.py` / `layered_stream_cut.py` (sparse structure dumps of layered files, never full expansion), legacy CSV reference tools |
| `test/data/`, `test/expected/` | small fixture MIDIs; verified full-corpus expected values |
| `build.bat` / `bootstrap.bat` / `release.bat` | Windows build (w64devkit g++); `release.bat` also builds the Linux package through WSL via `release_linux.sh` |
| `Makefile` | Linux/macOS build (pkg-config glfw3 + libarchive) |
| `vendor/` | imgui submodule, GLFW, libarchive (populated by `bootstrap.bat`; gitignored) |

## 3. Building
**Windows (this machine):** w64devkit g++ 16 at `C:\Users\ethen\Downloads\w64devkit\bin`.
`build.bat` -> `fMCG_gui.exe`. Headless probe:
```
g++ -std=c++17 -O2 -I. -Isrc -Ivendor/libarchive src/fmcg_{midi,format,util,path,engine,layered}.cpp \
    test/fmcg_probe.cpp -o build/probe.exe vendor/libarchive/lib/libarchive.dll.a -lpthread \
    -Wl,--stack,0x10000000
cp vendor/libarchive/bin/libarchive.dll build/
```
(256 MB stack: the layered walker recurses deeply.)

**Linux (WSL Ubuntu on this machine, g++ 15, libarchive-dev 3.8.5):** `make` for the GUI;
probe: same sources, link `-larchive -lpthread`, run under `ulimit -s 65536`
(ld ignores `-z stacksize`).

Python: Windows `python` 3.11, WSL `python3` 3.14 (stdlib only; no pytest).
The old walker needed `ulimit -s 65536`; the new one recurses only grammar-depth deep.

## 4. Testing
`python -I test/run_tests.py [--level quick|corpus|full] [-k substr] [--seeds N]` (or `make test LEVEL=...`)
- quick (35 s warm, ~50 s after an engine edit, ~75 s cold): 14 dialects x {layered, layered+close-held,
  standard} vs the oracle (~25 s), C++ tests (~35 s: test_parallel ~11 s, test_burst_fuzz ~10 s),
  plain corpus MIDIs compressed on the fly (layered == standard, <1 s).
- corpus (+~225 s): each layered corpus file shrunk -> layered == standard. Per file (standard + layered):
  NoiseMidi 76 s (two-pass ref; layered side is the 1.43e9-tick sweep), MIDI File 40 s, BSOD 35 s,
  Just 1 note 22 s, gymnopedie 21 s, happy 17 s, Ievans 14 s, paprikas <1 s.
- full (+~100 s): full-size layered files vs `test/expected/corpus_layered.json` (BSOD excluded: ~16 min;
  its verified values are in the JSON for manual runs).
Each run prints the slowest tests (`--times N`). Objects are cached per source in build/test/obj and
compiled in parallel; only sources whose dependencies changed recompile.
Probe: `probe <file> [fps=60] [dedup 0|1] [threads] [close_held 0|1] [spec 0|1]` ->
`OK notes= ticks= ppqn= peak_poly= peak_nps= frames= lossy= twopass=` (lossy=1: a `NOT counted` warning was emitted).
**Spec-breaking files (> 2^28 ticks) must never run on the single-pass standard path in tests** (dense tick
arrays reach tens of GB). The probe's default spec=1 restarts them in two-pass mode (twopass=1); two-pass
peaks are bin-quantized, so tests compare only notes/ticks/ppqn/frames then (`fmcgtest.keys_for`).
The layered walk is exempt (sparse cells + tick runs, no dense arrays).
Debug a layered file without expanding it: `python -I test/tools/layered_inspect.py <file>` (member
tree, runs, leaf prefixes), `layered_stream_cut.py` for payloads too big to buffer.

Test MIDI corpus: `C:\Users\ethen\Downloads\FMCG_TEST_FILES\`
- root: plain `.mid` files (142727, 6.3c-1, FINAL, Spiderweb Test, Yukari Necrofantasia, two Japanese titles)
- `NORMAL_COMPRESSED/`: single-layer archives (`Shinkis Demise ... .7z`, `nut3.7z` 8 GB mid,
  `Deadly apple rev2.rar.xz` 2.6 GB compressed). nut3 / Deadly apple are out of scope for now.
- `LAYERED_COMPRESSED/`: the layered-dedup targets (Ievans Polka, Just 1 note, MIDI File,
  NoiseMidi, When Systematic & Inferno has BSOD, gymnopedie, happy theme of DEATH, paprikas).

**HARD RULE: some of these expand to terabytes. NEVER fully decompress a layered file for
investigation** (no `xz -d` / `7z x` of the whole thing, no full flat stream). Sparse dumps
(per member / per track / per tile summaries, first N bytes, hashes) are fine; per-event dumps
of large data are not.

## 5. Layered dedup -- the hard part (current top priority)
**Construction:** a MIDI file is compressed (xz), the compressed stream is concatenated N
times, the result is compressed again, concatenated again, ... several layers deep. The
*tiles* concatenated at each layer can be ANY consecutive byte range of a MIDI that still
tiles validly: whole files, header+track pieces, single tracks, or raw slices of a track body
cut mid-event (running-status dumps). Expanded size can be TB; compressed is KB-MB.

**Definition of correct:** layered result == standard scan of the fully materialised flat
byte stream (`test/dialects/oracle.py` defines it executably). Not "plausible".
Flat-stream semantics: running status carries across member boundaries; declared MTrk
lengths beyond the data are clamped; note-offs are refcounted per (channel,key); a vel-0
note-on is an off (option); division 0xFFFF = 65535 ppqn; a 12-byte MThd (no division) =>
480 ppqn; multiple MThd overlay into the same tick space; tempo is global.

**Code:** `src/fmcg_layered.cpp` (`scan_layered`). Discovery builds the member grammar (each
unique member decoded once); a byte-level transducer that mirrors the standard scanner runs over
the grammar with per-(node, parser shape) memoized effects and analytic folding of repeats.
No content heuristics. See DESIGN.md.

### Rules that save days
1. **Never debug layered dedup on multi-GB corpus files first.** Reproduce at KB scale in
   `test/dialects/` (add a generator). Every real bug so far reproduced there in ms.
2. **Silent undercount is the worst failure.** Anything the walk cannot interpret must be
   counted in `LayeredStats::skipped_occ` and surface a `WARNING: ... NOT counted` line
   (probe prints `lossy=1`). Never `continue` quietly.
3. No `exit()`/crash switches/semantics-changing env vars in release builds.
4. Per-scan state belongs in `Walk` (fmcg_layered.cpp), not function statics.
5. The oracle, the engine's standard scanner and `Parser` in fmcg_layered.cpp encode the SAME
   parsing rules. Change all three together, then run the suite.
6. Numeric ranges (user decision 2026-10-10): every non-tick stat <= 93,823,560,428,205
   (fits int64/uint64, NOT 32-bit). Ticks: 99% <= 2^28, seen up to 1.4e9, must be handled
   unbounded (uint64). Per-tick dense cells must be 64-bit.
7. Peak-NPS 1-second window is **inclusive of the boundary** (an event exactly 1.0 s earlier
   still counts) -- user decision 2026-10-10.

## 6. Goals (from the user; numbered in the order given)
1. DONE (session 3): keep these root .md files complete enough for any agent to start from scratch.
   Ongoing duty: update them every session.
2. DONE (session 3): layered dedup fixed and verified on all 8 files in
   `FMCG_TEST_FILES/LAYERED_COMPRESSED` (OPEN_QUESTIONS.md table); Windows + Linux builds;
   `test/` consolidated into `test/run_tests.py`. The standard (non-dedup) path stays the reference.
3. NEXT (plan + user decisions in RENDER_PLAN.md): replace ASS+FFmpeg render with an in-built renderer (text + background drawn in-app),
   using only a lightweight encoder + muxer.
4. Typography options under Advanced: kerning, OpenType stylistic sets, old-style numerals, etc.
5. Resolution limits so text never overflows the frame in preview or render (re-check after 3).
6. GUI restructure (added 2026-10-11): the window is too tall. Today `gui/main.cpp` draw_frame stacks
   MIDI File (~L179) -> Pattern section (~L235-571: font, size, layout, colours, commas, BPM decimals,
   counter position) -> Global section (~L572-700: delays, vel-0 / close-held, resolution, FPS,
   threads, output path) -> action buttons -> progress -> log. New layout: MIDI File input area stays
   on top; BELOW it, Global on the LEFT and Pattern on the RIGHT, side by side (e.g. two columns /
   child regions). Do NOT change any individual option inside either group -- only move the groups.
7. Fixed-width cells (added 2026-10-11; AFTER goal 3, since the in-built renderer controls glyph
   placement directly): let a stat occupy a fixed-width cell (e.g. width of its widest possible value,
   or a user-set width, with alignment inside the cell) so several stats on one line no longer shift
   left/right as digit counts and proportional glyph widths change frame to frame.
8. Risky fast dedup (added 2026-10-11): a SEPARATE opt-in checkbox beside "Dedup". Rule: when a
   layer's total size is an exact multiple of its FIRST tile (member) size, assume the whole layer is
   that tile repeated (size / tile) times and skip cutting/decoding the rest of the layer. Show a clear
   warning next to the option: only use it on files where a layer can never be an exact multiple of
   its first tile while being made of different tiles -- otherwise the counts are silently wrong.
   Design notes: this is the one place a silent wrong answer is accepted by the user's choice, so the
   log must state that the assumption was applied (which layer, tile size, multiplicity). Where the
   layer size comes from: the xz index's declared uncompressed size of the parent member (free), so
   the payload never needs decoding past the first tile. Must stay off by default, and the test suite
   must not depend on it (add dialect cases that show both a correct and a wrong-by-design result).

## 7. Status log
- Session 3 (2026-10-10, Claude Code): layered dedup rewritten (fmcg_layered.cpp); all 8
  layered files verified (OPEN_QUESTIONS.md table); test/ restructured into run_tests.py; inclusive
  integer NPS window; tempo-meta window-straddle bug fixed in the standard parser; Windows + Linux
  builds and suites green. BSOD solved (93,715,050,000,000; 16 min, decode-bound). Next: goal 3.
See OPEN_QUESTIONS.md "Corpus status" for the per-file table; update it every session.
