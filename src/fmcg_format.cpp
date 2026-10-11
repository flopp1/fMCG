#include "fmcg_format.h"
#include "fmcg_util.h"

#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

// ---- template tokens -------------------------------------------------------------

namespace {

const char* const k_tokens[] = {
    "nc", "nc-total", "nc-rem", "cc", "cc-total", "cc-rem", "tick", "tick-total", "tick-rem",
    "sec", "sec-max", "sec-rem", "time", "time-max", "time-rem", "time-milli", "time-milli-max",
    "time-milli-rem", "bpm", "ppqn", "plph", "plph-max", "nps", "nps-max",
};

bool known_token(const std::string& name) {
    for (const char* t : k_tokens)
        if (name == t) return true;
    return false;
}

// Formats one stat token for one frame (everything ProcessTemplateLine takes).
struct StatFormatter {
    const FrameStats& fs;
    uint64_t total_notes, total_cc;
    double max_time;
    uint16_t ppqn;
    const CommaOpts& commas;
    int bdec;
    uint64_t total_ticks;
    int w_notes = 0, w_cc = 0, w_poly = 0, w_nps = 0, w_sec = 0, w_tick = 0;

    StatFormatter(const FrameStats& f, uint64_t tn, uint64_t tc, double mt, uint16_t pq,
                  const CommaOpts& c, PadOpts pad, BpmOpts bpm, uint64_t tt)
        : fs(f), total_notes(tn), total_cc(tc), max_time(mt), ppqn(pq), commas(c), total_ticks(tt) {
        bdec = std::max(0, std::min(6, bpm.decimals));   // cap: beyond 6 decimals is display noise
        if (pad.enabled) {   // each stat pads toward its own running maximum
            auto w = [](uint64_t v) { int n = 1; while (v >= 10) { v /= 10; ++n; } return n; };
            w_notes = w(total_notes);
            w_cc    = w(total_cc);
            w_poly  = w(static_cast<uint64_t>(std::max<int64_t>(0, fs.peak_polyphony)));
            w_nps   = w(static_cast<uint64_t>(std::max<double>(0, std::round(fs.peak_nps))));
            w_sec   = w(static_cast<uint64_t>(std::max<double>(0, max_time)));
            w_tick  = w(total_ticks);
        }
    }

    // Pad the raw digits first, then group with commas, so the pad always
    // reflects digit count.
    static std::string fmt(bool commas_on, uint64_t v, int width) {
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
    }
    // Ticks: signed so the start-delay countdown can run negative and count
    // up (mirrors the {sec}/{time} fields).
    std::string tick_signed(int64_t v) const {
        if (v >= 0) return fmt(commas.ticks, (uint64_t)v, w_tick);
        return "-" + fmt(commas.ticks, (uint64_t)(-v), w_tick);
    }
    // Signed seconds, truncating toward zero (start-delay countdown runs
    // negative and counts up to 0).
    std::string sec(double v) const {
        long long iv = static_cast<long long>(v);   // truncates toward zero
        bool neg = iv < 0;
        std::string s = std::to_string(neg ? (uint64_t)(-iv) : (uint64_t)iv);
        if (w_sec > 0 && (int)s.size() < w_sec)
            s.insert(0, (size_t)w_sec - s.size(), '0');
        return neg ? "-" + s : s;
    }

