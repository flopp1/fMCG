# fMCG

**Fast MIDI Counter Generator** — renders a video overlay of live note statistics for any MIDI file, from small piano rolls to multi-gigabyte black MIDIs.

It's fast! Renders a notecounter of a 2.3 billion note MIDI in 3 minutes on a Ryzen 5 5500. Has constant RAM usage, so it will NOT load the entire file in memory!

Thanks to Nitsua for the idea of zero copy mmap + other scanner micro optimisations.

## Features

- **Single-pass streaming engine** — the file is walked exactly once (mmap for plain files, chunked libarchive streaming for compressed archives). Memory scales with song *duration*, never with event count.
- **CJCMCG preprocessed files** — `.cjcmcg` files (plain or compressed) are read directly; see *CJCMCG preprocessed files* below.
- **Compressed input** — `.7z`, `.xz`, `.rar`, `.gz`, `.bz2`, `.zst`, `.lz4` and any nesting of these (e.g. `.rar.xz`, `7z inside 7z`) are decompressed on the fly via libarchive; the decompressed image is never materialized on disk or RAM. Decode is pipelined with parsing on a separate thread, and nesting is unwrapped recursively to arbitrary depth.
- **Dear ImGui GUI** — process, preview the video live, and render it — all in-process. The preview shows exactly the frames that get encoded.
- **Built-in renderer** — text is shaped with HarfBuzz (kerning, OpenType features) and drawn by fMCG itself; videos are encoded in-process to **MP4 (H.264, x264)** or **MOV (QuickTime Animation, lossless, optional transparent background)**. No FFmpeg needed.
- **Customizable stats overlay** — one line per stat, any text around tokens, per-stat comma separators, optional leading-zero padding that auto-sizes to each stat's own maximum, corner alignment or an exact x/y position, text and background colours.
- **Start delay** — black lead-in where all stats sit at zero and the current-time fields count up from negative to zero.
- **Parallel parsing** — "Parsing threads" (default: all cores) splits a plain MIDI file's tracks across worker threads; "Encoder threads" (default: auto) controls encoder threads at render time.
- **Processing stats** — live event counter, events/second and elapsed time during the scan, with a smooth progress bar.

## Building

### Windows

