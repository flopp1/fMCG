// settings_store.h -- persistent global settings and user-defined patterns.
//
// Storage layout (created on first run):
//   Windows : %APPDATA%\fMCG\settings.ini, %APPDATA%\fMCG\patterns\<name>.ini
//   Linux   : $XDG_CONFIG_HOME/fMCG/... (~/.config/fMCG)
//   macOS   : ~/Library/Application Support/fMCG/...
//
// Global settings hold everything EXCEPT what a pattern defines. Patterns
// bundle the overlay appearance: layout text, alignment/position, font
// family+style+size, text colour, background colour, comma options, padding.
// Every file is a flat "key = value" INI, written atomically on each change.
#pragma once
#include "../src/fmcg_format.h"   // CommaOpts, PadOpts

#include <string>
#include <vector>

// The overlay-appearance bundle a pattern captures.
struct PatternData {
    std::string layout_text;
    int         alignment = 0;        // GUI corner index (see alignment_items)
    int         pos_mode = 0;         // 0 = corners, 1 = explicit x/y
    int         pos_x = 30;
    int         pos_y = 30;
    std::string font_family = "Arial";
    std::string font_variant;         // subfamily/style ("" = family default)
    int         font_size = 36;
    std::string text_color_aabbggrr = "&H00FFFFFF";
    std::string bg_color_aabbggrr = "&H00000000";
    CommaOpts   commas;
    PadOpts     pad;
    BpmOpts     bpm;

    bool operator==(const PatternData& o) const {
        return layout_text == o.layout_text && alignment == o.alignment &&
               pos_mode == o.pos_mode && pos_x == o.pos_x && pos_y == o.pos_y &&
               font_family == o.font_family && font_variant == o.font_variant &&
               font_size == o.font_size &&
               text_color_aabbggrr == o.text_color_aabbggrr &&
               bg_color_aabbggrr == o.bg_color_aabbggrr &&
               memcmp_commas(o) && pad.enabled == o.pad.enabled &&
               bpm.decimals == o.bpm.decimals;
    }
private:
    bool memcmp_commas(const PatternData& o) const {
        return commas.notes == o.commas.notes && commas.polyphony == o.commas.polyphony &&
               commas.nps == o.commas.nps && commas.cc == o.commas.cc;
    }
public:
};

// Non-pattern, program-wide settings.
struct GlobalSettings {
    std::string midi_dir;             // last browsed folder (dialog start hint)
    int         width = 1920;
    int         height = 1080;
    double      fps = 60.0;
    bool        vel0_note_off = true;
    double      start_delay = 3.0;
    double      end_delay = 0.0;
    int         ffmpeg_threads = 0;   // 0 = ffmpeg auto-selects encoder threads
    std::string output_dir;           // last output video folder
};

// Returns the per-user config directory (created if missing; "" on failure).
std::string settings_dir();

// Global settings: load returns false (leaving defaults) if no file exists.
bool load_global_settings(GlobalSettings& out);
bool save_global_settings(const GlobalSettings& s);

// Patterns: stored as one INI per pattern inside the patterns/ subfolder.
std::vector<std::string> list_patterns();                 // sorted by name
bool load_pattern(const std::string& name, PatternData& out);
bool save_pattern(const std::string& name, const PatternData& p);   // overwrite allowed
bool delete_pattern(const std::string& name);
bool pattern_exists(const std::string& name);

// Ensure the shipped "Default" pattern exists on first run.
void ensure_default_pattern();

// Sanitise a user-supplied pattern name into a safe file stem ("" if empty).
std::string sanitize_pattern_name(const std::string& name);
