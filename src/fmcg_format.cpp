#include "fmcg_format.h"
#include "fmcg_util.h"

#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

// ---- template tokens -------------------------------------------------------------

namespace {

struct TokenName { const char* name; Stat stat; };
const TokenName k_tokens[] = {
    {"nc", Stat::Notes}, {"nc-total", Stat::NotesTotal}, {"nc-rem", Stat::NotesRem},
    {"cc", Stat::Cc}, {"cc-total", Stat::CcTotal}, {"cc-rem", Stat::CcRem},
    {"tick", Stat::Tick}, {"tick-total", Stat::TickTotal}, {"tick-rem", Stat::TickRem},
    {"sec", Stat::Sec}, {"sec-max", Stat::SecMax}, {"sec-rem", Stat::SecRem},
    {"time", Stat::Time}, {"time-max", Stat::TimeMax}, {"time-rem", Stat::TimeRem},
    {"time-milli", Stat::TimeMilli}, {"time-milli-max", Stat::TimeMilliMax}, {"time-milli-rem", Stat::TimeMilliRem},
    {"bpm", Stat::Bpm}, {"ppqn", Stat::Ppqn}, {"plph", Stat::Poly}, {"plph-max", Stat::PolyMax},
    {"nps", Stat::Nps}, {"nps-max", Stat::NpsMax},
    {"beat", Stat::Beat}, {"beat-total", Stat::BeatTotal}, {"beat-rem", Stat::BeatRem},
    {"measure", Stat::Measure}, {"measure-total", Stat::MeasureTotal}, {"measure-rem", Stat::MeasureRem},
    {"measure-beat", Stat::MeasureBeat}, {"timesig", Stat::TimeSig},
};

Stat lookup_token(const std::string& name) {
    for (const TokenName& t : k_tokens)
        if (name == t.name) return t.stat;
    return Stat::Literal;
}

// Formats the stats of one frame (one instance per formatted line).
struct StatFormatter {
    const FrameStats& fs;
    const SongTotals& song;
    const CommaOpts& commas;
    int bdec;
    int w_notes = 0, w_cc = 0, w_poly = 0, w_nps = 0, w_sec = 0, w_tick = 0;
    int w_beat = 0, w_measure = 0, w_mbeat = 0;

