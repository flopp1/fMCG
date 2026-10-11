#!/bin/sh
# build_deps.sh <src-dir> <build-dir> <prefix>
#
# Builds static libarchive for Windows (w64devkit / MinGW-w64) together with
# the compression libraries it uses for the inputs fMCG accepts:
#   zlib (gzip, zip/7z deflate), bzip2, xz/liblzma (xz, lzma, 7z LZMA),
#   zstd, lz4.
# 7z and RAR (v4 + v5) readers are part of libarchive itself. Everything else
# (OpenSSL, libxml2, expat, iconv, ACL/xattr, the bsdtar/bsdcpio/... tools,
# tests) is disabled. Called by bootstrap.bat with the extracted, SHA-256
# verified source trees under <src-dir>; installs headers and lib*.a into
# <prefix>/include and <prefix>/lib. Re-runnable: each build dir is fresh.
set -e
SRC="$1"
BLD="$2"
PFX="$3"
[ -n "$SRC" ] && [ -n "$BLD" ] && [ -n "$PFX" ] || { echo "usage: build_deps.sh <src> <build> <prefix>"; exit 2; }
mkdir -p "$BLD" "$PFX/include" "$PFX/lib"
GEN="MinGW Makefiles"
COMMON="-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PFX -DCMAKE_PREFIX_PATH=$PFX -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5"

cm() {   # cm <name> <source dir> [cmake options...]
    name="$1"; dir="$2"; shift 2
    echo "--- $name"
    rm -rf "$BLD/$name"
    cmake -G "$GEN" -S "$dir" -B "$BLD/$name" $COMMON "$@" > "$BLD/$name.log" 2>&1 \
        || { tail -30 "$BLD/$name.log"; exit 1; }
    cmake --build "$BLD/$name" -j 8 >> "$BLD/$name.log" 2>&1 || { tail -30 "$BLD/$name.log"; exit 1; }
    cmake --install "$BLD/$name" >> "$BLD/$name.log" 2>&1 || { tail -30 "$BLD/$name.log"; exit 1; }
}

cm zlib "$SRC/zlib-1.3.2" -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON -DZLIB_BUILD_TESTING=OFF

echo "--- bzip2"   # no CMake upstream: seven C files
rm -rf "$BLD/bzip2"; mkdir -p "$BLD/bzip2"
for f in blocksort huffman crctable randtable compress decompress bzlib; do
    gcc -O2 -D_FILE_OFFSET_BITS=64 -c "$SRC/bzip2-1.0.8/$f.c" -o "$BLD/bzip2/$f.o"
done
rm -f "$PFX/lib/libbz2.a"
ar rcs "$PFX/lib/libbz2.a" "$BLD"/bzip2/*.o
cp "$SRC/bzip2-1.0.8/bzlib.h" "$PFX/include/"

cm xz "$SRC/xz-5.8.4" -DXZ_NLS=OFF -DXZ_DOC=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF \
   -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF -DXZ_TOOL_SYMLINKS=OFF \
   -DXZ_TOOL_SYMLINKS_LZMA=OFF -DBUILD_TESTING=OFF
cm zstd "$SRC/zstd-1.5.7/build/cmake" -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
   -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_CONTRIB=OFF
cm lz4 "$SRC/lz4-1.10.0/build/cmake" -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF \
   -DBUILD_STATIC_LIBS=ON
cm libarchive "$SRC/libarchive-3.8.9" \
   -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_LIBB2=OFF \
   -DENABLE_LZO=OFF -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_WIN32_XMLLITE=OFF \
   -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF -DENABLE_ICONV=OFF -DENABLE_ACL=OFF \
   -DENABLE_XATTR=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF \
   -DENABLE_TEST=OFF -DENABLE_WERROR=OFF \
   -DENABLE_ZLIB=ON -DENABLE_BZip2=ON -DENABLE_LZMA=ON -DENABLE_ZSTD=ON -DENABLE_LZ4=ON \
   -DBZIP2_INCLUDE_DIR="$PFX/include" -DBZIP2_LIBRARIES="$PFX/lib/libbz2.a" \
   -DZLIB_INCLUDE_DIR="$PFX/include" -DZLIB_LIBRARY="$PFX/lib/libzs.a"
# Every codec must have been found: a missing one would silently drop a format.
for h in HAVE_LIBZ HAVE_LIBBZ2 HAVE_LIBLZMA HAVE_LIBZSTD HAVE_LIBLZ4; do
    grep -q "#define $h 1" "$BLD/libarchive/config.h" || { echo "ERROR: libarchive configured without $h"; exit 1; }
done
echo "--- installed:"
ls "$PFX/lib"
