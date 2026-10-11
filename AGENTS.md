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
   per-video-frame text (`FrameTextSource`: start-delay lead-in, song, end tail).
4. **Render** (`src/fmcg_text.cpp`, `src/fmcg_video.cpp`, `gui/jobs.cpp`): HarfBuzz-shaped text
   rasterized into an RGBA canvas (dirty-rect redraw), encoded in-process to MP4 (x264) or MOV
   (QuickTime Animation, optional alpha) by `fmcg_video::render_video`. The preview
   (`gui/preview.cpp`) shows the same canvas as a GL texture. No FFmpeg, no ASS file
   (`generate_ass` remains only as a tested library function).

## 2. Repository map
| path | role |
|---|---|
| `src/fmcg_engine.{h,cpp}` | engine (~3.2k lines): data sources, byte streams, MIDI event parser (`parse_track_body`), standard scan, parallel scan, two-pass fallback, tick->frame sweep, `process_streaming` entry |
| `src/fmcg_layered.cpp` | **layered dedup** (~1.4k lines): xz grammar discovery + memoized byte-level transducer (see DESIGN.md) |
| `src/fmcg_engine_internal.h` | engine-private shared types (`TickData`, `TrackSummary`, `scan_layered`) |
| `src/fmcg_midi.{h,cpp}` | `FrameStats`, `ProgressCallbacks`, format detection, mmap reader |
| `src/fmcg_format.{h,cpp}` | template tokens, comma/pad/BPM options, ASS generation |
| `src/fmcg_video.{h,cpp}` | encoders (x264 H.264, qtrle) + ISO-BMFF MP4/MOV writer + `render_video` (the Render button's loop) |
| `src/fmcg_text.{h,cpp}` | in-built renderer, text: HarfBuzz shaping + stb_truetype raster (`src/stb_truetype.h`) into an RGBA canvas; `FrameRenderer` redraws only the dirty rect (RENDER_PLAN.md) |
| `src/fmcg_fonts.{h,cpp}` | system font discovery (sfnt name/OS2 parsing) |
| `src/fmcg_path.*`, `src/fmcg_util.*` | path + misc helpers; `src/xxhash64.h` vendored hash |
| `fMCG_core.h` | umbrella header over `src/` |
| `gui/main.cpp` | ImGui window, all settings widgets |
| `gui/jobs.cpp` | worker threads: process (scan) and render (`fmcg_video::render_video`) |
| `gui/preview.cpp` | in-app live preview |
| `gui/settings_store.*`, `gui/app_state.*` | persisted settings, shared cross-thread state |
| `test/run_tests.py` | **single test entry point** (stdlib unittest; builds everything with g++) |
| `test/suites/` | `test_dialects.py` (oracle property tests), `test_cpp.py` (C++ tests), `test_corpus.py` (plain differential, shrunk + full layered corpus) |
| `test/fmcgtest/` | shared helpers: build, probe runner, paths, levels |
| `test/fmcg_probe.cpp` | headless CLI over the engine (used by all automated tests) |
| `test/dialects/` | generators (`gen.py`), brute-force oracle (`oracle.py`), table CLI (`run.py`) |
| `test/cpp/` | C++ engine tests (newopts, twopass, parallel, dedup, burst_window, burst_fuzz, harness) + benchmarks (bench_throughput, test_tickspace) + test_text / test_video / test_render (renderer, encoders with bit-exact qtrle round trips + optional FFmpeg cross-decode, end to end) |
| `test/tools/` | `shrink_layered.py` (rebuild a layered file with <= K repeats per run), `layered_inspect.py` / `layered_stream_cut.py` (sparse structure dumps of layered files, never full expansion), legacy CSV reference tools |
| `test/data/`, `test/expected/` | small fixture MIDIs; verified full-corpus expected values |
| `build.bat` / `bootstrap.bat` / `release.bat` | Windows build (w64devkit g++); `release.bat` also builds the Linux package through WSL via `release_linux.sh` |
| `Makefile` | Linux/macOS build (pkg-config glfw3 + libarchive) |
| `vendor/` | imgui submodule, GLFW, libarchive, HarfBuzz 14.5.0 source, x264 static lib (populated by `bootstrap.bat`, SHA-256 pinned; gitignored). `build_cache/` holds the downloads + NASM/x264 build trees |

## 3. Building
**Windows (this machine):** w64devkit g++ 16 at `C:\Users\ethen\Downloads\w64devkit\bin`.
`build.bat` -> `fMCG_gui.exe`. Headless probe:
```
g++ -std=c++17 -O2 -I. -Isrc -Ivendor/libarchive src/fmcg_{midi,format,util,path,engine,layered}.cpp \
    test/fmcg_probe.cpp -o build/probe.exe vendor/libarchive/lib/libarchive.dll.a -lpthread \
    -Wl,--stack,0x10000000
cp vendor/libarchive/bin/libarchive.dll build/
```
(256 MB stack: the layered walker recurses deeply.) Renderer code also needs `-Ivendor/harfbuzz/src -Ivendor/x264/include`,
`vendor/harfbuzz/src/harfbuzz.cc` (one TU, ~45 s to compile; cached as lib\harfbuzz.o / build/test/obj) and `vendor/x264/lib/libx264.a`.
Linux: `pkg-config --cflags --libs harfbuzz x264` (apt: libharfbuzz-dev libx264-dev).

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
3. DONE (session 3, RENDER_PLAN.md): in-built renderer replaces ASS+FFmpeg -- HarfBuzz + stb_truetype
   text, x264 MP4 (default) / qtrle MOV (optional alpha), shared by preview and render; outline setting.
4. DONE (session 3): typography options. Pattern column > "Advanced typography" (`draw_typography_options`
   in gui/main.cpp): kerning, ligatures, slashed zero, lining/old-style + proportional/tabular numerals,
   the font's stylistic sets (with font-provided names), free-form extra features, letter + line spacing.
   `fmcg_text::Typography` (features() / apply()) -> `TextStyle::features`, `letter_spacing`,
   `line_spacing`; `fmcg_text::font_features()` lists a font's GSUB/GPOS tags. Pattern keys typo_* ,
   letter_spacing, line_spacing. Layout::height() no longer counts line spacing after the last line.
5. Resolution limits so text never overflows the frame in preview or render (re-check after 3).
6. DONE (session 3): GUI restructure. `gui/main.cpp` draw_frame: MIDI File row on top, then a 2-column
   resizable ImGui table -- `draw_global_section()` LEFT, `draw_pattern_section()` RIGHT -- then action
   buttons, progress, log. The options inside each group are unchanged (only moved). Done together with
   the user's request for encoder settings (Global, collapsible "Encoder settings": CRF 0..51, x264
   preset, tune, keyframe interval; `fmcg_video::Options` crf/preset/tune/keyframe_seconds,
   `fmcg_video::k_presets` / `k_tunes`; settings.ini keys x264_crf / x264_preset / x264_tune /
   keyframe_seconds).
