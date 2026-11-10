// settings_store.cpp -- see settings_store.h.
//
// Format is deliberately boring flat INI ("key = value", '#'-comments) so
// files stay hand-editable and survive program updates untouched. The
// settings directory itself is NEVER versioned by the installer: updates
// just re-read whatever is there, which is what "persist across updates"
// requires.
#include "settings_store.h"

#include "../src/fmcg_path.h"     // extract_dir / extract_stem helpers
#include "../src/fmcg_video.h"    // k_presets / k_tunes

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <strings.h>   // strcasecmp
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Config directory discovery
// ---------------------------------------------------------------------------

std::string settings_dir() {
    static std::string cached;
    if (!cached.empty()) return cached;
    try {
        fs::path dir;
#if defined(_WIN32)
        wchar_t appdata[MAX_PATH] = L"";
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)) && appdata[0])
            dir = fs::path(appdata) / "fMCG";
        else
            dir = fs::path(".") / "fMCG_settings";   // last resort: portable
#else
        const char* xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && xdg[0]) dir = fs::path(xdg) / "fMCG";
        else {
            const char* home = getenv("HOME");
            if (!home || !home[0]) home = getpwuid(getuid())->pw_dir;
            dir = fs::path(home ? home : ".") / ".config" / "fMCG";
        }
#endif
        fs::create_directories(dir);
        fs::create_directories(dir / "patterns");
        cached = path_to_utf8(dir);   // UTF-8 like every fMCG path
    } catch (...) {
        cached = "";   // persistence unavailable; callers fall back to defaults
    }
    return cached;
}

// ---------------------------------------------------------------------------
// Tiny INI helper (flat key = value; values may be empty and contain '=')
// ---------------------------------------------------------------------------

namespace {

using KV = std::vector<std::pair<std::string, std::string>>;

// Strips a UTF-8 BOM and CRLF, splits "key = value".
KV parse_ini(const std::string& text) {
    KV out;
    std::string clean = text;
    if (clean.size() >= 3 && (unsigned char)clean[0] == 0xEF &&
        (unsigned char)clean[1] == 0xBB && (unsigned char)clean[2] == 0xBF)
        clean.erase(0, 3);
    std::istringstream ss(clean);
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        // '#'/';' comments, but never inside a value's leading position for layout lines:
        // layout text is stored with escaping (see escape/unescape below), so a
        // leading '#' on a raw line is always a comment.
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // trim spaces around key only; value keeps everything after "= "
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (!val.empty() && val[0] == ' ') val.erase(0, 1);
        out.push_back({key, val});
    }
    return out;
}

std::string get_str(const KV& kv, const char* key, const std::string& def = "") {
    for (const auto& [k, v] : kv)
        if (k == key) return v;
    return def;
}
int    get_int(const KV& kv, const char* key, int def) {
    std::string v = get_str(kv, key);
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}
double get_dbl(const KV& kv, const char* key, double def) {
    std::string v = get_str(kv, key);
    if (v.empty()) return def;
    try {
        const double d = std::stod(v);
        return std::isfinite(d) ? d : def;   // "nan"/"inf" would pass every range check
    } catch (...) { return def; }
}
bool   get_bool(const KV& kv, const char* key, bool def) {
    std::string v = get_str(kv, key);
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes";
}

// Multiline layout text is escaped as literal "\n" sequences so each Dialogue
// row survives the one-key-per-line format.
std::string escape_newlines(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') { /* drop */ }
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}
std::string unescape_newlines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i + 1] == 'n') { out += '\n'; ++i; continue; }
            if (s[i + 1] == '\\') { out += '\\'; ++i; continue; }
        }
        out += s[i];
    }
    return out;
}

// Write via temp file + rename so a crash mid-write never corrupts settings.
bool write_file_atomic(const fs::path& target, const std::string& content) {
    try {
        fs::path tmp = target;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) return false;
            f << content;
            if (!f.good()) return false;
        }
        fs::rename(tmp, target);   // POSIX-replace, works even if target exists
        return true;
    } catch (...) {
        return false;
    }
}

std::string patterns_dir() {
    std::string base = settings_dir();
    if (base.empty()) return "";
    return path_to_utf8(path_from_utf8(base) / "patterns");
}



} // namespace

