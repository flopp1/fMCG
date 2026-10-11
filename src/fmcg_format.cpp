#include "fmcg_format.h"
#include "fmcg_util.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Text-block extent estimation for custom-position clamping.
//
// libass font metrics are unavailable here (no font file is loaded in this
// translation unit), so extents are estimated conservatively: monospace-ish
// advance of 0.62em per character, line height 1.5x the font size. These are
// upper-bound-ish for typical stat strings (digits are tabular and near the
// em width; the estimate errs wide so the clamp never allows visible
// clipping). The same constants drive the preview's clamp in gui/preview.cpp
// so both surfaces agree.
// ---------------------------------------------------------------------------
static constexpr double k_char_em   = 0.62;   // advance width per char, in em
static constexpr double k_line_em   = 1.50;   // line height, in em

struct TextExtent {
    int max_line_w = 0;   // widest rendered row, in pixels
    int lines = 0;
};

static TextExtent measure_block(const std::vector<std::string>& lines, int font_size) {
    TextExtent e;
    e.lines = (int)std::max<size_t>(1, lines.size());
    for (const auto& l : lines) {
        int w = (int)std::ceil(l.size() * k_char_em * font_size);
        if (w > e.max_line_w) e.max_line_w = w;
    }
    return e;
}

static int block_height(int lines, int font_size) {
    return (int)std::ceil(lines * k_line_em * font_size);
}

std::string ProcessTemplateLine(const std::string& line, const FrameStats& fs,
                                       uint64_t total_notes, uint64_t total_cc_events,
                                       double max_time_sec, uint16_t ppqn,
                                       const CommaOpts& commas, PadOpts pad, BpmOpts bpm,
                                       uint64_t total_ticks) {
    std::string result = line;
    auto replace = [&](const std::string& token, const std::string& val) {
        size_t pos = 0;
        while ((pos = result.find(token, pos)) != std::string::npos) {
            result.replace(pos, token.length(), val);
            pos += val.length();
        }
    };

    const bool p = pad.enabled;
    // Digit widths: each stat pads toward its own running maximum.
    auto w = [](uint64_t v) { int n = 1; while (v >= 10) { v /= 10; ++n; } return n; };
    const int w_notes = p ? w(total_notes) : 0;
    const int w_cc    = p ? w(total_cc_events) : 0;
    const int w_poly  = p ? w(static_cast<uint64_t>(std::max<int64_t>(0, fs.peak_polyphony))) : 0;
    const int w_nps   = p ? w(static_cast<uint64_t>(std::max<double>(0, std::round(fs.peak_nps)))) : 0;
    const int w_sec   = p ? w(static_cast<uint64_t>(std::max<double>(0, max_time_sec))) : 0;
    const int w_tick  = p ? w(total_ticks) : 0;

    auto fmt = [&](bool commas_on, uint64_t v, int width) -> std::string {
        // Pad the raw digits first, then group with commas, so the pad always
        // reflects digit count.
        std::string s = std::to_string(v);
        if (width > 0 && (int)s.size() < width)
            s.insert(0, (size_t)width - s.size(), '0');
        if (commas_on) {
            std::string rev;
            int count = 0;
            for (int i = (int)s.size() - 1; i >= 0; --i) {
                rev.push_back(s[i]);
                if (++count % 3 == 0 && i > 0) rev.push_back(',');
            }
            s.assign(rev.rbegin(), rev.rend());
        }
        return s;
    };
    auto fmt_notes = [&](uint64_t v) { return fmt(commas.notes, v, w_notes); };
    auto fmt_poly  = [&](uint64_t v) { return fmt(commas.polyphony, v, w_poly); };
    auto fmt_nps   = [&](uint64_t v) { return fmt(commas.nps, v, w_nps); };
    auto fmt_cc    = [&](uint64_t v) { return fmt(commas.cc, v, w_cc); };
    auto fmt_tick  = [&](uint64_t v) { return fmt(commas.ticks, v, w_tick); };

    replace("{nc}", fmt_notes(fs.cumulative_notes));
    replace("{nc-total}", fmt_notes(total_notes));
    replace("{nc-rem}", fmt_notes(total_notes > fs.cumulative_notes ? total_notes - fs.cumulative_notes : 0));

    replace("{cc}", fmt_cc(fs.cumulative_cc));
    replace("{cc-total}", fmt_cc(total_cc_events));
    replace("{cc-rem}", fmt_cc(total_cc_events > fs.cumulative_cc ? total_cc_events - fs.cumulative_cc : 0));

    // Ticks: signed so the start-delay countdown can run negative and count
    // up (mirrors the {sec}/{time} fields).
    auto fmt_tick_signed = [&](int64_t v) -> std::string {
        if (v >= 0) return fmt_tick((uint64_t)v);
        std::string s = fmt_tick((uint64_t)(-v));
        return "-" + s;
    };

    replace("{tick}", fmt_tick_signed(fs.tick));
    replace("{tick-total}", fmt_tick(total_ticks));
    replace("{tick-rem}", fmt_tick((uint64_t)std::max<int64_t>(0, (int64_t)total_ticks - fs.tick)));

    // Signed seconds, truncating toward zero (start-delay countdown runs
    // negative and counts up to 0).
    auto fmt_sec = [&](double v) -> std::string {
        long long iv = static_cast<long long>(v);   // truncates toward zero
        bool neg = iv < 0;
        std::string s = std::to_string(neg ? (uint64_t)(-iv) : (uint64_t)iv);
        if (w_sec > 0 && (int)s.size() < w_sec)
            s.insert(0, (size_t)w_sec - s.size(), '0');
        return neg ? "-" + s : s;
    };

    replace("{sec}", fmt_sec(fs.timestamp_sec));
    replace("{sec-max}", fmt_sec(max_time_sec));
    replace("{sec-rem}", fmt_sec(max_time_sec > fs.timestamp_sec ? max_time_sec - fs.timestamp_sec : 0));

    replace("{time}", format_time(fs.timestamp_sec, false));
    replace("{time-max}", format_time(max_time_sec, false));
    replace("{time-rem}", format_time(max_time_sec > fs.timestamp_sec ? max_time_sec - fs.timestamp_sec : 0, false));

    replace("{time-milli}", format_time(fs.timestamp_sec, true));
    replace("{time-milli-max}", format_time(max_time_sec, true));
    replace("{time-milli-rem}", format_time(max_time_sec > fs.timestamp_sec ? max_time_sec - fs.timestamp_sec : 0, true));

    std::ostringstream bpm_ss;
    int bdec = bpm.decimals;
    if (bdec < 0) bdec = 0;
    if (bdec > 6) bdec = 6;   // cap: beyond 6 decimals is display noise
    bpm_ss << std::fixed << std::setprecision(bdec) << fs.bpm;

    replace("{bpm}", bpm_ss.str());
    replace("{ppqn}", std::to_string(ppqn));

    replace("{plph}", fmt_poly(static_cast<uint64_t>(std::max<int64_t>(0, fs.polyphony))));
    replace("{plph-max}", fmt_poly(static_cast<uint64_t>(std::max<int64_t>(0, fs.peak_polyphony))));

    uint64_t round_nps = static_cast<uint64_t>(std::round(std::max<double>(0, fs.notes_per_second)));
    uint64_t round_peak_nps = static_cast<uint64_t>(std::round(std::max<double>(0, fs.peak_nps)));
    replace("{nps}", fmt_nps(round_nps));
    replace("{nps-max}", fmt_nps(round_peak_nps));

    return result;
}

