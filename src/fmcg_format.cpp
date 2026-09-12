#include "fmcg_format.h"
#include "fmcg_util.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>

std::string ProcessTemplateLine(const std::string& line, const FrameStats& fs,
                                       uint64_t total_notes, uint64_t total_cc_events,
                                       double max_time_sec, uint16_t ppqn,
                                       const CommaOpts& commas, PadOpts pad) {
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

    replace("{nc}", fmt_notes(fs.cumulative_notes));
    replace("{nc-total}", fmt_notes(total_notes));
    replace("{nc-rem}", fmt_notes(total_notes > fs.cumulative_notes ? total_notes - fs.cumulative_notes : 0));

    replace("{cc}", fmt_cc(fs.cumulative_cc));
    replace("{cc-total}", fmt_cc(total_cc_events));
    replace("{cc-rem}", fmt_cc(total_cc_events > fs.cumulative_cc ? total_cc_events - fs.cumulative_cc : 0));

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
    bpm_ss << std::fixed << std::setprecision(2) << fs.bpm;

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
                          uint64_t total_cc, uint16_t ppqn, const AssConfig& cfg) {
    std::ofstream ass(ass_filename);
    ass << "[Script Info]\nScriptType: v4.00+\nPlayResX: " << cfg.width << "\nPlayResY: " << cfg.height << "\n\n";
    ass << "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n";
    ass << "Style: Default," << cfg.font_family << "," << cfg.font_size << "," << cfg.text_color_ass << "," << cfg.text_color_ass << ",&H00000000,&H80000000," << cfg.bold << "," << cfg.italic_flag << ",0,0,100,100,0,0,1,1,0," << cfg.ass_alignment << ",30,30,30,1\n\n";
    ass << "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

    int pos_x = 30, pos_y = 30;
    if (cfg.pos_mode == 1) {
        // Explicit placement: \pos anchors the text block's top-left corner.
        pos_x = cfg.pos_x;
        pos_y = cfg.pos_y;
    } else if (cfg.ass_alignment == 9)      { pos_x = cfg.width - 30;  pos_y = 30; }
    else if (cfg.ass_alignment == 1) { pos_x = 30;              pos_y = cfg.height - 30; }
    else if (cfg.ass_alignment == 3) { pos_x = cfg.width - 30;  pos_y = cfg.height - 30; }

    double total_duration = frames.empty() ? 0.0 : frames.back().timestamp_sec;

    // Start-delay lead-in: one Dialogue per frame with all-zero stats and
    // negative current-time fields counting up to 0 at the song's first frame.
    if (cfg.start_delay > 0.0 && !frames.empty()) {
        FrameStats zero_fs;                       // all counters zero
        zero_fs.bpm = frames.front().bpm;         // show the starting BPM, not 0
        const double frame_dur = 1.0 / cfg.fps;
        const long long n_delay = static_cast<long long>(std::ceil(cfg.start_delay * cfg.fps));
        for (long long k = 0; k < n_delay; ++k) {
            double t_start = k * frame_dur;
            double t_end = (k + 1) * frame_dur;
            if (t_end > cfg.start_delay) t_end = cfg.start_delay;
            zero_fs.timestamp_sec = t_start - cfg.start_delay;   // negative song time
            zero_fs.frame_index = (size_t)k;
            std::string text_block;
            for (size_t i = 0; i < template_lines.size(); ++i) {
                text_block += ProcessTemplateLine(template_lines[i], zero_fs, total_notes, 0, total_duration, ppqn, cfg.commas, cfg.pad);
                if (i + 1 < template_lines.size()) text_block += "\\N";
            }
            ass << "Dialogue: 0," << to_ass_time(t_start) << "," << to_ass_time(t_end)
                << ",Default,,0,0,0,,{\\pos(" << pos_x << "," << pos_y << ")}" << text_block << "\n";
        }
    }

    // Song frames, offset by the start delay.
    for (size_t fi = 0; fi < frames.size(); ++fi) {
        const auto& f = frames[fi];
        std::string text_block;
        for (size_t i = 0; i < template_lines.size(); ++i) {
            text_block += ProcessTemplateLine(template_lines[i], f, total_notes, total_cc, total_duration, ppqn, cfg.commas, cfg.pad);
            if (i + 1 < template_lines.size()) text_block += "\\N";
        }

        double t_start = f.timestamp_sec + cfg.start_delay;
        double t_end = (fi + 1 < frames.size()) ? frames[fi + 1].timestamp_sec + cfg.start_delay
                                                 : t_start + 1.0 / cfg.fps;
        ass << "Dialogue: 0," << to_ass_time(t_start) << "," << to_ass_time(t_end)
            << ",Default,,0,0,0,,{\\pos(" << pos_x << "," << pos_y << ")}" << text_block << "\n";
    }
    ass.close();
}