const char* const k_default_layout =
    "Time: {time-milli}/{time-milli-max}\n"
    "Notes: {nc}/{nc-total}/{nc-rem}\n"
    "NPS: {nps}/{nps-max}\n"
    "Polyphony: {plph}/{plph-max}\n"
    "BPM: {bpm}";

// ---------------------------------------------------------------------------
// Global settings
// ---------------------------------------------------------------------------

bool load_global_settings(GlobalSettings& out) {
    std::string dir = settings_dir();
    if (dir.empty()) return false;
    std::ifstream f(path_from_utf8(dir) / "settings.ini");
    if (!f.is_open()) return false;
    std::ostringstream ss; ss << f.rdbuf();
    globals_from_ini(ss.str(), out);
    return true;
}

void globals_from_ini(const std::string& text, GlobalSettings& out) {
    KV kv = parse_ini(text);
    out.midi_dir     = get_str(kv, "midi_dir");
    out.output_dir   = get_str(kv, "output_dir");
    out.width        = get_int(kv, "width", 1920);
    out.height       = get_int(kv, "height", 1080);
    out.fps          = get_dbl(kv, "fps", 60.0);
    out.vel0_note_off= get_bool(kv, "vel0_note_off", true);
    out.close_held_notes = get_bool(kv, "close_held_notes", false);
    out.start_delay  = get_dbl(kv, "start_delay", 3.0);
    out.end_delay    = get_dbl(kv, "end_delay", 0.0);
    out.ffmpeg_threads = get_int(kv, "ffmpeg_threads", 0);
    out.parse_threads  = get_int(kv, "parse_threads", 0);
    // dedup_mode replaced the single "track_dedup" checkbox, which ran the
    // layered walk on compressed files (and track dedup on plain ones):
    // an old track_dedup = 1 becomes layered dedup.
    out.dedup_mode     = get_int(kv, "dedup_mode", get_bool(kv, "track_dedup", false) ? 2 : 0);
    if (out.dedup_mode < 0 || out.dedup_mode > 2) out.dedup_mode = 0;
    out.output_format  = get_int(kv, "output_format", 0);
    if (out.output_format < 0 || out.output_format > 1) out.output_format = 0;
    out.transparent_bg = get_bool(kv, "transparent_bg", false);
    out.x264_crf       = get_int(kv, "x264_crf", 18);
    if (out.x264_crf < 0 || out.x264_crf > 51) out.x264_crf = 18;
    out.x264_preset    = get_str(kv, "x264_preset");
    {
        bool known = false;
        for (const char* p : fmcg_video::k_presets) known |= out.x264_preset == p;
        if (!known) out.x264_preset = "veryfast";
        out.x264_tune = get_str(kv, "x264_tune");
        known = out.x264_tune.empty();
        for (const char* t : fmcg_video::k_tunes) known |= out.x264_tune == t;
        if (!known) out.x264_tune.clear();
    }
    out.keyframe_seconds = get_dbl(kv, "keyframe_seconds", 2.0);
    if (!(out.keyframe_seconds >= 0.1 && out.keyframe_seconds <= 60.0)) out.keyframe_seconds = 2.0;
    out.log_height = get_dbl(kv, "log_height", 150.0);
    if (!(out.log_height >= 60.0 && out.log_height <= 10000.0)) out.log_height = 150.0;
    out.log_open = get_bool(kv, "log_open", false);
    if (out.fps <= 0) out.fps = 60.0;
    if (out.fps > 1000.0) out.fps = 1000.0;           // k_max_fps (gui/app_state.h)
    if (out.width <= 0) out.width = 1920;
    if (out.height <= 0) out.height = 1080;
    if (out.start_delay < 0) out.start_delay = 0.0;
    if (out.end_delay   < 0) out.end_delay   = 0.0;
    out.start_delay = std::min(out.start_delay, 3600.0);   // k_max_delay (gui/app_state.h)
    out.end_delay   = std::min(out.end_delay, 3600.0);
    if (out.ffmpeg_threads < 0) out.ffmpeg_threads = 0;
    if (out.parse_threads  < 0) out.parse_threads  = 0;
}

bool save_global_settings(const GlobalSettings& s) {
    std::string dir = settings_dir();
    if (dir.empty()) return false;
    return write_file_atomic(path_from_utf8(dir) / "settings.ini", globals_to_ini(s));
}

