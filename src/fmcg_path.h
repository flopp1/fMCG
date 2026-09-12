// fmcg_path.h -- file path helpers and MIDI validation.
#pragma once
#include <string>

std::string extract_dir(const std::string& path);
std::string extract_stem(const std::string& path);
// Per-app scratch directory inside the OS temp location (Windows: %TEMP%\\fMCG\\,
// POSIX: $TMPDIR or /tmp + /fMCG/), created on first call. Generated ASS files
// live here so users never see (or accidentally delete) them.
std::string app_temp_dir();
std::string trim_path(std::string s);
bool validate_midi(const std::string& path, std::string& err);   // needs fmcg_midi.h for is_compressed_file
