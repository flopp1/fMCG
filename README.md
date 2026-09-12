# fMCG

**Fast MIDI Counter Generator** — renders a video overlay of live note statistics for any MIDI file, from small piano rolls to multi-gigabyte black MIDIs.

It's fast! Renders a notecounter of a 2.3 billion note MIDI in 3 minutes on a Ryzen 5 5500. Has constant RAM usage, so it will NOT load the entire file in memory!

## Features

- **Single-pass streaming engine** — the file is walked exactly once (mmap for plain files, chunked libarchive streaming for compressed archives). Memory scales with song *duration*, never with event count.
- **Compressed input** — `.7z`, `.xz`, `.rar` (including double-compressed `.rar.xz`) are decompressed on the fly via libarchive; the decompressed image is never materialized on disk or RAM. Decode is pipelined with parsing on a separate thread.
- **Dear ImGui GUI** — process, preview the video live, and render with FFmpeg.
- **Customizable stats overlay** — one line per stat, any text around tokens, per-stat comma separators, optional leading-zero padding that auto-sizes to each stat's own maximum, corner alignment or an exact x/y position, text and background colours.
- **Start delay** — black lead-in where all stats sit at zero and the current-time fields count up from negative to zero.
- **Processing stats** — live event counter, events/second and elapsed time during the scan, with a smooth progress bar.

## Building

### Windows

