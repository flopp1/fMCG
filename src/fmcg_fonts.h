// fmcg_fonts.h -- system font discovery: directory scanning, sfnt 'name' and
// 'OS/2' table parsing, family/variant enumeration and file resolution.
#pragma once
#include <string>
#include <vector>

std::vector<std::string> get_font_directories();
void collect_font_files(const std::string& dir, std::vector<std::string>& out, int depth);
std::string read_font_family(const std::string& path);
std::vector<std::string> enumerate_system_fonts();

// One concrete variant (weight/slant) of a font family: a single file.
struct FontVariant {
    std::string file;         // full path to the ttf/otf/ttc
    std::string style;        // human-readable subfamily, e.g. "Bold", "Light Italic"
    int         weight = 400; // OS/2 usWeightClass (100..900), 0 if unknown
    bool        italic = false;
};

std::vector<FontVariant> enumerate_font_variants(const std::string& family);
std::string find_font_file_for_variant(const std::string& family, const std::string& style);
std::string find_font_file_for_family(const std::string& family);