7. Fixed-width cells (added 2026-10-11; AFTER goal 3, since the in-built renderer controls glyph
   placement directly): let a stat occupy a fixed-width cell (e.g. width of its widest possible value,
   or a user-set width, with alignment inside the cell) so several stats on one line no longer shift
   left/right as digit counts and proportional glyph widths change frame to frame.
8. Risky fast dedup (added 2026-10-11, rule revised by the user the same day): a SEPARATE opt-in
   checkbox beside "Dedup". Rule: within a layer's payload, skip any leading header member(s) and take
   the FIRST REPEATED tile (the first member that occurs twice in a row). If the remaining payload size
   (layer size minus the header bytes before that tile) is an exact multiple of that tile's size, assume
   the rest of the layer is that tile repeated (remaining / tile) times and stop cutting/decoding the
   layer there. (Matching against the very first member would almost never fire: in every corpus file
   the first member is a small header tile -- 884 / 72 / 172 bytes.) Show a clear warning next to the
   option: only use it on files where the rest of a layer can never be an exact multiple of that tile
   while being made of different tiles -- otherwise the counts are silently wrong.
   Design notes: the one place a silent wrong answer is accepted by the user's choice, so the log must
   state that the assumption was applied (which layer, header bytes, tile size, multiplicity). Layer
   size = the parent member's declared uncompressed size from its xz index (free). Off by default; the
   suite must not depend on it (add dialect cases showing both a correct and a wrong-by-design result).

9. Separate frame-corner anchoring from block anchoring (added 2026-10-11): custom x,y must not force
   the block's TOP-LEFT to be the anchor. Two independent settings: (a) which point of the FRAME the
   position is measured from / which corner preset to use, and (b) which point of the TEXT BLOCK sits on
   that position (top-left, top-centre, top-right, centre-left, centre, ..., bottom-right) -- e.g. a
   custom position anchored by the block's bottom-right. Lines inside the block then align with the
   block anchor's horizontal side. Today (renderer step 2) custom x,y is always the block's top-left
   (`fmcg_text::Placement`, `TextRenderer::place`); extend Placement with a block anchor rather than
   overloading `alignment`. Keep clamping so the whole block stays on screen (goal 5).

## 7. Status log
- Session 3 (2026-10-10, Claude Code): layered dedup rewritten (fmcg_layered.cpp); all 8
  layered files verified (OPEN_QUESTIONS.md table); test/ restructured into run_tests.py; inclusive
  integer NPS window; tempo-meta window-straddle bug fixed in the standard parser; Windows + Linux
  builds and suites green. BSOD solved (93,715,050,000,000; 16 min, decode-bound). Goal 3 done
  (in-built renderer; FFmpeg removed). Goal 6 done + encoder settings in the GUI. Goal 4 done.
  Font discovery fixed: TTC table offsets are file-absolute (every .ttc family was unreadable, e.g.
  Iosevka, Cambria); all families of a collection listed; FontVariant::face_index carried to the
  renderer; per-user font dir no longer scanned twice (duplicate styles).
  Next: goals 5/7/8/9 as the user prioritises.
See OPEN_QUESTIONS.md "Corpus status" for the per-file table; update it every session.
