// settings_store.h -- persistent global settings and user-defined patterns.
//
// Storage layout (created on first run):
//   Windows : %APPDATA%\fMCG\settings.ini, %APPDATA%\fMCG\patterns\<name>.ini
//   Linux   : $XDG_CONFIG_HOME/fMCG/... (~/.config/fMCG)
//
// Global settings hold everything EXCEPT what a pattern defines. Patterns
// bundle the overlay appearance: boxes (each with its layout text and
// anchor), margin, font family+style+size, text colour, background colour, outline, typography,
// comma options, padding, BPM decimals.
// Every file is a flat "key = value" INI, written atomically on each change.
#pragma once
#include "../src/fmcg_format.h"   // CommaOpts, PadOpts
#include "../src/fmcg_text.h"     // Typography
#include "../src/fmcg_boxes.h"    // boxes

#include <string>
#include <vector>

// The overlay-appearance bundle a pattern captures.
struct PatternData {
    fmcg_boxes::Node boxes;           // the frame split into boxes, each with its layout text
    int         margin = 30;          // inset of every box, px
    bool        cells = false;        // fixed-width stat cells
    int         cell_align = 2;       // 0 left, 1 centre, 2 right
    std::string font_family = "Arial";
    std::string font_variant;         // subfamily/style ("" = family default)
    int         font_size = 36;
    std::string text_color_aabbggrr = "&H00FFFFFF";
    std::string bg_color_aabbggrr = "&H00000000";
    bool        outline_enabled = true;      // text outline (in-built renderer): 1 px black by default
    double      outline_width = 1.0;         // px at video resolution
    std::string outline_color_aabbggrr = "&H00000000";
    fmcg_text::Typography typo;              // "Advanced typography" (OpenType features, spacing)
    CommaOpts   commas;
    PadOpts     pad;
    BpmOpts     bpm;

    bool operator==(const PatternData& o) const {
        return boxes == o.boxes && margin == o.margin &&
               cells == o.cells && cell_align == o.cell_align &&
               font_family == o.font_family && font_variant == o.font_variant &&
               font_size == o.font_size &&
               text_color_aabbggrr == o.text_color_aabbggrr &&
               bg_color_aabbggrr == o.bg_color_aabbggrr &&
               outline_enabled == o.outline_enabled && outline_width == o.outline_width &&
               outline_color_aabbggrr == o.outline_color_aabbggrr && typo == o.typo &&
               commas == o.commas && pad == o.pad && bpm == o.bpm;
    }
    FormatOpts format_opts() const { return FormatOpts{commas, pad, bpm}; }
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
    int         dedup_mode = 0;       // 0 off, 1 track (traditional), 2 layered
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
// The file text without the I/O (tests; load also migrates old keys).
void globals_from_ini(const std::string& text, GlobalSettings& out);
std::string globals_to_ini(const GlobalSettings& s);

// Patterns: stored as one INI per pattern inside the patterns/ subfolder.
std::vector<std::string> list_patterns();                 // sorted by name
// frame_w / frame_h: the video size, used only to convert a custom x,y
// position of a pattern from before boxes into a box.
bool load_pattern(const std::string& name, PatternData& out, int frame_w = 1920, int frame_h = 1080);
// Parse a pattern file's text (load_pattern without the file I/O). Accepts
// files written by every earlier version: missing keys take the defaults,
// and keys whose meaning changed are migrated (see the comments inside).
void pattern_from_ini(const std::string& text, PatternData& out, int frame_w = 1920, int frame_h = 1080);
std::string pattern_to_ini(const std::string& name, const PatternData& p);
bool save_pattern(const std::string& name, const PatternData& p);   // overwrite allowed
bool delete_pattern(const std::string& name);
bool pattern_exists(const std::string& name);

// The built-in layout (Default pattern, "Reset to default", empty layouts).
extern const char* const k_default_layout;
// Layout text -> overlay template lines: one per non-empty line ('\r'
// stripped); an empty layout gives the default layout's lines.
std::vector<std::string> layout_lines(const std::string& text);
// Every box's lines in box order, with the box index of each line (the
// FrameTextSource template). No lines at all = the default layout in box 0.
void pattern_template(const PatternData& p, std::vector<std::string>& lines, std::vector<int>& boxes);

// Ensure the shipped "Default" pattern exists on first run.
void ensure_default_pattern();

// Sanitise a user-supplied pattern name into a safe file stem ("" if empty).
std::string sanitize_pattern_name(const std::string& name);
