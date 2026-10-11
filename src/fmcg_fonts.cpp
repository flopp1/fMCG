#include "fmcg_fonts.h"

#include <fstream>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <set>
#include <sys/stat.h>
#include <dirent.h>

namespace {

inline uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
inline uint16_t read_be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

inline std::string utf16be_to_utf8(const uint8_t* s, size_t len) {
    std::string out;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint32_t cp = ((uint32_t)s[i] << 8) | s[i + 1];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) {
            uint32_t lo = ((uint32_t)s[i + 2] << 8) | s[i + 3];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// Extract one string from the sfnt 'name' table by name ID.
// Language-aware: records come in many languages (a legacy Rockwell Bold
// lists ID 2 as "Negreta" for Spanish and "Bold" for English), so prefer
// platform 3 encoding 1/10 language 0x0409 (en-US), then Mac Roman, then
// whatever appears first.
inline bool extract_sfnt_name_id(std::ifstream& f, uint64_t base, uint16_t want_id, std::string& out) {
    f.clear();
    f.seekg((std::streamoff)base, std::ios::beg);
    uint8_t hdr[12];
    if (!f.read((char*)hdr, 12)) return false;
    uint16_t num_tables = read_be16(hdr + 4);
    if (num_tables == 0 || num_tables > 512) return false;

    std::vector<uint8_t> dir((size_t)num_tables * 16);
    if (!f.read((char*)dir.data(), (std::streamsize)dir.size())) return false;

    uint64_t name_off = 0, name_len = 0;
    for (uint16_t i = 0; i < num_tables; ++i) {
        const uint8_t* rec = dir.data() + (size_t)i * 16;
        if (read_be32(rec) == 0x6E616D65) {
            name_off = read_be32(rec + 8);
            name_len = read_be32(rec + 12);
            break;
        }
    }
    if (name_off == 0 || name_len < 6) return false;

    // Table offsets are from the start of the FILE, also inside a TTC (the
    // subfont header sits at `base`, its tables anywhere in the file).
    f.clear();
    f.seekg((std::streamoff)name_off, std::ios::beg);
    std::vector<uint8_t> nt((size_t)std::min<uint64_t>(name_len, 1u << 16));
    if (!f.read((char*)nt.data(), (std::streamsize)nt.size())) return false;

    uint16_t count = read_be16(nt.data() + 2);
    uint16_t str_off = read_be16(nt.data() + 4);
    // Sanity-check the table header before trusting its records (format 0, or
    // 1 = with language tags; a few dozen records; string storage after the
    // record array). Damaged tables would otherwise yield garbage names.
    uint16_t format = read_be16(nt.data());
    if (format > 1 || count == 0 || count > 2048) return false;
    if ((uint32_t)str_off < 6u + (uint32_t)count * 12u) return false;
    int best_score = -1;
    size_t rec_pos = 6;
    for (uint32_t i = 0; i < count && rec_pos + 12 <= nt.size(); ++i, rec_pos += 12) {
        const uint8_t* rec = nt.data() + rec_pos;
        uint16_t platform = read_be16(rec);
        uint16_t encoding = read_be16(rec + 2);
        uint16_t lang     = read_be16(rec + 4);
        uint16_t name_id = read_be16(rec + 6);
        uint16_t length = read_be16(rec + 8);
        uint16_t offset = read_be16(rec + 10);
        if (name_id != want_id) continue;

        uint32_t off = (uint32_t)str_off + offset;
        if (length == 0 || off + length > nt.size()) continue;

        // Score: higher wins. en-US Windows Unicode first, then any Windows
        // Unicode, then Mac Roman, then leftovers.
        int score;
        if (platform == 3 && (encoding == 1 || encoding == 10))
            score = (lang == 0x0409) ? 4 : 3;
        else if (platform == 0)
            score = 2;
        else if (platform == 3 || platform == 1)
            score = 1;
        else continue;
        if (score <= best_score) continue;

        std::string val;
        if (platform == 3 && (encoding == 1 || encoding == 10))
            val = utf16be_to_utf8(nt.data() + off, length);
        else if (platform == 0)
            val = utf16be_to_utf8(nt.data() + off, length);
        else
            val.assign((const char*)nt.data() + off, length);
        if (val.empty()) continue;
        out = val;
        best_score = score;
    }
    return best_score >= 0;
}

// Family name: prefer the typographic family (ID 16), fall back to ID 1.
// Validates the sfnt version at `base` first.
inline bool extract_family_from_sfnt(std::ifstream& f, uint64_t base, std::string& out_family) {
    f.clear();
    f.seekg((std::streamoff)base, std::ios::beg);
    uint8_t v[4];
    if (!f.read((char*)v, 4)) return false;
    uint32_t version = read_be32(v);
    if (version != 0x00010000 && version != 0x4F54544F && version != 0x74727565)
        return false;
    if (extract_sfnt_name_id(f, base, 16, out_family) && !out_family.empty()) return true;
    return extract_sfnt_name_id(f, base, 1, out_family) && !out_family.empty();
}

bool ci_less_str(const std::string& a, const std::string& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)tolower((unsigned char)a[i]);
        unsigned char cb = (unsigned char)tolower((unsigned char)b[i]);
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

} // namespace

void collect_font_files(const std::string& dir, std::vector<std::string>& out, int depth) {
    if (depth > 8) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collect_font_files(full, out, depth + 1);
        } else {
            size_t dot = name.find_last_of('.');
            std::string ext = (dot == std::string::npos) ? "" : name.substr(dot);
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") out.push_back(full);
        }
    }
    closedir(d);
}


