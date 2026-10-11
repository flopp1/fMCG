// fmcg_fonts.h -- system font discovery: directory scanning, sfnt 'name' and
// 'OS/2' table parsing, family/variant enumeration and file resolution.
#pragma once
#include <string>
#include <vector>

std::vector<std::string> get_font_directories();
void collect_font_files(const std::string& dir, std::vector<std::string>& out, int depth);
std::string read_font_family(const std::string& path);              // first family in the file
std::vector<std::string> read_font_families(const std::string& path);  // every family (TTC: all faces)
std::vector<std::string> enumerate_system_fonts();

// One concrete variant (weight/slant) of a font family: one face of a file.
struct FontVariant {
    std::string file;         // full path to the ttf/otf/ttc
    int         face_index = 0;   // face inside a .ttc collection (0 for ttf/otf)
    std::string style;        // human-readable subfamily, e.g. "Bold", "Light Italic"
    int         weight = 400; // OS/2 usWeightClass (100..900), 0 if unknown
    bool        italic = false;
};

std::vector<FontVariant> enumerate_font_variants(const std::string& family);
// Resolve to a file; *face_index (optional) receives the face inside a .ttc.
std::string find_font_file_for_variant(const std::string& family, const std::string& style, int* face_index = nullptr);
std::string find_font_file_for_family(const std::string& family, int* face_index = nullptr);
