// fmcg_format.h -- layout templates: stat tokens, number formatting and the
// per-video-frame text (FrameTextSource) shared by the preview and the renderer.
#pragma once
#include "fmcg_midi.h"
#include "fmcg_text.h"   // TextPiece / TextBlockSpec (plain structs; no renderer dependency)

#include <string>
#include <vector>

// Per-statistic comma separator selection.
struct CommaOpts {
    bool notes     = true;   // {nc} {nc-total} {nc-rem}
    bool polyphony = true;   // {plph} {plph-max}
    bool nps       = true;   // {nps} {nps-max}
    bool cc        = false;  // {cc} {cc-total} {cc-rem}
    bool ticks     = true;   // {tick} {tick-total} {tick-rem}
};

// Padding: a single "pad with leading zeros" checkbox. When on, each numeric
// stat is padded to the digit width of ITS OWN maximum (notes -> total notes,
// polyphony -> peak polyphony, NPS -> peak NPS, CC -> total CC, seconds ->
// total seconds), so every value of a stat lines up without a user-set width.
struct PadOpts {
    bool enabled = false;
};

// {bpm} formatting: number of decimal places, 0..6 (clamped). 0 renders a
// whole number ("120"), the default 2 matches the long-standing output.
struct BpmOpts {
    int decimals = 2;
};

// A layout template line split into literal text and stat tokens ({nc} ->
// token "nc"). Unknown {names} stay literal text.
struct TemplateSegment { std::string text; bool token = false; };
std::vector<TemplateSegment> parse_template_line(const std::string& line);

// One formatted line as pieces: literal text (cell -1) and stat values
// (cell = index of the stat on its line, 0, 1, ...). Concatenated, it is
// exactly ProcessTemplateLine's result.
std::vector<fmcg_text::TextPiece> format_template_line(const std::string& line, const FrameStats& fs,
                                                       uint64_t total_notes, uint64_t total_cc_events,
                                                       double max_time_sec, uint16_t ppqn,
                                                       const CommaOpts& commas = {}, PadOpts pad = {},
                                                       BpmOpts bpm = {}, uint64_t total_ticks = 0);

std::string ProcessTemplateLine(const std::string& line, const FrameStats& fs,
                                uint64_t total_notes, uint64_t total_cc_events,
                                double max_time_sec, uint16_t ppqn,
                                const CommaOpts& commas = {}, PadOpts pad = {},
                                BpmOpts bpm = {}, uint64_t total_ticks = 0);

// Overlay text for every VIDEO frame -- shared by the ASS writer, the preview
// and the in-built renderer so they can never disagree. Video frame k is a
// start-delay lead-in frame (k < lead_in_frames(): zero stats, negative
// time/ticks, starting BPM) or song frame k - lead_in_frames() (the engine's
// frames already include the end-delay tail).
struct FrameTextSource {
    const std::vector<FrameStats>* frames = nullptr;
    std::vector<std::string> template_lines;
    uint64_t total_notes = 0, total_cc = 0, total_ticks = 0;
    uint16_t ppqn = 480;
    double fps = 60.0, start_delay = 0.0, end_delay = 0.0;
    CommaOpts commas;
    PadOpts pad;
    BpmOpts bpm;

    size_t lead_in_frames() const;              // ceil(start_delay * fps), 0 without frames
    size_t frame_count() const;                 // lead-in + song frames
    double song_duration() const;               // MIDI horizon ({time-max}); tail excluded
    FrameStats stats(size_t k) const;
    std::vector<std::string> lines(size_t k) const;
    // The same text as pieces (literals + stat cells), for fixed-width cells.
    fmcg_text::PieceLines pieces(size_t k) const;
    // Pieces for arbitrary stats (pattern-only preview without a processed file).
    fmcg_text::PieceLines pieces_for(const FrameStats& fs, bool lead_in) const;
    // Template pieces + per-stat candidate strings that bound every value each
    // stat takes in this video: what the renderer needs for fixed-width cells
    // and for fitting the largest block into the frame.
    fmcg_text::TextBlockSpec block_spec() const;
};