// sfnt header offsets of every face in the file: {0} for a plain ttf/otf,
// one per subfont for a TTC collection, empty if not a font.
static std::vector<uint64_t> face_bases(std::ifstream& f) {
    std::vector<uint64_t> bases;
    f.clear();
    f.seekg(0, std::ios::beg);
    uint8_t hdr[12];
    if (!f.read((char*)hdr, 12)) return bases;
    const uint32_t version = read_be32(hdr);
    if (version == 0x74746366) {   // 'ttcf'
        const uint32_t num = read_be32(hdr + 8);
        if (num == 0 || num > 256) return bases;
        // Read ALL offsets before parsing any face: the parsers seek the stream.
        for (uint32_t i = 0; i < num; ++i) {
            uint8_t ob[4];
            if (!f.read((char*)ob, 4)) break;
            bases.push_back(read_be32(ob));
        }
    } else if (version == 0x00010000 || version == 0x4F54544F || version == 0x74727565) {
        bases.push_back(0);
    }
    return bases;
}

// Every family name in the file (a TTC can hold several, e.g. Cambria +
// Cambria Math), in face order, without duplicates.
std::vector<std::string> read_font_families(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    for (uint64_t base : face_bases(f)) {
        std::string fam;
        if (extract_family_from_sfnt(f, base, fam) && std::find(out.begin(), out.end(), fam) == out.end())
            out.push_back(fam);
    }
    return out;
}

std::string read_font_family(const std::string& path) {
    const std::vector<std::string> fams = read_font_families(path);
    return fams.empty() ? "" : fams[0];
}

std::vector<std::string> get_font_directories() {
    std::vector<std::string> dirs;
#if defined(_WIN32)
    const char* windir = std::getenv("WINDIR");
    if (windir) dirs.push_back(std::string(windir) + "/Fonts");
    const char* local = std::getenv("LOCALAPPDATA");
    if (local) dirs.push_back(std::string(local) + "/Microsoft/Windows/Fonts");
#else
    for (const char* d : {"/usr/share/fonts", "/usr/local/share/fonts", "/System/Library/Fonts", "/Library/Fonts"})
        dirs.push_back(d);
#endif
    const char* home = std::getenv("HOME");
#if defined(_WIN32)
    if (!home) home = std::getenv("USERPROFILE");
    if (home) dirs.push_back(std::string(home) + "/AppData/Local/Microsoft/Windows/Fonts");
    // %LOCALAPPDATA% and %USERPROFILE%/AppData/Local name the same folder with
    // different separators: normalise, or every per-user font is listed twice.
    for (std::string& d : dirs) std::replace(d.begin(), d.end(), '\\', '/');
#else
    if (home) {
        dirs.push_back(std::string(home) + "/.fonts");
        dirs.push_back(std::string(home) + "/.local/share/fonts");
        dirs.push_back(std::string(home) + "/Library/Fonts");
    }
#endif
    std::sort(dirs.begin(), dirs.end(), ci_less_str);
    dirs.erase(std::unique(dirs.begin(), dirs.end(), [](const std::string& a, const std::string& b) {
        return !ci_less_str(a, b) && !ci_less_str(b, a);
    }), dirs.end());
    return dirs;
}

