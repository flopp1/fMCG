// fmcg_format.h -- layout templates: stat tokens, number formatting and the
// per-video-frame text (FrameTextSource) shared by the preview and the renderer.
#pragma once
#include "fmcg_midi.h"
#include "fmcg_text.h"   // TextPiece / TextBlockSpec (plain structs; no renderer dependency)

#include <cstdint>
#include <string>
#include <vector>

// Per-statistic comma separator selection.
struct CommaOpts {
    bool notes     = true;   // {nc} {nc-total} {nc-rem}
    bool polyphony = true;   // {plph} {plph-max}
    bool nps       = true;   // {nps} {nps-max}
    bool cc        = false;  // {cc} {cc-total} {cc-rem} {cps} {cps-max}
    bool ticks     = true;   // {tick} {tick-total} {tick-rem}
    bool beats     = true;   // {beat*} {measure*}
    bool operator==(const CommaOpts& o) const {
        return notes == o.notes && polyphony == o.polyphony && nps == o.nps && cc == o.cc &&
               ticks == o.ticks && beats == o.beats;
    }
    bool operator!=(const CommaOpts& o) const { return !(*this == o); }
};

// Padding: a single "pad with leading zeros" checkbox. When on, each numeric
// stat is padded to the digit width of ITS OWN maximum (notes -> total notes,
// polyphony -> peak polyphony, NPS -> peak NPS, CC -> total CC, seconds ->
// total seconds, beats -> total beats ...), so every value of a stat lines up
// without a user-set width.
struct PadOpts {
    bool enabled = false;
    bool operator==(const PadOpts& o) const { return enabled == o.enabled; }
    bool operator!=(const PadOpts& o) const { return !(*this == o); }
};

// {bpm} formatting: number of decimal places, 0..6 (clamped). 0 renders a
// whole number ("120"), the default 2 matches the long-standing output.
struct BpmOpts {
    int decimals = 2;
    bool operator==(const BpmOpts& o) const { return decimals == o.decimals; }
    bool operator!=(const BpmOpts& o) const { return !(*this == o); }
};

// Every number-formatting option of a pattern.
struct FormatOpts {
    CommaOpts commas;
    PadOpts   pad;
    BpmOpts   bpm;
    bool operator==(const FormatOpts& o) const { return commas == o.commas && pad == o.pad && bpm == o.bpm; }
    bool operator!=(const FormatOpts& o) const { return !(*this == o); }
};

// Song-wide values the per-frame stats are formatted against (totals,
// remaining values, padding widths).
struct SongTotals {
    uint64_t  notes = 0, cc = 0, ticks = 0;
    double    duration = 0.0;   // song length in seconds ({time-max}, {sec-max})
    uint16_t  ppqn = 480;
    SongMeter meter;
};

// Every layout token; Stat::Literal marks plain text.
enum class Stat : uint8_t {
    Literal,
    Notes, NotesTotal, NotesRem, Cc, CcTotal, CcRem, Tick, TickTotal, TickRem,
    Sec, SecMax, SecRem, Time, TimeMax, TimeRem, TimeMilli, TimeMilliMax, TimeMilliRem,
    Bpm, Ppqn, Poly, PolyMax, Nps, NpsMax, Cps, CpsMax,
    Beat, BeatTotal, BeatRem, Measure, MeasureTotal, MeasureRem, MeasureBeat, TimeSig,
};

// A layout template line split into literal text and stat tokens ("{nc}" ->
// Stat::Notes), resolved once so formatting a frame does no name lookups.
// Unknown {names} stay literal text.
struct TemplateSegment {
    std::string text;            // the literal text (empty for a stat)
    Stat stat = Stat::Literal;
    bool is_stat() const { return stat != Stat::Literal; }
};
std::vector<TemplateSegment> parse_template_line(const std::string& line);

// One formatted line as pieces: literal text (cell -1) and stat values
// (cell = index of the stat on its line, 0, 1, ...).
std::vector<fmcg_text::TextPiece> format_template_segments(const std::vector<TemplateSegment>& segs,
                                                           const FrameStats& fs, const SongTotals& song,
                                                           const FormatOpts& fmt);
std::vector<fmcg_text::TextPiece> format_template_line(const std::string& line, const FrameStats& fs,
                                                       const SongTotals& song, const FormatOpts& fmt = {});
// The same line as one string.
std::string format_line(const std::string& line, const FrameStats& fs, const SongTotals& song,
                        const FormatOpts& fmt = {});

// Overlay text for every VIDEO frame -- shared by the preview and the
// in-built renderer so they can never disagree. Video frame k is a
// start-delay lead-in frame (k < lead_in_frames(): zero stats, negative
// time/ticks, starting BPM) or song frame k - lead_in_frames() (the engine's
// frames already include the end-delay tail).
struct FrameTextSource {
    const std::vector<FrameStats>* frames = nullptr;
    std::vector<std::string> template_lines;
    // Per template line: the box it is drawn in (fmcg_boxes leaf index;
    // TextBlockSpec::boxes). Empty = every line in box 0.
    std::vector<int> template_boxes;
    SongTotals song;                            // song.duration is ignored: see song_duration()
    FormatOpts fmt;
    double fps = 60.0, start_delay = 0.0, end_delay = 0.0;

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
    // template_boxes, one entry per template line (missing entries = 0).
    std::vector<int> line_boxes() const;

private:
    // template_lines split once (re-split only when the lines change); the
    // render loop asks for every frame. Not shared across threads: each
    // render / preview owns its FrameTextSource copy.
    const std::vector<std::vector<TemplateSegment>>& parsed() const;
    mutable std::vector<std::string> parsed_from_;
    mutable std::vector<std::vector<TemplateSegment>> parsed_;
    mutable bool parsed_ok_ = false;
};