1. Install a MinGW-w64 toolchain ([w64devkit](https://github.com/skeeto/w64devkit/releases)) and [Git for Windows](https://git-scm.com/download/win).
2. Run `bootstrap.bat` once — it fetches the Dear ImGui submodule and downloads the pinned GLFW 3.4 and libarchive 3.8.9 packages into `vendor/`.
3. Run `build.bat`. This produces `fMCG_gui.exe` and copies `libarchive.dll` next to it.

Both scripts are idempotent; re-run `build.bat` after changing sources.

### Linux / macOS

GLFW and libarchive are resolved via pkg-config (i.e. your system package manager):

```bash
# Debian/Ubuntu
sudo apt install build-essential pkg-config git libglfw3-dev libarchive-dev libgl1-mesa-dev
# Fedora
sudo dnf install gcc-c++ make pkgconf-pkg-config git glfw-devel libarchive-devel mesa-libGL-devel
# Arch
sudo pacman -S base-devel pkgconf git glfw libarchive mesa
# macOS
brew install pkg-config glfw libarchive

git submodule update --init   # fetch Dear ImGui (pinned v1.92.9b)
make                          # builds ./fMCG_gui (default target)
```

Dear ImGui is a git submodule compiled from `vendor/imgui` (no standard distro package). To use a system copy instead: `make IMGUI_CFLAGS=-I/usr/include/imgui IMGUI_SOURCES=`.

## Rendering requirement

Rendering uses **FFmpeg** (the `subtitles` filter, i.e. a build with libass) and must be on PATH: `ffmpeg -version`. Any recent [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) full build works on Windows.

## Distributing Windows binaries

A built `fMCG_gui.exe` links libarchive **dynamically** and needs `libarchive.dll` sitting in the same folder (the build copies it there; `bootstrap.bat` fetches it into `vendor\libarchive\bin\`). Without it the exe starts but fails the moment it touches a MIDI file.

**You may redistribute `libarchive.dll` with your build.** It is [BSD-3-Clause](https://opensource.org/license/bsd-3-clause) licensed, which explicitly permits redistribution in binary form, in commercial and non-commercial products alike. The only obligations are:

- include the BSD-3 copyright/licence notice for libarchive (and its embedded components) in your distribution — e.g. a `THIRD_PARTY_LICENSES.txt` shipped alongside the exe;
- do not use the authors' names to promote derived products (the non-endorsement clause).

The same applies to everything else bundled in this DLL: zlib (zlib licence), bzip2 (BSD-style), xz/LZMA (public domain / 0BSD), zstd (BSD), LZ4 (BSD/MIT), libxml2 (MIT) and libiconv (LGPL — dynamically linked inside the DLL, which LGPL permits). ImGui itself is MIT and GLFW's static import is zlib-style; neither adds obligations beyond the licence notice.

So a shareable release is just: `fMCG_gui.exe` + `libarchive.dll` + a licence notice file — the user still supplies FFmpeg themselves (it's GPL on Windows, so it should never be merged into your own distribution anyway; instructing users to install it is the standard and compliant approach).

`THIRD_PARTY_LICENSES.txt` in the repo root is the ready-made notices file covering everything shipped in the binary and the DLL.

**Making a release zip** — one command:

```bat
release.bat            :: -> dist\fMCG-windows\ and dist\fMCG-windows.zip
release.bat v1.2       :: -> dist\fMCG-v1.2-windows\ ... (name suffix)
```

The script builds first (so the exe is current), assembles the folder with `fMCG_gui.exe`, `libarchive.dll`, `THIRD_PARTY_LICENSES.txt` and `README.md`, verifies nothing is missing, and zips it via PowerShell. On Linux/macOS the equivalent is `make release` → `dist/fMCG-linux.tar.gz`.

## Usage
1. Pick a MIDI file (plain `.mid`, or `.7z`/`.xz`/`.rar`/`.rar.xz` containing one).
2. Adjust settings (see below), then **Process** — the file is scanned and the stats overlay is generated.
3. **Preview** plays the video live in-app; **Render Video** encodes the final MP4 next to the MIDI.

The output is `<midi name>_fMCG.mp4` in the MIDI's folder (editable).

## Layout configuration

The layout textbox defines the overlay. **Each line becomes one overlay row**, and a row can contain any mix of tokens and literal text -- so you can put several stats side by side (e.g. `Notes: {nc}  Poly: {plph}  NPS: {nps}`) or keep one stat per line. A default layout is present on startup; **Load from file...** reads a `.txt` layout. The following tokens are substituted per frame:

### Notes
| Token | Meaning |
|---|---|
| `{nc}` | Current notes played |
| `{nc-total}` | Total notes |
| `{nc-rem}` | Notes remaining |

### Control changes (enable "Count CC events" to populate)
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

- **Comma separators** — per-stat toggles (Notes / Poly / NPS / CC) to format numbers like `1,234,567`.
- **Leading zeros** — pads every number with zeros so digits line up; each stat pads toward its own maximum (notes → total notes, polyphony → peak polyphony, seconds → total seconds), so no width needs to be set.
- **Vel-0 as Note-Off** — treat `note-on` with velocity 0 as a note-off (standard MIDI behaviour; disable for unusual files).
- **Count CC events** — enables the `{cc}` stats. Disabled costs nothing; the scanner skips CC tracking entirely.
- **Counter position** — *Corners* places the block via the Alignment dropdown (**Top/Bottom Left/Right/Center**); *Custom x,y* places the **top-left of the text block** at exact video pixels. The valid ranges (`0..width`, `0..height`) are shown next to the fields, and out-of-range values are clamped so the anchor can never leave the frame.
- **Start delay (seconds)** — black lead-in before the song, with stats at zero and a negative time countdown.
- **Text / Background colour** — click the swatch to open a compact picker with R/G/B sliders and a live preview; the hex value updates in real time.
- **Resolution / FPS / Font** — output video size, frame rate, and overlay font family + size.
- **Preview without a MIDI** — the Preview button always works: with no file processed it shows the current pattern (layout, font, colours, position) with all stats at zero over the chosen background, including the start-delay countdown.

### Patterns and persistent settings

A **pattern** bundles the overlay look: layout text, alignment/position, font family + style + size, text colour, background colour, comma options and padding. Pick one from the dropdown, **Save** to update it, **Save As...** to create a new one (saving over an existing name asks for confirmation first), **Delete** to remove. Patterns live in the per-user settings folder and survive program updates; switching patterns with unsaved edits asks before discarding.

Everything **not** in a pattern — resolution, FPS, CC counting, vel-0 handling, start delay — is a global setting: saved automatically as you change it (debounced to at most one disk write per second) and restored on the next launch.

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
             fmcg_format, fmcg_fonts, fmcg_render) -- one .h/.cpp pair each
gui/         Dear ImGui application (app_state, dialogs, jobs, preview,
             main, colour_edit, settings_store)
fMCG_core.h  umbrella header: includes every src/ module, nothing else
test/        self-contained test suites + fixture tooling
vendor/      third-party deps fetched by bootstrap.bat (imgui submodule, GLFW, libarchive)
```

## Implementation notes

- Single sequential pass over the input; tick-space accumulation with an anchor-interpolated sweep converts events to per-frame stats in O(ticks + frames).
- The FFmpeg invocation writes a `.bat`/shell script and renders the ASS from a fixed bare filename (`temp_stats.ass`), because ffmpeg's filter-argument parser mangles backslashes, apostrophes and colons — a user path can never be passed through `subtitles=` safely.
- `test/` holds two self-contained suites plus fixture tooling:
    - `test_harness.cpp` — parse-correctness regression suite (note/poly/NPS/BPM/tempo-map checks on generated fixtures, compressed-vs-plain equivalence for `.7z`/`.tar.xz`, CC counting, vel-0 handling). Run standalone: `test_harness.exe <file.mid>` prints a parse summary; `test_harness.exe --csv` also dumps a comparison CSV.
    - `test_newopts.cpp` — formatting-layer checks (auto-padding, per-stat commas, CC tokens, negative countdown, ASS lead-in, colour conversion).
    - `gen_midi.py` regenerates all fixtures (run it after cloning — the binary `.mid` files are not committed).
    - `ref_csv.py` + `compare_csv.py` diff fMCG's output against an independent Python SMF parser; `check_full_csv.py` validates invariants of a full CSV dump; `extract_tracks.py` slices tracks off huge MIDIs for spot checks.
  On Linux/macOS: `make test` builds and runs both suites. On Windows, compile each `src/*.cpp` to objects (flags in `build.bat`), compile the suite `.cpp`, and link them together against `vendor\libarchive\lib\libarchive.dll.a`; keep `libarchive.dll` beside the exe.

## License

GPL-3.0 — see [LICENSE](LICENSE).
