#include "fmcg_path.h"
#include <fstream>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include "fmcg_midi.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// UTF-8 file paths
// ---------------------------------------------------------------------------

#if defined(_WIN32)
std::wstring widen_utf8(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return std::wstring(utf8.begin(), utf8.end());   // not UTF-8: best effort
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0], n);
    return w;
}
#endif

fs::path path_from_utf8(const std::string& utf8) {
#if defined(_WIN32)
    return fs::path(widen_utf8(utf8));
#else
    return fs::path(utf8);
#endif
}

std::string path_to_utf8(const fs::path& p) {
#if defined(_WIN32)
    const std::wstring& w = p.native();
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
#else
    return p.string();
#endif
}

FILE* fopen_utf8(const std::string& utf8, const char* mode) {
#if defined(_WIN32)
    const std::string m = mode;
    return _wfopen(widen_utf8(utf8).c_str(), std::wstring(m.begin(), m.end()).c_str());
#else
    return std::fopen(utf8.c_str(), mode);
#endif
}

// ---------------------------------------------------------------------------
// File path helpers
// ---------------------------------------------------------------------------

std::string extract_dir(const std::string& path) {
    size_t sep = path.find_last_of("/\\");
    return (sep != std::string::npos) ? path.substr(0, sep + 1) : "./";
}

std::string extract_stem(const std::string& path) {
    size_t sep = path.find_last_of("/\\");
    std::string basename = (sep != std::string::npos) ? path.substr(sep + 1) : path;
    size_t dot = basename.find_last_of('.');
    return (dot != std::string::npos) ? basename.substr(0, dot) : basename;
}

bool validate_midi(const std::string& path, std::string& err) {
    std::ifstream f(path_from_utf8(path), std::ios::binary);
    if (!f) { err = "cannot open file"; return false; }
    char sig[4] = {0};
    f.read(sig, 4);
    f.close();
    if (std::memcmp(sig, "MThd", 4) == 0) return true;
    // Accept compressed archives (7z, xz, rar) — validated by libarchive during decompression
    if (is_compressed_file(path)) return true;
    // CJCMCG preprocessed files have no signature: accepted by name and fully
    // validated by the engine's reader (src/fmcg_cjcmcg.cpp).
    std::string low = path;
    for (char& c : low) c = (char)std::tolower((unsigned char)c);
    if (low.size() >= 7 && low.compare(low.size() - 7, 7, ".cjcmcg") == 0) return true;
    err = "not a valid MIDI file (missing MThd header)";
    return false;
}