void generate_ass(const std::string& ass_filename, const std::vector<FrameStats>& frames,
                          const std::vector<std::string>& template_lines, uint64_t total_notes,
                          uint64_t total_cc, uint16_t ppqn, const AssConfig& cfg,
                          uint64_t total_ticks) {
    std::ofstream ass(ass_filename);
    ass << "[Script Info]\nScriptType: v4.00+\nPlayResX: " << cfg.width << "\nPlayResY: " << cfg.height << "\n\n";
    ass << "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n";
    ass << "Style: Default," << cfg.font_family << "," << cfg.font_size << "," << cfg.text_color_ass << "," << cfg.text_color_ass << ",&H00000000,&H80000000," << cfg.bold << "," << cfg.italic_flag << ",0,0,100,100,0,0,1,1,0," << cfg.ass_alignment << ",30,30,30,1\n\n";
    ass << "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

    int pos_x = 30, pos_y = 30;
    if (cfg.pos_mode == 1) {
        // Explicit placement: \pos anchors the text block's TOP-LEFT corner
        // (alignment 7). Clamp so the ANCHOR stays on the frame.
        pos_x = cfg.pos_x;
        pos_y = cfg.pos_y;
        if (pos_x < 0) pos_x = 0;
        if (pos_y < 0) pos_y = 0;
        if (pos_x > cfg.width)  pos_x = cfg.width;
        if (pos_y > cfg.height) pos_y = cfg.height;
    }
    else if (cfg.ass_alignment == 9) { pos_x = cfg.width - 30;  pos_y = 30; }
    else if (cfg.ass_alignment == 1) { pos_x = 30;              pos_y = cfg.height - 30; }
    else if (cfg.ass_alignment == 3) { pos_x = cfg.width - 30;  pos_y = cfg.height - 30; }
    else if (cfg.ass_alignment == 8) { pos_x = cfg.width / 2;   pos_y = 30; }             // Top Center
    else if (cfg.ass_alignment == 2) { pos_x = cfg.width / 2;   pos_y = cfg.height - 30; } // Bottom Center

    // Per-frame clamping for custom positions: the text block must stay fully
    // on screen (bottom-right included). Block extents vary as counters grow
    // (e.g. "999 -> 1,000" gains a digit), so measure each frame's rendered
    // rows and clamp the anchor accordingly. Corner/center alignments are
    // positioned by libass itself and need no clamping.
    auto clamp_pos = [&](const std::vector<std::string>& lines) {
        if (cfg.pos_mode != 1) return;
        TextExtent ext = measure_block(lines, cfg.font_size);
        int bh = block_height(ext.lines, cfg.font_size);
        // Keep at least the estimated em-box on screen; never negative.
        int max_x = std::max(0, cfg.width  - std::min(ext.max_line_w, cfg.width));
        int max_y = std::max(0, cfg.height - std::min(bh,          cfg.height));
        if (pos_x > max_x) pos_x = max_x;
        if (pos_y > max_y) pos_y = max_y;
    };

    // Text per video frame comes from the shared FrameTextSource (the same
    // source the preview and the in-built renderer use); only the ASS
    // timing and \pos placement are specific to this writer.
    FrameTextSource src;
    src.frames = &frames;
    src.template_lines = template_lines;
    src.total_notes = total_notes;
    src.total_cc = total_cc;
    src.total_ticks = total_ticks;
    src.ppqn = ppqn;
    src.fps = cfg.fps;
    src.start_delay = cfg.start_delay;
    src.end_delay = cfg.end_delay;
    src.commas = cfg.commas;
    src.pad = cfg.pad;
    src.bpm = cfg.bpm;
    auto emit_frame = [&](size_t k, double t_start, double t_end) {
        const std::vector<std::string> rendered = src.lines(k);
        std::string text_block;
        for (size_t i = 0; i < rendered.size(); ++i) {
            text_block += rendered[i];
            if (i + 1 < rendered.size()) text_block += "\\N";
        }
        clamp_pos(rendered);
        ass << "Dialogue: 0," << to_ass_time(t_start) << "," << to_ass_time(t_end)
            << ",Default,,0,0,0,,{\\pos(" << pos_x << "," << pos_y << ")}" << text_block << "\n";
    };

    // Start-delay lead-in frames, then the song frames (the engine already
    // extends `frames` with the end-delay tail), offset by the start delay.
    const double frame_dur = 1.0 / cfg.fps;
    const size_t n_delay = src.lead_in_frames();
    for (size_t k = 0; k < n_delay; ++k) {
        double t_start = (double)k * frame_dur;
        double t_end = (double)(k + 1) * frame_dur;
        if (t_end > cfg.start_delay) t_end = cfg.start_delay;
        emit_frame(k, t_start, t_end);
    }
    for (size_t fi = 0; fi < frames.size(); ++fi) {
        double t_start = frames[fi].timestamp_sec + cfg.start_delay;
        double t_end = (fi + 1 < frames.size()) ? frames[fi + 1].timestamp_sec + cfg.start_delay
                                                 : t_start + frame_dur;
        emit_frame(n_delay + fi, t_start, t_end);
    }
    ass.close();
}


