# Building, testing and distributing fMCG

- [Windows](#windows)
- [Linux](#linux)
- [Tests](#tests)
- [Running the prebuilt Linux binary](#running-the-prebuilt-linux-binary)
- [Distributing Windows binaries](#distributing-windows-binaries)
- [Making a release](#making-a-release)
- [Project layout](#project-layout)
- [Implementation notes](#implementation-notes)

## Windows

1. Install a MinGW-w64 toolchain ([w64devkit](https://github.com/skeeto/w64devkit/releases)) and [Git for Windows](https://git-scm.com/download/win).
2. Run `bootstrap.bat` once. It takes a few minutes and needs w64devkit's `sh`, `make` and `cmake`. It does three things:
   - fetches the Dear ImGui submodule;
   - downloads GLFW 3.4 and the **sources** of HarfBuzz, x264, libarchive 3.8.9 and its codecs (zlib, bzip2, xz, zstd, lz4), each pinned by SHA-256;
   - builds x264 and libarchive with its codecs as static libraries into `vendor/`.
3. Run `build.bat`. It produces a self-contained `fMCG.exe` that imports only Windows system DLLs.

Both scripts can be re-run safely. Re-run `build.bat` after changing sources: only what changed is recompiled.

`build.bat debug` builds with `FMCG_DEBUG=1`, which adds a console window and extra diagnostic logging.

## Linux

GLFW, libarchive, HarfBuzz and x264 come from your package manager via pkg-config:

```bash
# Debian/Ubuntu
sudo apt install build-essential pkg-config git libglfw3-dev libarchive-dev liblzma-dev libharfbuzz-dev libx264-dev libgl1-mesa-dev
# Fedora
sudo dnf install gcc-c++ make pkgconf-pkg-config git glfw-devel libarchive-devel xz-devel harfbuzz-devel x264-devel mesa-libGL-devel   # x264-devel: RPM Fusion
# Arch
sudo pacman -S base-devel pkgconf git glfw libarchive harfbuzz x264 mesa

git submodule update --init   # fetch Dear ImGui (pinned v1.92.9b)
make                          # builds ./fMCG (default target)
make debug                    # variant: FMCG_DEBUG diagnostics
make profile                  # -O3 plus debug symbols + frame pointers, for perf
```

- `make profile` keeps every optimisation and adds `-g -fno-omit-frame-pointer`, so the optimised binary can be profiled: run `perf record --call-graph fp ./fMCG`, then `perf report`.
- Switching between `make` and `make profile` rebuilds the objects with the new flags automatically; a `.build_mode` stamp tracks the current mode. The modes combine: `make profile DEBUG=1`.
- Dear ImGui is a git submodule compiled from `vendor/imgui`, because there is no standard distro package. To use a system copy instead, run `make IMGUI_CFLAGS=-I/usr/include/imgui IMGUI_SOURCES=`.

## Tests

```bash
python -I test/run_tests.py                 # quick: property suite vs oracle, C++ tests, corpus differential
python -I test/run_tests.py --level full    # + layered corpus (needs the private test corpus)
```

The runner needs only Python 3 (standard library) and the same g++ and libarchive toolchain as the build. It compiles the headless probe and the C++ tests into `build/test/` itself. On Linux, `make test LEVEL=quick|corpus|full` does the same.

## Running the prebuilt Linux binary

The Linux release binary is built on Ubuntu in WSL. It links GLFW, libarchive, HarfBuzz, x264, OpenGL and libstdc++ **dynamically**, so you need their runtime libraries (no `-dev` packages):

```bash
# Debian/Ubuntu
sudo apt install libglfw3 libarchive13 liblzma5 libharfbuzz0b libx264-165 libgl1   # libx264-NNN: your distro's x264 ABI
# Fedora
sudo dnf install glfw libarchive harfbuzz x264-libs mesa-libGL   # x264-libs: RPM Fusion
# Arch
sudo pacman -S glfw libarchive harfbuzz x264 mesa
```

- The app needs a desktop with X11 or Wayland. Under WSL itself, WSLg provides one on Windows 10 and 11.
- The binary was built against the glibc of the Ubuntu it was built on, so it runs on that Ubuntu or any newer distro. A `GLIBC_x.yy not found` error means your distro is older; build from source instead.
- x264's library name carries its ABI version (`libx264.so.165` here). A distro with a different version needs a build from source.

## Distributing Windows binaries

A built `fMCG.exe` is **self-contained**.

- libarchive and its compression codecs, HarfBuzz, x264, GLFW and Dear ImGui are all linked statically.
- The exe imports only Windows system DLLs: kernel32, user32, gdi32, opengl32, shell32, bcrypt and msvcrt.
- You can copy it anywhere and it runs. `release.bat` checks this before packaging.

Everything linked in may be redistributed in binary form, as long as the licence notices travel with it:

- **Permissive licences:**
  - libarchive (BSD-2-Clause, with a few per-file notes: UC Regents BSD-3, public domain, and BLAKE2 under CC0 / OpenSSL / Apache 2.0);
  - zlib (zlib licence), bzip2 (BSD-style), liblzma (0BSD), Zstandard (BSD-3-Clause), LZ4 (BSD-2-Clause);
  - HarfBuzz (Old MIT), stb_truetype (public domain / MIT), Dear ImGui (MIT), GLFW (zlib licence).
- **x264** is GPL-2.0-or-later, which is compatible with fMCG's GPL-3.0. `THIRD_PARTY_LICENSES.txt` records the exact x264 source commit.

A shareable release is `fMCG.exe` plus the licence files. `THIRD_PARTY_LICENSES.txt` in the repo root is the ready-made notices file, with every library's upstream licence text verbatim for the exact versions built.

## Making a release

One command builds and packages both platforms:

```bat
release.bat            :: -> dist\fMCG-windows\ + .zip, dist\fMCG-linux\ + .tar.gz
release.bat v1.2       :: -> dist\fMCG-v1.2-windows\, dist\fMCG-v1.2-linux\ ... (name suffix)
```

1. **Windows package.** The script builds the exe and assembles `fMCG.exe`, `LICENSE`, `THIRD_PARTY_LICENSES.txt`, `README.md` and the `docs` folder. It checks that nothing is missing and that the exe imports only Windows system DLLs, then zips the folder with PowerShell.
2. **Linux package.** It builds the Linux binary through **WSL** (`release_linux.sh`, run in a temporary Linux-side copy of the sources) and packages it with the same files as a `.tar.gz`.

The Linux package needs **WSL** with a Linux distribution and its build packages:

```bat
wsl --install
```

Then, inside the distribution (Debian/Ubuntu):

```bash
sudo apt install build-essential pkg-config libglfw3-dev libarchive-dev liblzma-dev libharfbuzz-dev libx264-dev libgl1-mesa-dev
```

- Without WSL, or with packages missing, the Windows release is still produced, and `release.bat` reports the Linux step as an error that says what to install.
- On a Linux machine, `./release_linux.sh "$PWD" fMCG` (or `make release`) builds the Linux package directly.

## Project layout

```text
src/         core library modules (fmcg_path, fmcg_util, fmcg_midi, fmcg_engine, fmcg_sweep,
             fmcg_parallel, fmcg_layered, fmcg_cjcmcg, fmcg_format, fmcg_fonts, fmcg_text,
             fmcg_boxes, fmcg_video; fmcg_reader.h = the shared MIDI reader)
gui/         Dear ImGui application (app_state, dialogs, jobs, preview, main, panels,
             pattern_panel, global_panel, colour_edit, settings_store)
fMCG_core.h  umbrella header: includes every src/ module, nothing else
docs/        user and build documentation
test/        self-contained test suites + fixture tooling
vendor/      third-party deps prepared by bootstrap.bat (imgui submodule, GLFW, HarfBuzz source,
             static x264 / libarchive + codecs built from pinned sources by build_deps.sh)
```

## Implementation notes

- **Scan.** The input is read in a single sequential pass, accumulating events in tick space. An anchor-interpolated sweep then converts them to per-frame stats in O(ticks + frames).
- **Parallel parsing.** Plain (uncompressed) MIDI files can parse tracks in parallel.
  - The MTrk chunks are split across worker threads and merged into the same per-tick arrays.
  - Per-tick accumulation does not depend on order, so the results are identical to the sequential walk.
  - Compressed inputs, which cannot be split, and the two-pass fallback always parse sequentially.
- **Rendering** happens in-process (`src/fmcg_text`, `src/fmcg_video`).
  - For each video frame, the overlay text is shaped with HarfBuzz and rasterised with stb_truetype into an RGBA canvas, redrawing only the area that changed.
  - The encoders (x264 and QuickTime Animation) only convert or compare that area.
  - A small ISO-BMFF writer produces the MP4 or MOV, with 64-bit offsets so files over 4 GB work.
  - The preview uploads the same canvas as a texture.
- **Tests.** `test/run_tests.py` runs the oracle property suite, the C++ engine, renderer and encoder tests (`test/cpp/`) and the corpus checks. `test/tools/` holds fixture and inspection tooling.