1. Install a MinGW-w64 toolchain ([w64devkit](https://github.com/skeeto/w64devkit/releases)) and [Git for Windows](https://git-scm.com/download/win).
2. Run `bootstrap.bat` once — it fetches the Dear ImGui submodule, downloads GLFW 3.4, and downloads the **sources** of HarfBuzz, x264, libarchive 3.8.9 and its codecs (zlib, bzip2, xz, zstd, lz4), each pinned by SHA-256, then builds x264 and libarchive + codecs as static libraries into `vendor/` (a few minutes, once; needs w64devkit's `sh`, `make` and `cmake`).
3. Run `build.bat`. This produces a self-contained `fMCG.exe` (it imports only Windows system DLLs).

Both scripts are idempotent; re-run `build.bat` after changing sources.

`build.bat debug` (or `make debug` on Linux/macOS) builds with `FMCG_DEBUG=1` (a console window plus extra diagnostic logging).

### Linux / macOS

GLFW and libarchive are resolved via pkg-config (i.e. your system package manager):

```bash
# Debian/Ubuntu
sudo apt install build-essential pkg-config git libglfw3-dev libarchive-dev libharfbuzz-dev libx264-dev libgl1-mesa-dev
# Fedora
sudo dnf install gcc-c++ make pkgconf-pkg-config git glfw-devel libarchive-devel harfbuzz-devel x264-devel mesa-libGL-devel   # x264-devel: RPM Fusion
# Arch
sudo pacman -S base-devel pkgconf git glfw libarchive harfbuzz x264 mesa
# macOS
brew install pkg-config glfw libarchive harfbuzz x264

git submodule update --init   # fetch Dear ImGui (pinned v1.92.9b)
make                          # builds ./fMCG (default target)
make debug                    # variant: FMCG_DEBUG diagnostics
make profile                  # -O3 plus debug symbols + frame pointers, for perf
```

`make profile` keeps every optimisation and adds `-g -fno-omit-frame-pointer`, so the optimised binary can be profiled: `perf record --call-graph fp ./fMCG`, then `perf report`. Switching between `make` and `make profile` automatically rebuilds the objects with the new flags (a `.build_mode` stamp tracks the current mode), and the modes combine: `make profile DEBUG=1`.

Dear ImGui is a git submodule compiled from `vendor/imgui` (no standard distro package). To use a system copy instead: `make IMGUI_CFLAGS=-I/usr/include/imgui IMGUI_SOURCES=`.

## Tests

```bash
python -I test/run_tests.py                 # quick: property suite vs oracle, C++ tests, corpus differential
python -I test/run_tests.py --level full    # + layered corpus (needs the private test corpus)
```

The runner needs only Python 3 (standard library) and the same g++/libarchive toolchain as the build; it compiles the headless probe and the C++ tests into `build/test/` itself. `make test LEVEL=quick|corpus|full` does the same on Linux/macOS.

## CJCMCG preprocessed files (`.cjcmcg`)

fMCG also reads the preprocessed files saved by **CJCMCG** (another note counter), plain or inside any supported archive (`song.cjcmcg`, `song.cjcmcg.xz`, a `.7z` holding a `.cjcmcg`, ...). Pick one like a MIDI and press **Process**; every overlay token then works as for a MIDI:

| Available from a `.cjcmcg` | Not in the file |
|---|---|
| notes (`{nc}`, `{nc-total}`, `{nc-rem}`), NPS and peak NPS (fMCG's exact 1 s window), polyphony and peak, tempo / `{bpm}`, time tokens, ticks, `{ppqn}` | CC (`{cc*}` stay 0), lyrics (ignored: not an fMCG stat) |

A `.cjcmcg` keeps only per-tick note-on / note-off **counts** (no pitches, velocities, channels or tracks), so:

- velocity-0 note-ons count as notes (CJCMCG counted every note-on event); **Vel-0 as Note-Off** and **Close held notes** cannot be applied;
- polyphony is note-ons minus note-offs, never below zero (a note-off with nothing held is ignored, like fMCG does for a key that is not held);
- the song ends at the last note / tempo / lyric tick (the file has no End-of-Track events).

For a MIDI without velocity-0 note-ons, a `.cjcmcg` made from it gives the same stats as the MIDI itself. CJCMCG's writer stores a placeholder tempo record at tick 0 (its internal value for 120 BPM, which would otherwise read as ~6 BPM); fMCG recognises it and uses the standard 120 BPM default.

## Output formats

- **MP4 (H.264)** — the default; plays everywhere. Encoded in-process with x264 (BT.709; defaults CRF 18, `veryfast`).
- **MOV (QuickTime Animation)** — lossless RGB, very fast, small for this kind of content; meant for video editors (Premiere, After Effects, Vegas, DaVinci Resolve). With **Transparent background** the frame keeps an alpha channel (32-bit ARGB) so it can be overlaid without keying. Not playable in browsers.

**Encoder settings** (Global column, collapsible; saved globally):

| Setting | Default | Meaning |
|---|---|---|
| Quality (CRF) | 18 | MP4 only. 0..51, lower = better quality and bigger files; 18 is visually lossless for text. 0 is mathematically lossless (High 4:4:4 profile, which many players and browsers cannot play). |
| Speed preset | veryfast | MP4 only. x264 preset, `ultrafast` .. `placebo`: slower presets give smaller files at the same quality but render slower. |
| Tune | None | MP4 only. x264 tune: Animation, Still image, Film, Grain, Fast decode. |
| Keyframe every | 2 s | Both formats. Maximum keyframe distance (0.1..60 s); shorter = faster seeking in editors, slightly bigger files. |

**Reset encoder defaults** restores the table's defaults.

## Running a prebuilt Linux binary

The Linux release binary is built on Ubuntu in WSL and links GLFW, libarchive, HarfBuzz, x264, OpenGL and libstdc++ **dynamically**. You only need the runtime libraries (no `-dev` packages) to run it:

```bash
# Debian/Ubuntu
sudo apt install libglfw3 libarchive13 libharfbuzz0b libx264-165 libgl1   # libx264-NNN: your distro's x264 ABI
# Fedora
sudo dnf install glfw libarchive harfbuzz x264-libs mesa-libGL   # x264-libs: RPM Fusion
# Arch
sudo pacman -S glfw libarchive harfbuzz x264 mesa
```

Notes:

- A desktop with X11 or Wayland is required (the app is a GUI). Under WSL itself, WSLg provides this on Windows 10/11.
- The binary was built against the glibc of the Ubuntu it was built on, so it runs on that Ubuntu or any newer distro. If it fails with a `GLIBC_x.yy not found` error, your distro is older than the build system — build from source instead (section above).
- x264's library name carries its ABI version (`libx264.so.165` here); a distro with a different version needs a build from source.

## Distributing Windows binaries

A built `fMCG.exe` is **self-contained**: libarchive and its compression codecs, HarfBuzz, x264, GLFW and Dear ImGui are all linked statically, and the exe imports only Windows system DLLs (kernel32, user32, gdi32, opengl32, shell32, bcrypt, msvcrt). Copy the exe anywhere and it runs; `release.bat` checks this before packaging.

Everything linked in is under a licence that permits redistribution in binary form, provided the licence notices travel with it:

- libarchive (BSD-2-Clause, with a few per-file notes: UC Regents BSD-3, public domain, and BLAKE2 under CC0 / OpenSSL / Apache 2.0), zlib (zlib licence), bzip2 (BSD-style), liblzma (0BSD), Zstandard (BSD-3-Clause), LZ4 (BSD-2-Clause), HarfBuzz (Old MIT), stb_truetype (public domain / MIT), Dear ImGui (MIT), GLFW (zlib licence);
- x264 is GPL-2.0-or-later, compatible with fMCG's GPL-3.0; `THIRD_PARTY_LICENSES.txt` records the exact x264 source commit.

So a shareable release is just `fMCG.exe` plus the licence files. `THIRD_PARTY_LICENSES.txt` in the repo root is the ready-made notices file, with every library's upstream licence text verbatim for the exact versions built.

**Making a release** — one command builds and packages both platforms:

```bat
release.bat            :: -> dist\fMCG-windows\ + .zip, dist\fMCG-linux\ + .tar.gz
release.bat v1.2       :: -> dist\fMCG-v1.2-windows\, dist\fMCG-v1.2-linux\ ... (name suffix)
debug build            :: build.bat debug (Windows) or make debug (Linux/macOS)
```

The script builds the Windows exe first, assembles `fMCG.exe`, `LICENSE`, `THIRD_PARTY_LICENSES.txt` and `README.md`, verifies nothing is missing and that the exe imports only Windows system DLLs, and zips it via PowerShell. It then builds the Linux binary through **WSL** (`release_linux.sh`, in a temporary Linux-side copy of the sources) and packages it with the same licence files and README as a `.tar.gz`.

**Requirement for the Linux package: WSL** with a Linux distribution and its build packages:

```bat
wsl --install
```

then, inside the distribution (Debian/Ubuntu):

```bash
sudo apt install build-essential pkg-config libglfw3-dev libarchive-dev libharfbuzz-dev libx264-dev libgl1-mesa-dev
```

Without WSL (or with packages missing) the Windows release is still produced and `release.bat` reports the Linux step as an error with what to install. On a Linux/macOS machine, `./release_linux.sh "$PWD" fMCG` (or `make release`) builds the Linux package directly.

## Usage
1. Pick a MIDI file (plain `.mid`, or `.7z`/`.xz`/`.rar`/`.rar.xz` containing one).
2. Adjust settings (see below), then **Process** — the file is scanned and the stats overlay is generated.
3. **Preview** plays the video live in-app; **Render Video** encodes the final video (MP4 or MOV, see *Output formats*) next to the MIDI.

The output is `<midi name>_fMCG.mp4` (or `.mov`) in the MIDI's folder (editable).

## Layout configuration

The layout textbox defines the overlay. **Each line becomes one overlay row**, and a row can contain any mix of tokens and literal text -- so you can put several stats side by side (e.g. `Notes: {nc}  Poly: {plph}  NPS: {nps}`) or keep one stat per line. A default layout is present on startup; **Load from file...** reads a `.txt` layout. The following tokens are substituted per frame:

### Notes
| Token | Meaning |
|---|---|
| `{nc}` | Current notes played |
| `{nc-total}` | Total notes |
| `{nc-rem}` | Notes remaining |

### Control changes
| Token | Meaning |
|---|---|
| `{cc}` | Current CC events |
| `{cc-total}` | Total CC events |
| `{cc-rem}` | CC events remaining |

### Time
| Token | Meaning |
|---|---|
| `{sec}` / `{sec-max}` / `{sec-rem}` | Time in seconds (current / total / remaining) |
| `{time}` / `{time-max}` / `{time-rem}` | Time as `mm:ss` (rolls over to `h:mm:ss`) |
| `{time-milli}` / `{time-milli-max}` / `{time-milli-rem}` | Time as `mm:ss.mmm` |

During the start delay, `{sec}`/`{time}`/`{time-milli}` run from negative and count up to zero.

### Other
| Token | Meaning |
|---|---|
| `{bpm}` | Tempo / beats per minute |
| `{plph}` / `{plph-max}` | Polyphony / maximum polyphony so far |
| `{ppqn}` | PPQ(N) of the file |
| `{nps}` / `{nps-max}` | Notes per second (1s window) / maximum so far |

## Options

The window shows the MIDI file picker on top; below it the **Global settings** (left column: delays, vel-0 / held notes, resolution, FPS, threads, dedup, output format, encoder settings, output path) and the **Pattern** (right column: the overlay look) sit side by side, with the action buttons, progress bar and log underneath. The divider between the two columns can be dragged.

- **Comma separators** — per-stat toggles (Notes / Poly / NPS / CC) to format numbers like `1,234,567`.
- **Leading zeros** — pads every number with zeros so digits line up; each stat pads toward its own maximum (notes → total notes, polyphony → peak polyphony, seconds → total seconds), so no width needs to be set.
- **Fixed-width stats** (with *align* Left / Center / Right) — every stat gets a cell as wide as its widest value anywhere in the video, measured in the chosen font, and the value is aligned inside it. Text after a stat and the width of each line then stay put while digits change. Saved with the pattern.
- **BPM decimals** — how many decimal places `{bpm}` renders (0 = whole numbers like `120`; default 2; capped at 6). Saved with the pattern.
- **Vel-0 as Note-Off** — treat `note-on` with velocity 0 as a note-off (standard MIDI behaviour; disable for unusual files).
- **Counter position** — *Corners* places the block via the Alignment dropdown (**Top/Bottom Left/Right/Center**, 30 px from the edges). *Custom x,y* has two anchors:
  - **Measured from**: the point of the frame (any of 9: corners, edge centres, centre) that x,y count from. From an edge they count inward (from the right edge +x moves left, from the bottom +y moves up); from a centre line +x is right and +y is down. A position measured from a corner stays in that corner when the resolution changes.
  - **Block anchor**: the point of the text block (any of 9) that sits at that position; the block's lines align to its side (left, centre or right). The defaults (Top Left / Top Left) are the classic "top-left of the block at x,y".
- **The text never leaves the frame** — in every mode the block (and its outline) is kept fully on screen. If the largest block the video will ever show is bigger than the frame (minus the corner margins), the font is scaled down once for the whole video so it fits; the preview shows a note and the render log says by how much.
- **Start delay (seconds)** — black lead-in before the song, with stats at zero and a negative time countdown.
- **End delay (seconds)** — tail after the song: event stats freeze at their resting values, time keeps counting and `{tick}` advances at the last tempo received (default 0).
- **Track deduplication** — experimental, **off by default**: before parsing each track, its raw bytes are hashed and compared against tracks already seen; byte-identical repeats replay the cached result instead of being parsed. Helps most on "concatenate-then-compress" files (one MIDI's track duplicated many times, then compressed), where it can skip almost all parse work; on fully unique content it only costs the hashing. Held-note duplicates are never reused (they cannot be replayed exactly), and the replay cache is budget-capped for very large unique files.
- **Risky fast dedup** — opt-in, needs Dedup, layered (xz-inside-xz) files only, **off at every start** (never saved). In each layer, once the first tile repeats, the rest of the layer is *assumed* to be that tile repeated whenever its remaining size is an exact multiple of the tile's size, and is not decoded. It is exact on files built as "header + one tile repeated", and **silently wrong** on files where the rest of a layer is made of different tiles whose sizes happen to add up to a multiple; only use it where that cannot happen. Every assumption is written to the log (layer, header bytes, tile size, multiplicity, bytes skipped), followed by a final reminder that the counts depend on it.
- **Text / Background colour** — click the swatch to open a compact picker with R/G/B sliders and a live preview; the hex value updates in real time.
- **Resolution / FPS / Font** — output video size (16..16384 px; MP4 needs an even width and height), frame rate, and overlay font family + size.
- **Advanced typography** (Pattern column, collapsible; saved with the pattern) — OpenType options applied by the HarfBuzz shaper:
  - **Kerning** / **Ligatures** (on by default) and **Slashed zero** (the font's `zero` feature; fonts whose zero is already slashed may show no change, and many keep other zero styles in stylistic sets or character variants, e.g. Victor Mono `ss02`..`ss05`, Iosevka `cv10=1`..`30`).
  - **Numerals**: font default, *Lining* or *Old-style*; and font default, *Proportional* or *Tabular* width. Tabular digits all have the same width, so counters stop shifting sideways as digits change.
  - **Stylistic sets**: the sets the selected font offers (`ss01`..`ss20`), with the font's own names when it provides them.
  - **More features**: any other OpenType features, space/comma separated (`cv01 salt -calt smcp`, `tag=2` for an alternate); applied last, so they override the options above. Invalid entries are shown in red and ignored.
  - **Letter spacing** (px, may be negative) and **Line spacing** (multiple of the font size).
  
  The block lists the features the selected font actually has, and warns when a chosen option is not in the font (it then has no effect).
- **Preview without a MIDI** — the Preview button always works: with no file processed it shows the current pattern (layout, font, colours, position) with all stats at zero over the chosen background, including the start-delay countdown.

### Patterns and persistent settings

A **pattern** bundles the overlay look: layout text, alignment/position, font family + style + size, advanced typography, text colour, background colour, outline, comma options, padding and BPM decimals. Pick one from the dropdown, **Save** to update it, **Save As...** to create a new one (saving over an existing name asks for confirmation first), **Delete** to remove. Patterns live in the per-user settings folder and survive program updates; switching patterns with unsaved edits asks before discarding.

Everything **not** in a pattern — resolution, FPS, vel-0 handling, delays, threads, output format and encoder settings — is a global setting: saved automatically as you change it (debounced to at most one disk write per second) and restored on the next launch.

All persistent state lives in the per-user config directory (created on first run):

| Platform | Location |
|---|---|
| Windows | `%APPDATA%\fMCG\` |
| Linux | `$XDG_CONFIG_HOME/fMCG/` or `~/.config/fMCG/` |
| macOS | `~/Library/Application Support/fMCG/` |

Inside: `settings.ini` (global settings) and `patterns/<name>.ini` (one file per pattern). The files are plain `key = value` INI — hand-editable, and never touched by program updates. Deleting the folder resets everything; the `Default` pattern is recreated on next launch.

## Project layout

```text
src/         core library modules (fmcg_path, fmcg_util, fmcg_midi, fmcg_engine,
             fmcg_format, fmcg_fonts, fmcg_text, fmcg_video) -- one .h/.cpp pair each
gui/         Dear ImGui application (app_state, dialogs, jobs, preview,
             main, colour_edit, settings_store)
fMCG_core.h  umbrella header: includes every src/ module, nothing else
test/        self-contained test suites + fixture tooling
vendor/      third-party deps prepared by bootstrap.bat (imgui submodule, GLFW, HarfBuzz source,
             static x264 / libarchive + codecs built from pinned sources by build_deps.sh)
```

## Implementation notes

- Single sequential pass over the input; tick-space accumulation with an anchor-interpolated sweep converts events to per-frame stats in O(ticks + frames).
- Plain (uncompressed) MIDI files can additionally parse tracks in parallel: the MTrk chunks are split across worker threads ("Parsing threads", default = all cores) and merged into the same per-tick arrays, whose per-tick accumulation is order-independent, so results are identical to the sequential walk. Compressed inputs (which cannot split) and the two-pass fallback always parse sequentially.
- Rendering is in-process (`src/fmcg_text`, `src/fmcg_video`): per video frame, the overlay text is shaped (HarfBuzz) and rasterized (stb_truetype) into an RGBA canvas, redrawing only the area that changed; the encoders (x264 / QuickTime Animation) only convert or compare that area, and a small ISO-BMFF writer produces the MP4/MOV (64-bit offsets, so files over 4 GB work). The preview uploads the same canvas as a texture.
- Tests: `python -I test/run_tests.py` (see *Tests* above) runs the oracle property suite, the C++ engine/renderer/encoder tests (`test/cpp/`) and the corpus checks; `test/tools/` holds fixture and inspection tooling.

## License

GPL-3.0 — see [LICENSE](LICENSE).