std::string globals_to_ini(const GlobalSettings& s) {
    std::ostringstream ss;
    ss << "# fMCG global settings (auto-saved)\n";
    ss << "midi_dir = " << s.midi_dir << "\n";
    ss << "output_dir = " << s.output_dir << "\n";
    ss << "width = " << s.width << "\n";
    ss << "height = " << s.height << "\n";
    ss << "fps = " << s.fps << "\n";
    ss << "vel0_note_off = " << (s.vel0_note_off ? 1 : 0) << "\n";
    ss << "close_held_notes = " << (s.close_held_notes ? 1 : 0) << "\n";
    ss << "start_delay = " << s.start_delay << "\n";
    ss << "end_delay = " << s.end_delay << "\n";
    ss << "ffmpeg_threads = " << s.ffmpeg_threads << "\n";
    ss << "parse_threads = " << s.parse_threads << "\n";
    ss << "dedup_mode = " << s.dedup_mode << "\n";
    ss << "output_format = " << s.output_format << "\n";
    ss << "transparent_bg = " << (s.transparent_bg ? 1 : 0) << "\n";
    ss << "x264_crf = " << s.x264_crf << "\n";
    ss << "x264_preset = " << s.x264_preset << "\n";
    ss << "x264_tune = " << s.x264_tune << "\n";
    ss << "keyframe_seconds = " << s.keyframe_seconds << "\n";
    ss << "log_height = " << s.log_height << "\n";
    ss << "log_open = " << (s.log_open ? 1 : 0) << "\n";
    return ss.str();
}

// ---------------------------------------------------------------------------
// Patterns
// ---------------------------------------------------------------------------

std::string sanitize_pattern_name(const std::string& raw) {
    std::string name;
    for (char c : raw) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';                        // filesystem-hostile characters
        if ((unsigned char)c >= 32) name += c;   // drop control chars
    }
    // trim surrounding whitespace
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    // reserved device names on Windows
    static const char* reserved[] = {"CON","PRN","AUX","NUL","COM1","COM2","COM3","COM4",
                                     "LPT1","LPT2","LPT3"};
    for (const char* r : reserved)
#if defined(_WIN32)
        if (_stricmp(name.c_str(), r) == 0) return name + "_pattern";
#else
        if (strcasecmp(name.c_str(), r) == 0) return name + "_pattern";
#endif
    return name;
}

bool pattern_exists(const std::string& name) {
    std::string pdir = patterns_dir();
    if (pdir.empty() || name.empty()) return false;
    std::error_code ec;
    return fs::exists(path_from_utf8(pdir) / path_from_utf8(name + ".ini"), ec);
}

bool save_pattern(const std::string& name, const PatternData& p) {
    std::string pdir = patterns_dir();
    if (pdir.empty() || name.empty()) return false;
    return write_file_atomic(path_from_utf8(pdir) / path_from_utf8(name + ".ini"), pattern_to_ini(name, p));
}