    std::string token(const std::string& n) const {
        const double rem_t = max_time > fs.timestamp_sec ? max_time - fs.timestamp_sec : 0;
        if (n == "nc") return fmt(commas.notes, fs.cumulative_notes, w_notes);
        if (n == "nc-total") return fmt(commas.notes, total_notes, w_notes);
        if (n == "nc-rem") return fmt(commas.notes, total_notes > fs.cumulative_notes ? total_notes - fs.cumulative_notes : 0, w_notes);
        if (n == "cc") return fmt(commas.cc, fs.cumulative_cc, w_cc);
        if (n == "cc-total") return fmt(commas.cc, total_cc, w_cc);
        if (n == "cc-rem") return fmt(commas.cc, total_cc > fs.cumulative_cc ? total_cc - fs.cumulative_cc : 0, w_cc);
        if (n == "tick") return tick_signed(fs.tick);
        if (n == "tick-total") return fmt(commas.ticks, total_ticks, w_tick);
        if (n == "tick-rem") return fmt(commas.ticks, (uint64_t)std::max<int64_t>(0, (int64_t)total_ticks - fs.tick), w_tick);
        if (n == "sec") return sec(fs.timestamp_sec);
        if (n == "sec-max") return sec(max_time);
        if (n == "sec-rem") return sec(rem_t);
        if (n == "time") return format_time(fs.timestamp_sec, false);
        if (n == "time-max") return format_time(max_time, false);
        if (n == "time-rem") return format_time(rem_t, false);
        if (n == "time-milli") return format_time(fs.timestamp_sec, true);
        if (n == "time-milli-max") return format_time(max_time, true);
        if (n == "time-milli-rem") return format_time(rem_t, true);
        if (n == "bpm") {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(bdec) << fs.bpm;
            return ss.str();
        }
        if (n == "ppqn") return std::to_string(ppqn);
        if (n == "plph") return fmt(commas.polyphony, static_cast<uint64_t>(std::max<int64_t>(0, fs.polyphony)), w_poly);
        if (n == "plph-max") return fmt(commas.polyphony, static_cast<uint64_t>(std::max<int64_t>(0, fs.peak_polyphony)), w_poly);
        if (n == "nps") return fmt(commas.nps, static_cast<uint64_t>(std::round(std::max<double>(0, fs.notes_per_second))), w_nps);
        if (n == "nps-max") return fmt(commas.nps, static_cast<uint64_t>(std::round(std::max<double>(0, fs.peak_nps))), w_nps);
        return "{" + n + "}";
    }
};

} // namespace

std::vector<TemplateSegment> parse_template_line(const std::string& line) {
    std::vector<TemplateSegment> out;
    std::string lit;
    size_t i = 0;
    while (i < line.size()) {
        if (line[i] == '{') {
            const size_t close = line.find('}', i + 1);
            if (close != std::string::npos) {
                const std::string name = line.substr(i + 1, close - i - 1);
                if (known_token(name)) {
                    if (!lit.empty()) { out.push_back({lit, false}); lit.clear(); }
                    out.push_back({name, true});
                    i = close + 1;
                    continue;
                }
            }
        }
        lit += line[i++];
    }
    if (!lit.empty()) out.push_back({lit, false});
    return out;
}

std::vector<fmcg_text::TextPiece> format_template_line(const std::string& line, const FrameStats& fs,
                                                       uint64_t total_notes, uint64_t total_cc_events,
                                                       double max_time_sec, uint16_t ppqn,
                                                       const CommaOpts& commas, PadOpts pad,
                                                       BpmOpts bpm, uint64_t total_ticks) {
    return format_template_segments(parse_template_line(line), fs, total_notes, total_cc_events, max_time_sec,
                                    ppqn, commas, pad, bpm, total_ticks);
}

std::vector<fmcg_text::TextPiece> format_template_segments(const std::vector<TemplateSegment>& segs,
                                                           const FrameStats& fs, uint64_t total_notes,
                                                           uint64_t total_cc_events, double max_time_sec,
                                                           uint16_t ppqn, const CommaOpts& commas, PadOpts pad,
                                                           BpmOpts bpm, uint64_t total_ticks) {
    const StatFormatter f(fs, total_notes, total_cc_events, max_time_sec, ppqn, commas, pad, bpm, total_ticks);
    std::vector<fmcg_text::TextPiece> out;
    out.reserve(segs.size());
    int cell = 0;
    for (const TemplateSegment& s : segs)
        out.push_back(s.token ? fmcg_text::TextPiece{f.token(s.text), cell++} : fmcg_text::TextPiece{s.text, -1});
    return out;
}