    StatFormatter(const FrameStats& f, const SongTotals& s, const FormatOpts& o)
        : fs(f), song(s), commas(o.commas) {
        bdec = std::max(0, std::min(6, o.bpm.decimals));   // cap: beyond 6 decimals is display noise
        if (o.pad.enabled) {   // each stat pads toward its own running maximum
            auto w = [](uint64_t v) { int n = 1; while (v >= 10) { v /= 10; ++n; } return n; };
            w_notes   = w(song.notes);
            w_cc      = w(song.cc);
            w_poly    = w(static_cast<uint64_t>(std::max<int64_t>(0, fs.peak_polyphony)));
            w_nps     = w(static_cast<uint64_t>(std::max<double>(0, std::round(fs.peak_nps))));
            w_sec     = w(static_cast<uint64_t>(std::max<double>(0, song.duration)));
            w_tick    = w(song.ticks);
            w_beat    = w(song.meter.beats);
            w_measure = w(song.meter.measures);
            w_mbeat   = w(song.meter.max_num);
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
    static uint64_t rem(uint64_t total, uint64_t cur) { return total > cur ? total - cur : 0; }
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
    std::string bpm() const {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(bdec) << fs.bpm;
        return ss.str();
    }

    std::string format(Stat st) const {
        const double dur = song.duration;
        const double rem_t = dur > fs.timestamp_sec ? dur - fs.timestamp_sec : 0;
        const uint64_t poly = static_cast<uint64_t>(std::max<int64_t>(0, fs.polyphony));
        const uint64_t peak_poly = static_cast<uint64_t>(std::max<int64_t>(0, fs.peak_polyphony));
        switch (st) {
        case Stat::Literal:      return {};
        case Stat::Notes:        return fmt(commas.notes, fs.cumulative_notes, w_notes);
        case Stat::NotesTotal:   return fmt(commas.notes, song.notes, w_notes);
        case Stat::NotesRem:     return fmt(commas.notes, rem(song.notes, fs.cumulative_notes), w_notes);
        case Stat::Cc:           return fmt(commas.cc, fs.cumulative_cc, w_cc);
        case Stat::CcTotal:      return fmt(commas.cc, song.cc, w_cc);
        case Stat::CcRem:        return fmt(commas.cc, rem(song.cc, fs.cumulative_cc), w_cc);
        case Stat::Tick:         return tick_signed(fs.tick);
        case Stat::TickTotal:    return fmt(commas.ticks, song.ticks, w_tick);
        case Stat::TickRem:      return fmt(commas.ticks, (uint64_t)std::max<int64_t>(0, (int64_t)song.ticks - fs.tick), w_tick);
        case Stat::Sec:          return sec(fs.timestamp_sec);
        case Stat::SecMax:       return sec(dur);
        case Stat::SecRem:       return sec(rem_t);
        case Stat::Time:         return format_time(fs.timestamp_sec, false);
        case Stat::TimeMax:      return format_time(dur, false);
        case Stat::TimeRem:      return format_time(rem_t, false);
        case Stat::TimeMilli:    return format_time(fs.timestamp_sec, true);
        case Stat::TimeMilliMax: return format_time(dur, true);
        case Stat::TimeMilliRem: return format_time(rem_t, true);
        case Stat::Bpm:          return bpm();
        case Stat::Ppqn:         return std::to_string(song.ppqn);
        case Stat::Poly:         return fmt(commas.polyphony, poly, w_poly);
        case Stat::PolyMax:      return fmt(commas.polyphony, peak_poly, w_poly);
        case Stat::Nps:          return fmt(commas.nps, static_cast<uint64_t>(std::round(std::max<double>(0, fs.notes_per_second))), w_nps);
        case Stat::NpsMax:       return fmt(commas.nps, static_cast<uint64_t>(std::round(std::max<double>(0, fs.peak_nps))), w_nps);
        case Stat::Beat:         return fmt(commas.beats, fs.beat, w_beat);
        case Stat::BeatTotal:    return fmt(commas.beats, song.meter.beats, w_beat);
        case Stat::BeatRem:      return fmt(commas.beats, rem(song.meter.beats, fs.beat), w_beat);
        case Stat::Measure:      return fmt(commas.beats, fs.measure, w_measure);
        case Stat::MeasureTotal: return fmt(commas.beats, song.meter.measures, w_measure);
        case Stat::MeasureRem:   return fmt(commas.beats, rem(song.meter.measures, fs.measure), w_measure);
        case Stat::MeasureBeat:  return fmt(false, fs.measure_beat, w_mbeat);
        case Stat::TimeSig:      return std::to_string(fs.ts_num) + "/" + std::to_string((uint64_t)1 << std::min<int>(fs.ts_dd, 63));
        }
        return {};
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
                const Stat st = lookup_token(line.substr(i + 1, close - i - 1));
                if (st != Stat::Literal) {
                    if (!lit.empty()) { out.push_back({lit, Stat::Literal}); lit.clear(); }
                    out.push_back({std::string(), st});
                    i = close + 1;
                    continue;
                }
            }
        }
        lit += line[i++];
    }
    if (!lit.empty()) out.push_back({lit, Stat::Literal});
    return out;
}

std::vector<fmcg_text::TextPiece> format_template_segments(const std::vector<TemplateSegment>& segs,
                                                           const FrameStats& fs, const SongTotals& song,
                                                           const FormatOpts& fmt) {
    const StatFormatter f(fs, song, fmt);
    std::vector<fmcg_text::TextPiece> out;
    out.reserve(segs.size());
    int cell = 0;
    for (const TemplateSegment& s : segs)
        out.push_back(s.is_stat() ? fmcg_text::TextPiece{f.format(s.stat), cell++} : fmcg_text::TextPiece{s.text, -1});
    return out;
}

std::vector<fmcg_text::TextPiece> format_template_line(const std::string& line, const FrameStats& fs,
                                                       const SongTotals& song, const FormatOpts& fmt) {
    return format_template_segments(parse_template_line(line), fs, song, fmt);
}

std::string format_line(const std::string& line, const FrameStats& fs, const SongTotals& song,
                        const FormatOpts& fmt) {
    std::string result;
    for (const fmcg_text::TextPiece& p : format_template_line(line, fs, song, fmt)) result += p.text;
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
    z.ts_num = frames->front().ts_num;            // and the starting time signature
    z.ts_dd = frames->front().ts_dd;
    z.timestamp_sec = (double)k * (1.0 / fps) - start_delay;
    // Negative song ticks, counting up to 0: linear back-extrapolation at the
    // initial tempo from the song's first frame (safe even when the first
    // tempo change sits after tick 0).
    const double us0 = 60000000.0 / (frames->front().bpm > 0.0 ? frames->front().bpm : 120.0);
    z.tick = frames->front().tick + (int64_t)std::llround(z.timestamp_sec * (double)song.ppqn * 1e6 / us0);
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
    SongTotals s = song;
    s.duration = song_duration();
    if (lead_in) { s.cc = 0; s.ticks = 0; }   // the lead-in shows CC and tick totals as 0
    fmcg_text::PieceLines out;
    out.reserve(template_lines.size());
    for (const std::vector<TemplateSegment>& t : parsed())
        out.push_back(format_template_segments(t, fs, s, fmt));
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
    //    polyphony, NPS, BPM, beat / measure numbers), once per time signature
    //    the song uses ({timesig} and {measure-beat} change with it).
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
        mx.cumulative_notes = song.notes;
        mx.cumulative_cc = song.cc;
        mx.polyphony = std::max<int64_t>(mx.polyphony, mx.peak_polyphony);
        mx.notes_per_second = std::max(mx.notes_per_second, mx.peak_nps);
        for (const FrameStats& f : *frames) {
            mx.bpm = std::max(mx.bpm, f.bpm);
            mx.polyphony = std::max<int64_t>(mx.polyphony, f.peak_polyphony);
            mx.peak_polyphony = std::max<int64_t>(mx.peak_polyphony, f.peak_polyphony);
            mx.notes_per_second = std::max(mx.notes_per_second, f.peak_nps);
            mx.peak_nps = std::max(mx.peak_nps, f.peak_nps);
        }
        mx.beat = song.meter.beats;
        mx.measure = song.meter.measures;
        mx.measure_beat = song.meter.max_num;
        std::vector<std::pair<uint8_t, uint8_t>> sigs;   // distinct time signatures, file order
        for (const FrameStats& f : *frames)
            if (sigs.size() < 64 && (sigs.empty() || sigs.back() != std::make_pair(f.ts_num, f.ts_dd)) &&
                std::find(sigs.begin(), sigs.end(), std::make_pair(f.ts_num, f.ts_dd)) == sigs.end())
                sigs.push_back({f.ts_num, f.ts_dd});
        if (sigs.empty()) sigs.push_back({mx.ts_num, mx.ts_dd});
        for (const auto& sg : sigs) {
            mx.ts_num = sg.first;
            mx.ts_dd = sg.second;
            probes.push_back({mx, false});
        }
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
