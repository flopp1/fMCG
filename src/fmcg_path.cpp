#include "fmcg_path.h"
#include <fstream>
#include <cstring>
#include "fmcg_midi.h"

// ---------------------------------------------------------------------------
// File path helpers
// ---------------------------------------------------------------------------

inline std::string extract_dir(const std::string& path) {
    size_t sep = path.find_last_of("/\\");
    return (sep != std::string::npos) ? path.substr(0, sep + 1) : "./";
}

inline std::string extract_stem(const std::string& path) {
    size_t sep = path.find_last_of("/\\");
    std::string basename = (sep != std::string::npos) ? path.substr(sep + 1) : path;
    size_t dot = basename.find_last_of('.');
    return (dot != std::string::npos) ? basename.substr(0, dot) : basename;
}

inline std::string trim_path(std::string s) {
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
    auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && is_space(s.front())) s.erase(s.begin());
    while (!s.empty() && is_space(s.back())) s.pop_back();
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    while (!s.empty() && is_space(s.front())) s.erase(s.begin());
    while (!s.empty() && is_space(s.back())) s.pop_back();
    return s;
}

inline bool validate_midi(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open file"; return false; }
    char sig[4] = {0};
    f.read(sig, 4);
    f.close();
    if (std::memcmp(sig, "MThd", 4) == 0) return true;
    // Accept compressed archives (7z, xz, rar) — validated by libarchive during decompression
    if (is_compressed_file(path)) return true;
    err = "not a valid MIDI file (missing MThd header)";
    return false;
}
