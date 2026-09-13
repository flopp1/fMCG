// settings_store.cpp -- see settings_store.h.
//
// Format is deliberately boring flat INI ("key = value", '#'-comments) so
// files stay hand-editable and survive program updates untouched. The
// settings directory itself is NEVER versioned by the installer: updates
// just re-read whatever is there, which is what "persist across updates"
// requires.
#include "settings_store.h"

#include "../src/fmcg_path.h"     // extract_dir / extract_stem helpers

#include <algorithm>
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
        char appdata[MAX_PATH] = "";
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)) && appdata[0])
            dir = fs::path(appdata) / "fMCG";
        else
            dir = fs::path(".") / "fMCG_settings";   // last resort: portable
#elif defined(__APPLE__)
        const char* home = getenv("HOME");
        dir = fs::path(home ? home : ".") / "Library" / "Application Support" / "fMCG";
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
        cached = dir.string();
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
    try { return std::stod(v); } catch (...) { return def; }
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
    return (fs::path(base) / "patterns").string();
}

const char* k_default_layout =
    "Time: {time-milli}/{time-milli-max}\n"
    "Notes: {nc}/{nc-total}/{nc-rem}\n"
    "NPS: {nps}/{nps-max}\n"
    "Polyphony: {plph}/{plph-max}\n"
    "BPM: {bpm}";

} // namespace

// ---------------------------------------------------------------------------
// Global settings
// ---------------------------------------------------------------------------

bool load_global_settings(GlobalSettings& out) {
    std::string dir = settings_dir();
    if (dir.empty()) return false;
    std::ifstream f(fs::path(dir) / "settings.ini");
    if (!f.is_open()) return false;
    std::ostringstream ss; ss << f.rdbuf();
    KV kv = parse_ini(ss.str());
    out.midi_dir     = get_str(kv, "midi_dir");
    out.output_dir   = get_str(kv, "output_dir");
    out.width        = get_int(kv, "width", 1920);
    out.height       = get_int(kv, "height", 1080);
    out.fps          = get_dbl(kv, "fps", 60.0);
    out.vel0_note_off= get_bool(kv, "vel0_note_off", true);
    out.start_delay  = get_dbl(kv, "start_delay", 3.0);
    out.end_delay    = get_dbl(kv, "end_delay", 0.0);
    out.ffmpeg_threads = get_int(kv, "ffmpeg_threads", 0);
    if (out.fps <= 0) out.fps = 60.0;
    if (out.width <= 0) out.width = 1920;
    if (out.height <= 0) out.height = 1080;
    if (out.start_delay < 0) out.start_delay = 0.0;
    if (out.end_delay   < 0) out.end_delay   = 0.0;
    if (out.ffmpeg_threads < 0) out.ffmpeg_threads = 0;
    return true;
}

bool save_global_settings(const GlobalSettings& s) {
    std::string dir = settings_dir();
    if (dir.empty()) return false;
    std::ostringstream ss;
    ss << "# fMCG global settings (auto-saved)\n";
    ss << "midi_dir = " << s.midi_dir << "\n";
    ss << "output_dir = " << s.output_dir << "\n";
    ss << "width = " << s.width << "\n";
    ss << "height = " << s.height << "\n";
    ss << "fps = " << s.fps << "\n";
    ss << "vel0_note_off = " << (s.vel0_note_off ? 1 : 0) << "\n";
    ss << "start_delay = " << s.start_delay << "\n";
    ss << "end_delay = " << s.end_delay << "\n";
    ss << "ffmpeg_threads = " << s.ffmpeg_threads << "\n";
    return write_file_atomic(fs::path(dir) / "settings.ini", ss.str());
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
    return fs::exists(fs::path(pdir) / (name + ".ini"), ec);
}

