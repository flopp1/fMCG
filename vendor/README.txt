# Vendored third-party dependencies

Populated automatically: on Windows by `bootstrap.bat` (run once after
cloning), on Linux/macOS by `git submodule update --init` for imgui plus your
system package manager for GLFW/libarchive. Nothing here needs to be
committed; each package is pinned so builds are reproducible.

## What ends up here

| Directory     | What                                                              |
|---------------|-------------------------------------------------------------------|
| `imgui/`      | Dear ImGui **git submodule**, pinned to v1.92.9b (sources + backends/) |
| `GLFW/GLFW/`  | GLFW 3.4 headers (Windows only; Linux/macOS use the system copy)  |
| `GLFW/*.a`    | GLFW 3.4 static library for MinGW-w64 (official binary zip)       |
| `libarchive/` | libarchive 3.8.9 headers + import lib + self-contained DLL (Windows only) |

Windows-only pinned download URLs:

- https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.bin.WIN64.zip
- https://github.com/li-ruijie/libarchive/releases/download/v3.8.9/libarchive-v3.8.9-windows-mingw-x64-static.zip

Dear ImGui is fetched on every platform via the git submodule:

    git submodule update --init vendor/imgui

Notes:
- The li-ruijie libarchive package's `libarchive.a` does NOT link statically
  (its dependencies are declared dllimport), so Windows builds link the
  import library and ship the self-contained `libarchive.dll`.
- Licenses: ImGui MIT, GLFW zlib-style, libarchive BSD -- all permissive;
  license texts are inside the repositories/archives.

## Linux / macOS

The Makefile resolves GLFW and libarchive through pkg-config (system package
manager), and compiles Dear ImGui from the submodule at `imgui/`. Fetch it
with:

    git submodule update --init vendor/imgui