std::string pattern_to_ini(const std::string& name, const PatternData& p) {
    std::ostringstream ss;
    ss << "# fMCG pattern: " << name << "\n";
    ss << "boxes = " << fmcg_boxes::structure_string(p.boxes) << "\n";
    for (int i = 0, n = fmcg_boxes::leaf_count(p.boxes); i < n; ++i) {
        const fmcg_boxes::BoxContent& b = fmcg_boxes::leaf_at(p.boxes, i)->box;
        const std::string k = "box" + std::to_string(i + 1) + "_";
        ss << k << "layout = " << escape_newlines(b.text) << "\n";
        ss << k << "anchor = " << b.anchor << "\n";
        ss << k << "line_align = " << b.line_align << "\n";
    }
    ss << "margin = " << p.margin << "\n";
    ss << "cells = " << (p.cells ? 1 : 0) << "\n";
    ss << "cell_align = " << p.cell_align << "\n";
    ss << "font_family = " << p.font_family << "\n";
    ss << "font_variant = " << p.font_variant << "\n";
    ss << "font_size = " << p.font_size << "\n";
    ss << "text_color = " << p.text_color_aabbggrr << "\n";
    ss << "bg_color = " << p.bg_color_aabbggrr << "\n";
    ss << "outline_enabled = " << (p.outline_enabled ? 1 : 0) << "\n";
    ss << "outline_width = " << p.outline_width << "\n";
    ss << "outline_color = " << p.outline_color_aabbggrr << "\n";
    ss << "typo_kerning = " << (p.typo.kerning ? 1 : 0) << "\n";
    ss << "typo_ligatures = " << (p.typo.ligatures ? 1 : 0) << "\n";
    ss << "typo_slashed_zero = " << (p.typo.slashed_zero ? 1 : 0) << "\n";
    ss << "typo_numerals = " << p.typo.numerals << "\n";
    ss << "typo_numeral_width = " << p.typo.numeral_width << "\n";
    {
        std::string sets;   // "ss01 ss03" -- readable in the INI
        for (int n = 1; n <= 20; ++n)
            if (p.typo.stylistic_sets & (1u << (n - 1)))
                sets += (sets.empty() ? "ss" : " ss") + std::string(n < 10 ? "0" : "") + std::to_string(n);
        ss << "typo_stylistic_sets = " << sets << "\n";
    }
    ss << "typo_features = " << p.typo.extra << "\n";
    ss << "letter_spacing = " << p.typo.letter_spacing << "\n";
    ss << "line_spacing = " << p.typo.line_spacing << "\n";
    ss << "comma_notes = " << (p.commas.notes ? 1 : 0) << "\n";
    ss << "comma_poly = " << (p.commas.polyphony ? 1 : 0) << "\n";
    ss << "comma_nps = " << (p.commas.nps ? 1 : 0) << "\n";
    ss << "comma_cc = " << (p.commas.cc ? 1 : 0) << "\n";
    ss << "comma_ticks = " << (p.commas.ticks ? 1 : 0) << "\n";
    ss << "comma_beats = " << (p.commas.beats ? 1 : 0) << "\n";
    ss << "pad_enabled = " << (p.pad.enabled ? 1 : 0) << "\n";
    int bpm_dec = p.bpm.decimals;
    if (bpm_dec < 0) bpm_dec = 0;
    if (bpm_dec > 6) bpm_dec = 6;
    ss << "bpm_decimals = " << bpm_dec << "\n";
    return ss.str();
}

bool load_pattern(const std::string& name, PatternData& out, int frame_w, int frame_h) {
    std::string pdir = patterns_dir();
    if (pdir.empty() || name.empty()) return false;
    std::ifstream f(path_from_utf8(pdir) / path_from_utf8(name + ".ini"));
    if (!f.is_open()) return false;
    std::ostringstream ss; ss << f.rdbuf();
    pattern_from_ini(ss.str(), out, frame_w, frame_h);
    return true;
}

