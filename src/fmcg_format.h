// fmcg_format.h -- per-frame template rendering and ASS subtitle generation.
#pragma once
#include "fmcg_midi.h"

#include <string>
#include <vector>

// Per-statistic comma separator selection.
struct CommaOpts {
    bool notes     = true;   // {nc} {nc-total} {nc-rem}
    bool polyphony = true;   // {plph} {plph-max}
    bool nps       = true;   // {nps} {nps-max}
    bool cc        = false;  // {cc} {cc-total} {cc-rem}
};

// Padding: a single "pad with leading zeros" checkbox. When on, each numeric
// stat is padded to the digit width of ITS OWN maximum (notes -> total notes,
// polyphony -> peak polyphony, NPS -> peak NPS, CC -> total CC, seconds ->
// total seconds), so every value of a stat lines up without a user-set width.
struct PadOpts {
    bool enabled = false;
};

std::string ProcessTemplateLine(const std::string& line, const FrameStats& fs,
                                uint64_t total_notes, uint64_t total_cc_events,
                                double max_time_sec, uint16_t ppqn,
                                const CommaOpts& commas = {}, PadOpts pad = {});

struct AssConfig {
    int width = 1920;
    int height = 1080;
    double fps = 60.0;
    int font_size = 36;
    std::string font_family = "Arial";
    int bold = 0;               // ASS Style Bold flag (from the chosen variant)
    int italic_flag = 0;        // ASS Style Italic flag
    std::string text_color_ass = "&H00FFFFFF";
    std::string bg_color_ass = "&H00000000";   // video background (&HAABBGGRR)
    int ass_alignment = 7;
    // Counter placement: mode 0 = corner alignment (pos derived from
    // ass_alignment), mode 1 = explicit (pos_x,pos_y) at the text block's
    // TOP-LEFT corner (libass \pos anchors the top-left with alignment 7).
    int pos_mode = 0;
    int pos_x = 30;
    int pos_y = 30;
    CommaOpts commas;
    PadOpts pad;
    double start_delay = 0.0;   // lead-in seconds: zero stats, negative countdown
};

void generate_ass(const std::string& ass_filename, const std::vector<FrameStats>& frames,
                  const std::vector<std::string>& template_lines, uint64_t total_notes,
                  uint64_t total_cc, uint16_t ppqn, const AssConfig& cfg);