// Family names the UI font atlas can draw (Basic Latin + Latin-1, 0x20..0xFF).
// CJK/Arabic/... names would render as rows of '?' in the dropdown. Decodes
// UTF-8: a raw byte test can never reject CJK, whose multibyte sequences are
// built entirely from bytes <= 0xFF.
static bool renderable_name(const std::string& fam) {
    for (size_t i = 0; i < fam.size(); ) {
        unsigned char c = (unsigned char)fam[i];
        uint32_t cp; size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else return false;   // invalid lead byte
        if (i + len > fam.size()) return false;
        for (size_t k = 1; k < len; ++k) {
            unsigned char cc = (unsigned char)fam[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < 0x20 || cp > 0xFF) return false;
        i += len;
    }
    return true;
}

std::vector<std::string> enumerate_system_fonts() {
    std::set<std::string, bool (*)(const std::string&, const std::string&)> families(ci_less_str);
    std::vector<std::string> files;
    for (const std::string& dir : get_font_directories()) {
        files.clear();
        collect_font_files(dir, files, 0);
        for (const auto& path : files)
            for (const std::string& fam : read_font_families(path))
                if (!fam.empty() && renderable_name(fam)) families.insert(fam);
    }
    return std::vector<std::string>(families.begin(), families.end());
}

// All faces belonging to a family (style name read from ID 17, falling back
// to ID 2; weight/italic from the OS/2 table when present).
std::vector<FontVariant> enumerate_font_variants(const std::string& family) {
    std::vector<FontVariant> out;
    std::set<std::string> seen_files;
    for (const std::string& dir : get_font_directories()) {
        std::vector<std::string> files;
        collect_font_files(dir, files, 0);
        for (const auto& path : files) {
            if (!seen_files.insert(path).second) continue;
            std::ifstream f(path, std::ios::binary);
            if (!f) continue;
            const std::vector<uint64_t> bases = face_bases(f);
            f.clear();
            f.seekg(0, std::ios::end);
            const std::streamoff fsize = f.tellg();

            for (size_t face = 0; face < bases.size(); ++face) {
                const uint64_t base = bases[face];
                std::string fam;
                if (!extract_family_from_sfnt(f, base, fam) || fam.empty()) continue;
                if (ci_less_str(fam, family) || ci_less_str(family, fam)) continue;

                FontVariant v;
                v.file = path;
                v.face_index = (int)face;
                if (!extract_sfnt_name_id(f, base, 17, v.style) || v.style.empty())
                    extract_sfnt_name_id(f, base, 2, v.style);
                if (v.style.empty()) v.style = "Regular";

                // OS/2 table: usWeightClass at offset 4, fsSelection at 62
                // (bit 0 = italic).
                f.clear();
                f.seekg((std::streamoff)base, std::ios::beg);
                uint8_t h2[12];
                if (f.read((char*)h2, 12)) {
                    uint16_t num_tables = read_be16(h2 + 4);
                    std::vector<uint8_t> dir((size_t)num_tables * 16);
                    if (num_tables > 0 && num_tables <= 512 && f.read((char*)dir.data(), (std::streamsize)dir.size())) {
                        for (uint16_t i = 0; i < num_tables; ++i) {
                            const uint8_t* rec = dir.data() + (size_t)i * 16;
                            if (read_be32(rec) != 0x4F532F32) continue;   // "OS/2"
                            uint64_t os2 = read_be32(rec + 8);   // file-absolute (TTC too)
                            if (os2 + 64 > (uint64_t)fsize) break;
                            f.clear(); f.seekg((std::streamoff)os2, std::ios::beg);
                            uint8_t t[64];
                            if (f.read((char*)t, 64)) {
                                v.weight = read_be16(t + 4);
                                v.italic = (read_be16(t + 62) & 1) != 0;
                            }
                            break;
                        }
                    }
                }

                // One entry per style: the same family+style in two files
                // (or two faces) would be indistinguishable in the dropdown.
                bool dup = false;
                for (const auto& e : out) if (e.style == v.style) { dup = true; break; }
                if (!dup) out.push_back(std::move(v));
            }
        }
    }
    // Stable presentation: Regular first, then by weight, then by style name.
    std::stable_sort(out.begin(), out.end(), [](const FontVariant& a, const FontVariant& b) {
        auto is_reg = [](const std::string& s) {
            return !ci_less_str(s, "regular") && !ci_less_str("regular", s);
        };
        bool ra = is_reg(a.style), rb = is_reg(b.style);
        if (ra != rb) return ra > rb;   // Regular first
        if (a.weight != b.weight) return a.weight < b.weight;
        return a.style < b.style;
    });
    return out;
}

// Resolve a family + style (subfamily) to a concrete font file + face. Empty
// style matches the Regular variant / first face of the family.
std::string find_font_file_for_variant(const std::string& family, const std::string& style, int* face_index) {
    if (face_index) *face_index = 0;
    const std::vector<FontVariant> vs = enumerate_font_variants(family);
    for (const auto& v : vs)
        if (!style.empty() && v.style == style) {
            if (face_index) *face_index = v.face_index;
            return v.file;
        }
    if (!vs.empty()) {   // empty / vanished style: Regular (sorted first)
        if (face_index) *face_index = vs[0].face_index;
        return vs[0].file;
    }
    return find_font_file_for_family(family, face_index);
}

std::string find_font_file_for_family(const std::string& family, int* face_index) {
    if (face_index) *face_index = 0;
    std::string first_file;
    for (const std::string& dir : get_font_directories()) {
        std::vector<std::string> files;
        collect_font_files(dir, files, 0);
        for (const auto& path : files) {
            if (first_file.empty()) first_file = path;
            if (family.empty()) continue;
            std::ifstream f(path, std::ios::binary);
            const std::vector<uint64_t> bases = face_bases(f);
            for (size_t face = 0; face < bases.size(); ++face) {
                std::string fam;
                if (extract_family_from_sfnt(f, bases[face], fam) &&
                    !ci_less_str(fam, family) && !ci_less_str(family, fam)) {
                    if (face_index) *face_index = (int)face;
                    return path;
                }
            }
        }
    }
    return first_file;
}