// ---- FrameTextSource ---------------------------------------------------------

size_t FrameTextSource::lead_in_frames() const {
    if (!(start_delay > 0.0) || !frames || frames->empty()) return 0;
    return (size_t)static_cast<long long>(std::ceil(start_delay * fps));
}

size_t FrameTextSource::frame_count() const {
    return lead_in_frames() + (frames ? frames->size() : 0);
}

double FrameTextSource::song_duration() const {
    // {time-max} / {sec-max} / {tick-rem} describe the MIDI SONG horizon, not
    // the end-delay-extended video: subtract the tail the engine appended.
    // {time} itself keeps counting past it and ends at the extended time.
    const double total = (!frames || frames->empty()) ? 0.0 : frames->back().timestamp_sec;
    const double d = total - end_delay;
    return d < 0.0 ? 0.0 : d;
}

FrameStats FrameTextSource::stats(size_t k) const {
    const size_t n = lead_in_frames();
    if (k >= n) {
        if (!frames || frames->empty()) return FrameStats{};
        const size_t i = k - n;
        return (*frames)[i < frames->size() ? i : frames->size() - 1];
    }
    // Lead-in: all counters zero, negative current-time fields counting up
    // to 0 at the song's first frame.
    FrameStats z;
    z.bpm = frames->front().bpm;                  // show the starting BPM, not 0
    z.timestamp_sec = (double)k * (1.0 / fps) - start_delay;
    // Negative song ticks, counting up to 0: linear back-extrapolation at the
    // initial tempo from the song's first frame (safe even when the first
    // tempo change sits after tick 0).
    const double us0 = 60000000.0 / (frames->front().bpm > 0.0 ? frames->front().bpm : 120.0);
    z.tick = frames->front().tick + (int64_t)std::llround(z.timestamp_sec * (double)ppqn * 1e6 / us0);
    z.frame_index = k;
    return z;
}

std::vector<std::string> FrameTextSource::lines(size_t k) const {
    const bool lead_in = k < lead_in_frames();
    const FrameStats fs = stats(k);
    const double max_time = song_duration();
    std::vector<std::string> out;
    out.reserve(template_lines.size());
    for (const std::string& t : template_lines)
        out.push_back(lead_in
            ? ProcessTemplateLine(t, fs, total_notes, 0, max_time, ppqn, commas, pad, bpm, 0)
            : ProcessTemplateLine(t, fs, total_notes, total_cc, max_time, ppqn, commas, pad, bpm, total_ticks));
    return out;
}