// A pattern from before boxes: one block, placed by a corner preset
// (alignment + 30 px margin) or by a custom x,y (frame anchor + offset, block
// anchor on that point). Converted into the box that puts the block in the
// same place: the whole frame for a preset; for a custom position the box
// whose matching edge (or centre line) is the point, margin 0.
static void boxes_from_old_position(const KV& kv, bool ass_era, PatternData& out, int W, int H) {
    auto has = [&kv](const char* key) {
        for (const auto& e : kv) if (e.first == key) return true;
        return false;
    };
    fmcg_boxes::BoxContent c;
    c.text = unescape_newlines(get_str(kv, "layout", k_default_layout));
    int alignment = get_int(kv, "alignment", 0);
    if (alignment < 0 || alignment > 5) alignment = 0;
    static const int k_corner_numpad[6] = {7, 9, 1, 3, 8, 2};   // TL, TR, BL, BR, top centre, bottom centre
    const int pos_mode = get_int(kv, "pos_mode", 0);
    if (pos_mode != 1) {
        c.anchor = k_corner_numpad[alignment];
        out.boxes = fmcg_boxes::Node();
        out.boxes.box = c;
        out.margin = 30;
        return;
    }
    // 3x3 anchor indices (0 = top-left .. 8 = bottom-right).
    int fa = get_int(kv, "frame_anchor", 0), ba = get_int(kv, "block_anchor", 0);
    if (fa < 0 || fa > 8) fa = 0;
    if (ba < 0 || ba > 8) ba = 0;
    if (ass_era && !has("block_anchor")) {
        // ASS era: custom x,y anchored the block by its corner alignment,
        // measured from the frame's top-left.
        static const int k_corner_anchor[6] = {0, 2, 6, 8, 1, 7};
        ba = k_corner_anchor[alignment];
        fa = 0;
    }
    W = std::max(16, W);
    H = std::max(16, H);
    const double px = get_int(kv, "pos_x", 30), py = get_int(kv, "pos_y", 30);
    const int fh = fa % 3, fv = fa / 3, bh = ba % 3, bv = ba / 3;
    double X = fh == 2 ? W - px : fh == 1 ? W / 2.0 + px : px;
    double Y = fv == 2 ? H - py : fv == 1 ? H / 2.0 + py : py;
    X = std::max(0.0, std::min((double)W, X));
    Y = std::max(0.0, std::min((double)H, Y));
    // One axis: the box edge(s) through the point, and the anchor side.
    auto span = [](double P, double L, int side, double& a, double& b, int& out_side) {
        out_side = side;
        if (side == 0) { a = P; b = L; }
        else if (side == 2) { a = 0; b = P; }
        else {
            const double d = std::min(P, L - P);
            if (d >= 1.0) { a = P - d; b = P + d; }
            else { a = 0; b = L; out_side = P <= L / 2 ? 0 : 2; }   // centred on an edge: clamps there
        }
        if (b - a < 1.0) { a = 0; b = L; out_side = side == 0 ? 2 : 0; }   // at the far edge
    };
    double x0, x1, y0, y1;
    int sh, sv;
    span(X, W, bh, x0, x1, sh);
    span(Y, H, bv, y0, y1, sv);
    c.anchor = (sv == 0 ? 7 : sv == 1 ? 4 : 1) + sh;
    if (sh != bh) c.line_align = bh + 1;   // the anchor moved to an edge: lines keep the old block anchor's side
    out.boxes = fmcg_boxes::from_rect({x0 / W, y0 / H, x1 / W, y1 / H}, c);
    out.margin = 0;
}

