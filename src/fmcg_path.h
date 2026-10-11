// fmcg_path.h -- file path helpers and MIDI validation.
#pragma once
#include <cstdio>
#include <filesystem>
#include <string>

// Paths are UTF-8 everywhere in fMCG (the GUI's file dialogs and text inputs
// produce UTF-8). On Windows the narrow C/C++ file APIs use the ANSI code
// page instead, so a name like "最終鬼畜ネット結晶.mid" cannot be opened
// through them on most systems: open files through these helpers.
std::filesystem::path path_from_utf8(const std::string& utf8);
std::string path_to_utf8(const std::filesystem::path& p);
FILE* fopen_utf8(const std::string& utf8, const char* mode);
#if defined(_WIN32)
std::wstring widen_utf8(const std::string& utf8);
#endif

std::string extract_dir(const std::string& path);
std::string extract_stem(const std::string& path);
// Per-app scratch directory inside the OS temp location (Windows: %TEMP%\\fMCG\\,
// POSIX: $TMPDIR or /tmp + /fMCG/), created on first call. Generated ASS files
// live here so users never see (or accidentally delete) them.
std::string app_temp_dir();
std::string trim_path(std::string s);
bool validate_midi(const std::string& path, std::string& err);   // needs fmcg_midi.h for is_compressed_file