std::string ProcessTemplateLine(const std::string& line, const FrameStats& fs,
                                uint64_t total_notes, uint64_t total_cc_events,
                                double max_time_sec, uint16_t ppqn,
                                const CommaOpts& commas, PadOpts pad, BpmOpts bpm,
                                uint64_t total_ticks) {
    std::string result;
    for (const fmcg_text::TextPiece& p : format_template_line(line, fs, total_notes, total_cc_events,
                                                              max_time_sec, ppqn, commas, pad, bpm, total_ticks))
        result += p.text;
    return result;
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

const std::vector<std::vector<TemplateSegment>>& FrameTextSource::parsed() const {
    if (!parsed_ok_ || parsed_from_ != template_lines) {
        parsed_.clear();
        for (const std::string& t : template_lines) parsed_.push_back(parse_template_line(t));
        parsed_from_ = template_lines;
        parsed_ok_ = true;
    }
    return parsed_;
}

fmcg_text::PieceLines FrameTextSource::pieces_for(const FrameStats& fs, bool lead_in) const {
    const double max_time = song_duration();
    fmcg_text::PieceLines out;
    out.reserve(template_lines.size());
    for (const std::vector<TemplateSegment>& t : parsed())
        out.push_back(lead_in
            ? format_template_segments(t, fs, total_notes, 0, max_time, ppqn, commas, pad, bpm, 0)
            : format_template_segments(t, fs, total_notes, total_cc, max_time, ppqn, commas, pad, bpm, total_ticks));
    return out;
}

fmcg_text::PieceLines FrameTextSource::pieces(size_t k) const {
    return pieces_for(stats(k), k < lead_in_frames());
}

std::vector<std::string> FrameTextSource::lines(size_t k) const {
    std::vector<std::string> out;
    for (const auto& line : pieces(k)) {
        std::string s;
        for (const auto& p : line) s += p.text;
        out.push_back(std::move(s));
    }
    return out;
}

fmcg_text::TextBlockSpec FrameTextSource::block_spec() const {
    // Probe frames whose values bound every stat over the whole video:
    //  - the first video frame (most negative time/ticks; largest *-rem during
    //    the lead-in),
    //  - the first song frame (largest *-rem),
    //  - the last frame (largest time/ticks/totals so far),
    //  - a synthetic frame with every running stat at its maximum (notes, CC,
    //    polyphony, NPS, BPM).
    // Each probe's text, with every digit replaced by each of 0..9, gives the
    // candidates; the renderer measures them in the actual font, so the
    // widest digit of that font (proportional or tabular) is covered.
    std::vector<std::pair<FrameStats, bool>> probes;
    if (frames && !frames->empty()) {
        const size_t n = frame_count();
        probes.push_back({stats(0), lead_in_frames() > 0});
        probes.push_back({stats(lead_in_frames()), false});
        probes.push_back({stats(n - 1), false});
        FrameStats mx = frames->back();
        mx.cumulative_notes = total_notes;
        mx.cumulative_cc = total_cc;
        mx.polyphony = std::max<int64_t>(mx.polyphony, mx.peak_polyphony);
        mx.notes_per_second = std::max(mx.notes_per_second, mx.peak_nps);
        for (const FrameStats& f : *frames) {
            mx.bpm = std::max(mx.bpm, f.bpm);
            mx.polyphony = std::max<int64_t>(mx.polyphony, f.peak_polyphony);
            mx.peak_polyphony = std::max<int64_t>(mx.peak_polyphony, f.peak_polyphony);
            mx.notes_per_second = std::max(mx.notes_per_second, f.peak_nps);
            mx.peak_nps = std::max(mx.peak_nps, f.peak_nps);
        }
        probes.push_back({mx, false});
    } else {   // pattern-only preview: zero stats, time from -start_delay to 0
        FrameStats z;
        z.timestamp_sec = -start_delay;
        probes.push_back({z, false});
        z.timestamp_sec = 0.0;
        probes.push_back({z, false});
    }
    fmcg_text::TextBlockSpec spec;
    spec.pieces = pieces_for(probes.front().first, probes.front().second);
    spec.extremes.resize(spec.pieces.size());
    for (const auto& pr : probes) {
        const fmcg_text::PieceLines pl = pieces_for(pr.first, pr.second);
        for (size_t li = 0; li < pl.size(); ++li)
            for (const auto& p : pl[li]) {
                if (p.cell < 0) continue;
                auto& c = spec.extremes[li];
                if ((size_t)p.cell >= c.size()) c.resize(p.cell + 1);
                for (char d = '0'; d <= '9'; ++d) {
                    std::string s = p.text;
                    for (char& ch : s)
                        if (ch >= '0' && ch <= '9') ch = d;
                    if (std::find(c[p.cell].begin(), c[p.cell].end(), s) == c[p.cell].end()) c[p.cell].push_back(s);
                }
            }
    }
    return spec;
}