void pattern_from_ini(const std::string& text, PatternData& out, int frame_w, int frame_h) {
    KV kv = parse_ini(text);
    auto has = [&kv](const char* key) {
        for (const auto& e : kv) if (e.first == key) return true;
        return false;
    };
    // Patterns saved before the in-built renderer (no outline keys) were drawn
    // by libass, with a 1 px black outline and custom x,y anchoring the block
    // by its corner alignment (\pos under the style's alignment).
    const bool ass_era = !has("outline_enabled");
    if (has("boxes") && fmcg_boxes::parse_structure(get_str(kv, "boxes"), out.boxes)) {
        for (int i = 0, n = fmcg_boxes::leaf_count(out.boxes); i < n; ++i) {
            fmcg_boxes::BoxContent& b = fmcg_boxes::leaf_at(out.boxes, i)->box;
            const std::string k = "box" + std::to_string(i + 1) + "_";
            b.text = unescape_newlines(get_str(kv, (k + "layout").c_str()));
            b.anchor = get_int(kv, (k + "anchor").c_str(), 7);
            if (b.anchor < 1 || b.anchor > 9) b.anchor = 7;
            b.line_align = get_int(kv, (k + "line_align").c_str(), 0);
            if (b.line_align < 0 || b.line_align > 3) b.line_align = 0;
        }
        out.margin = get_int(kv, "margin", 30);
    } else {
        boxes_from_old_position(kv, ass_era, out, frame_w, frame_h);
    }
    if (out.margin < 0 || out.margin > 1000) out.margin = 30;
    out.cells                = get_bool(kv, "cells", false);
    out.cell_align           = get_int(kv, "cell_align", 2);
    if (out.cell_align < 0 || out.cell_align > 2) out.cell_align = 2;
    out.font_family          = get_str(kv, "font_family", "Arial");
    out.font_variant         = get_str(kv, "font_variant");
    out.font_size            = get_int(kv, "font_size", 36);
    out.text_color_aabbggrr  = get_str(kv, "text_color", "&H00FFFFFF");
    out.bg_color_aabbggrr    = get_str(kv, "bg_color", "&H00000000");
    out.outline_enabled      = get_bool(kv, "outline_enabled", true);   // ASS era: libass's 1 px outline
    out.outline_width        = get_dbl(kv, "outline_width", 1.0);
    if (out.outline_width < 0.0) out.outline_width = 0.0;
    if (out.outline_width > 20.0) out.outline_width = 20.0;
    out.outline_color_aabbggrr = get_str(kv, "outline_color", "&H00000000");
    out.typo = fmcg_text::Typography();
    out.typo.kerning       = get_bool(kv, "typo_kerning", true);
    out.typo.ligatures     = get_bool(kv, "typo_ligatures", true);
    out.typo.slashed_zero  = get_bool(kv, "typo_slashed_zero", false);
    out.typo.numerals      = get_int(kv, "typo_numerals", 0);
    if (out.typo.numerals < 0 || out.typo.numerals > 2) out.typo.numerals = 0;
    out.typo.numeral_width = get_int(kv, "typo_numeral_width", 0);
    if (out.typo.numeral_width < 0 || out.typo.numeral_width > 2) out.typo.numeral_width = 0;
    for (const std::string& t : fmcg_text::split_features(get_str(kv, "typo_stylistic_sets"))) {
        const int n = (t.size() == 4 && t[0] == 's' && t[1] == 's') ? std::atoi(t.c_str() + 2) : 0;
        if (n >= 1 && n <= 20) out.typo.stylistic_sets |= 1u << (n - 1);
    }
    out.typo.extra         = get_str(kv, "typo_features");
    out.typo.letter_spacing = get_dbl(kv, "letter_spacing", 0.0);
    if (!(out.typo.letter_spacing >= -50.0 && out.typo.letter_spacing <= 200.0)) out.typo.letter_spacing = 0.0;
    out.typo.line_spacing  = get_dbl(kv, "line_spacing", 1.0);
    if (!(out.typo.line_spacing >= 0.5 && out.typo.line_spacing <= 4.0)) out.typo.line_spacing = 1.0;
    out.commas.notes         = get_bool(kv, "comma_notes", true);
    out.commas.polyphony     = get_bool(kv, "comma_poly", true);
    out.commas.nps           = get_bool(kv, "comma_nps", true);
    out.commas.cc            = get_bool(kv, "comma_cc", false);
    out.commas.ticks         = get_bool(kv, "comma_ticks", true);
    out.commas.beats         = get_bool(kv, "comma_beats", true);
    out.pad.enabled          = get_bool(kv, "pad_enabled", false);
    out.bpm.decimals         = get_int(kv, "bpm_decimals", 2);
    if (out.bpm.decimals < 0) out.bpm.decimals = 0;
    if (out.bpm.decimals > 6) out.bpm.decimals = 6;
    if (out.font_size <= 0) out.font_size = 36;
}

bool delete_pattern(const std::string& name) {
    std::string pdir = patterns_dir();
    if (pdir.empty() || name.empty()) return false;
    std::error_code ec;
    return fs::remove(path_from_utf8(pdir) / path_from_utf8(name + ".ini"), ec) && !ec;
}

std::vector<std::string> list_patterns() {
    std::vector<std::string> out;
    std::string pdir = patterns_dir();
    if (pdir.empty()) return out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(path_from_utf8(pdir), ec)) {
        if (!e.is_regular_file()) continue;
        std::string fn = path_to_utf8(e.path().filename());
        if (fn.size() > 4 && fn.compare(fn.size() - 4, 4, ".ini") == 0)
            out.push_back(fn.substr(0, fn.size() - 4));
    }
    // Default first, rest alphabetically -- feels natural in the dropdown.
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        bool da = (a == "Default"), db = (b == "Default");
        if (da != db) return da;
        return a < b;
    });
    return out;
}

void ensure_default_pattern() {
    if (pattern_exists("Default")) return;
    PatternData p;   // struct defaults == the shipped default look
    p.boxes.box.text = k_default_layout;
    save_pattern("Default", p);
}

std::vector<std::string> layout_lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out.empty() && text != k_default_layout ? layout_lines(k_default_layout) : out;
}

void pattern_template(const PatternData& p, std::vector<std::string>& lines, std::vector<int>& boxes) {
    lines.clear();
    boxes.clear();
    for (int i = 0, n = fmcg_boxes::leaf_count(p.boxes); i < n; ++i) {
        std::istringstream in(fmcg_boxes::leaf_at(p.boxes, i)->box.text);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            lines.push_back(line);
            boxes.push_back(i);
        }
    }
}
