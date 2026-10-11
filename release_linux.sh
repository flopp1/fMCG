#!/usr/bin/env bash
# release_linux.sh <repo-path> <dist-name>
#
# Builds the Linux fMCG_gui inside WSL (or any Linux shell) and packages it as
# dist/<dist-name>-linux/ + dist/<dist-name>-linux.tar.gz. Called by
# release.bat; can also be run directly: ./release_linux.sh "$PWD" fMCG
#
# The build runs in a temporary copy so Linux objects never mix with the
# Windows objects that build.bat leaves in the source tree.
set -euo pipefail

SRC="${1:?usage: release_linux.sh <repo-path> <dist-name>}"
NAME="${2:-fMCG}"

missing=()
for t in g++ make pkg-config tar; do
    command -v "$t" >/dev/null 2>&1 || missing+=("$t")
done
if command -v pkg-config >/dev/null 2>&1; then
    for p in glfw3 libarchive harfbuzz x264; do
        pkg-config --exists "$p" 2>/dev/null || missing+=("$p (development package)")
    done
fi
if [ ${#missing[@]} -gt 0 ]; then
    echo "ERROR: the Linux build environment is missing: ${missing[*]}"
    echo "       Debian/Ubuntu: sudo apt install build-essential pkg-config libglfw3-dev libarchive-dev libharfbuzz-dev libx264-dev libgl1-mesa-dev"
    exit 2
fi
if [ ! -f "$SRC/vendor/imgui/imgui.h" ]; then
    echo "ERROR: vendor/imgui not found. Run bootstrap.bat (or: git submodule update --init) first."
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -r "$SRC/Makefile" "$SRC/fMCG_core.h" "$SRC/src" "$SRC/gui" "$WORK/"
mkdir -p "$WORK/vendor"
cp -r "$SRC/vendor/imgui" "$WORK/vendor/"
rm -f "$WORK"/src/*.o "$WORK"/gui/*.o

echo "  building fMCG_gui (Linux) ..."
if ! make -C "$WORK" -j"$(nproc)" fMCG_gui >"$WORK/build.log" 2>&1; then
    tail -30 "$WORK/build.log"
    echo "ERROR: Linux build failed."
    exit 1
fi

DIST="$SRC/dist/$NAME-linux"
rm -rf "$DIST" "$DIST.tar.gz"
mkdir -p "$DIST"
cp "$WORK/fMCG_gui" "$DIST/"
cp "$SRC/LICENSE" "$SRC/THIRD_PARTY_LICENSES.txt" "$SRC/README.md" "$DIST/"
tar -czf "$DIST.tar.gz" -C "$SRC/dist" "$NAME-linux"
echo "  OK: dist/$NAME-linux and dist/$NAME-linux.tar.gz"