bool save_pattern(const std::string& name, const PatternData& p) {
    std::string pdir = patterns_dir();
    if (pdir.empty() || name.empty()) return false;
    std::ostringstream ss;
    ss << "# fMCG pattern: " << name << "\n";
    ss << "layout = " << escape_newlines(p.layout_text) << "\n";
    ss << "alignment = " << p.alignment << "\n";
    ss << "pos_mode = " << p.pos_mode << "\n";
    ss << "pos_x = " << p.pos_x << "\n";
    ss << "pos_y = " << p.pos_y << "\n";
    ss << "font_family = " << p.font_family << "\n";
    ss << "font_variant = " << p.font_variant << "\n";
    ss << "font_size = " << p.font_size << "\n";
    ss << "text_color = " << p.text_color_aabbggrr << "\n";
    ss << "bg_color = " << p.bg_color_aabbggrr << "\n";
    ss << "comma_notes = " << (p.commas.notes ? 1 : 0) << "\n";
    ss << "comma_poly = " << (p.commas.polyphony ? 1 : 0) << "\n";
    ss << "comma_nps = " << (p.commas.nps ? 1 : 0) << "\n";
    ss << "comma_cc = " << (p.commas.cc ? 1 : 0) << "\n";
    ss << "comma_ticks = " << (p.commas.ticks ? 1 : 0) << "\n";
    ss << "pad_enabled = " << (p.pad.enabled ? 1 : 0) << "\n";
    int bpm_dec = p.bpm.decimals;
    if (bpm_dec < 0) bpm_dec = 0;
    if (bpm_dec > 6) bpm_dec = 6;
    ss << "bpm_decimals = " << bpm_dec << "\n";
    return write_file_atomic(fs::path(pdir) / (name + ".ini"), ss.str());
}

bool load_pattern(const std::string& name, PatternData& out) {
    std::string pdir = patterns_dir();
    if (pdir.empty() || name.empty()) return false;
    std::ifstream f(fs::path(pdir) / (name + ".ini"));
    if (!f.is_open()) return false;
    std::ostringstream ss; ss << f.rdbuf();
    KV kv = parse_ini(ss.str());
    out.layout_text          = unescape_newlines(get_str(kv, "layout", k_default_layout));
    out.alignment            = get_int(kv, "alignment", 0);
    out.pos_mode             = get_int(kv, "pos_mode", 0);
    out.pos_x                = get_int(kv, "pos_x", 30);
    out.pos_y                = get_int(kv, "pos_y", 30);
    out.font_family          = get_str(kv, "font_family", "Arial");
    out.font_variant         = get_str(kv, "font_variant");
    out.font_size            = get_int(kv, "font_size", 36);
    out.text_color_aabbggrr  = get_str(kv, "text_color", "&H00FFFFFF");
    out.bg_color_aabbggrr    = get_str(kv, "bg_color", "&H00000000");
    out.commas.notes         = get_bool(kv, "comma_notes", true);
    out.commas.polyphony     = get_bool(kv, "comma_poly", true);
    out.commas.nps           = get_bool(kv, "comma_nps", true);
    out.commas.cc            = get_bool(kv, "comma_cc", false);
    out.commas.ticks         = get_bool(kv, "comma_ticks", true);
    out.pad.enabled          = get_bool(kv, "pad_enabled", false);
    out.bpm.decimals         = get_int(kv, "bpm_decimals", 2);
    if (out.bpm.decimals < 0) out.bpm.decimals = 0;
    if (out.bpm.decimals > 6) out.bpm.decimals = 6;
    if (out.pos_mode < 0 || out.pos_mode > 1) out.pos_mode = 0;
    if (out.alignment < 0 || out.alignment > 5) out.alignment = 0;
    if (out.font_size <= 0) out.font_size = 36;
    return true;
}

bool delete_pattern(const std::string& name) {
    std::string pdir = patterns_dir();
    if (pdir.empty() || name.empty()) return false;
    std::error_code ec;
    return fs::remove(fs::path(pdir) / (name + ".ini"), ec) && !ec;
}

std::vector<std::string> list_patterns() {
    std::vector<std::string> out;
    std::string pdir = patterns_dir();
    if (pdir.empty()) return out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(pdir, ec)) {
        if (!e.is_regular_file()) continue;
        std::string fn = e.path().filename().string();
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
    p.layout_text = k_default_layout;
    save_pattern("Default", p);
}
