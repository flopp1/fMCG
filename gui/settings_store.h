// settings_store.h -- persistent global settings and user-defined patterns.
//
// Storage layout (created on first run):
//   Windows : %APPDATA%\fMCG\settings.ini, %APPDATA%\fMCG\patterns\<name>.ini
//   Linux   : $XDG_CONFIG_HOME/fMCG/... (~/.config/fMCG)
//
// Global settings hold everything EXCEPT what a pattern defines. Patterns
// bundle the overlay appearance: layout text, alignment/position, font
// family+style+size, text colour, background colour, outline, typography,
// comma options, padding, BPM decimals.
// Every file is a flat "key = value" INI, written atomically on each change.
#pragma once
#include "../src/fmcg_format.h"   // CommaOpts, PadOpts
#include "../src/fmcg_text.h"     // Typography

#include <string>
#include <vector>

// The overlay-appearance bundle a pattern captures.
struct PatternData {
    std::string layout_text;
    int         alignment = 0;        // GUI corner index (see alignment_items)
    int         pos_mode = 0;         // 0 = corners, 1 = explicit x/y
    int         pos_x = 30;
    int         pos_y = 30;
    int         frame_anchor = 0;     // custom position: frame point x,y is measured from (GUI 3x3 index, 0 = top-left)
    int         block_anchor = 0;     // custom position: block point placed there (GUI 3x3 index)
    bool        cells = false;        // fixed-width stat cells
    int         cell_align = 2;       // 0 left, 1 centre, 2 right
    std::string font_family = "Arial";
    std::string font_variant;         // subfamily/style ("" = family default)
    int         font_size = 36;
    std::string text_color_aabbggrr = "&H00FFFFFF";
    std::string bg_color_aabbggrr = "&H00000000";
    bool        outline_enabled = false;     // text outline (in-built renderer), off by default
    double      outline_width = 1.0;         // px at video resolution
    std::string outline_color_aabbggrr = "&H00000000";
    fmcg_text::Typography typo;              // "Advanced typography" (OpenType features, spacing)
    CommaOpts   commas;
    PadOpts     pad;
    BpmOpts     bpm;

    bool operator==(const PatternData& o) const {
        return layout_text == o.layout_text && alignment == o.alignment &&
               pos_mode == o.pos_mode && pos_x == o.pos_x && pos_y == o.pos_y &&
               frame_anchor == o.frame_anchor && block_anchor == o.block_anchor &&
               cells == o.cells && cell_align == o.cell_align &&
               font_family == o.font_family && font_variant == o.font_variant &&
               font_size == o.font_size &&
               text_color_aabbggrr == o.text_color_aabbggrr &&
               bg_color_aabbggrr == o.bg_color_aabbggrr &&
               outline_enabled == o.outline_enabled && outline_width == o.outline_width &&
               outline_color_aabbggrr == o.outline_color_aabbggrr && typo == o.typo &&
               memcmp_commas(o) && pad.enabled == o.pad.enabled &&
               bpm.decimals == o.bpm.decimals;
    }
private:
    bool memcmp_commas(const PatternData& o) const {
        return commas.notes == o.commas.notes && commas.polyphony == o.commas.polyphony &&
               commas.nps == o.commas.nps && commas.cc == o.commas.cc && commas.ticks == o.commas.ticks;
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
    bool        close_held_notes = false;   // release notes still held at a track's end
    double      start_delay = 3.0;
    double      end_delay = 0.0;
    int         ffmpeg_threads = 0;   // encoder (x264) threads, 0 = auto (saved key name kept for compatibility)
    int         parse_threads = 0;    // 0 = all cores (parallel per-track parse); 1 = sequential
    bool        track_dedup = false;  // skip repeated content (track replay / layered dedup)
    int         output_format = 0;    // 0 = MP4 (H.264, x264), 1 = MOV (QuickTime Animation, lossless)
    bool        transparent_bg = false;   // MOV only: alpha-transparent background
    int         x264_crf = 18;               // global encoder settings (MP4): CRF 0..51
    std::string x264_preset = "veryfast";    // fmcg_video::k_presets
    std::string x264_tune;                   // "" = none, else fmcg_video::k_tunes
    double      keyframe_seconds = 2.0;      // both formats
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
