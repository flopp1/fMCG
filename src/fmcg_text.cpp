// fmcg_text.cpp -- see fmcg_text.h.
#include "fmcg_text.h"
#include "fmcg_path.h"

#include <hb.h>
#include <hb-ot.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace fmcg_text {

// ---- small helpers -----------------------------------------------------------

// Two hex digits at p -> 0..255, or -1 (never throws: colours come from
// hand-editable pattern files).
static int hex_byte(const char* p) {
    auto nib = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
             : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    const int hi = nib(p[0]), lo = nib(p[1]);
    return hi < 0 || lo < 0 ? -1 : hi * 16 + lo;
}

RGBA from_ass_colour(const std::string& ass) {
    if (ass.size() >= 8 && ass[0] == '&' && (ass[1] == 'H' || ass[1] == 'h')) {
        std::string hex = ass.substr(2);
        int aa = 0;
        if (hex.size() >= 8) { aa = hex_byte(hex.data()); hex = hex.substr(2, 6); }
        if (hex.size() >= 6 && aa >= 0) {
            const int b = hex_byte(hex.data()), g = hex_byte(hex.data() + 2), r = hex_byte(hex.data() + 4);
            if (b >= 0 && g >= 0 && r >= 0)
                return rgba((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)(255 - aa));   // ASS alpha: 00 = opaque
        }
    }
    return 0xFFFFFFFFu;
}

Rect Rect::united(const Rect& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    const int x1 = std::min(x, o.x), y1 = std::min(y, o.y);
    const int x2 = std::max(x + w, o.x + o.w), y2 = std::max(y + h, o.y + o.h);
    return {x1, y1, x2 - x1, y2 - y1};
}

Rect Rect::clipped(int W, int H) const {
    const int x1 = std::max(0, x), y1 = std::max(0, y);
    const int x2 = std::min(W, x + w), y2 = std::min(H, y + h);
    if (x2 <= x1 || y2 <= y1) return {};
    return {x1, y1, x2 - x1, y2 - y1};
}

void Canvas::fill_rect(Rect r, RGBA c) {
    r = r.clipped(w, h);
    if (r.empty()) return;
    const uint8_t p[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    for (int y = r.y; y < r.y + r.h; ++y) {
        uint8_t* q = row(y) + (size_t)r.x * 4;
        for (int x = 0; x < r.w; ++x, q += 4) std::memcpy(q, p, 4);
    }
}

// Straight-alpha "over" of colour c (its own alpha scaled by coverage a, 0..255).
static inline void blend(uint8_t* d, RGBA c, unsigned cov) {
    const unsigned sa = (unsigned)(c & 0xFF) * cov / 255;   // effective source alpha
    if (!sa) return;
    const unsigned sr = c >> 24, sg = (c >> 16) & 0xFF, sb = (c >> 8) & 0xFF;
    const unsigned da = d[3];
    if (sa == 255 || da == 0) {
        if (sa == 255) { d[0] = (uint8_t)sr; d[1] = (uint8_t)sg; d[2] = (uint8_t)sb; d[3] = 255; return; }
        d[0] = (uint8_t)sr; d[1] = (uint8_t)sg; d[2] = (uint8_t)sb; d[3] = (uint8_t)sa; return;
    }
    // out_a = sa + da (1 - sa);  out_c = (c sa + dc da (1 - sa)) / out_a
    const unsigned dw = da * (255 - sa) / 255;
    const unsigned oa = sa + dw;
    d[0] = (uint8_t)((sr * sa + d[0] * dw) / oa);
    d[1] = (uint8_t)((sg * sa + d[1] * dw) / oa);
    d[2] = (uint8_t)((sb * sa + d[2] * dw) / oa);
    d[3] = (uint8_t)oa;
}

// ---- implementation ----------------------------------------------------------

namespace {
constexpr int kPhases = 4;   // horizontal subpixel positions per pixel

struct Glyph {
    int w = 0, h = 0, x0 = 0, y0 = 0;    // coverage box relative to the (integer) pen origin
    std::vector<uint8_t> cov;
    int ow = 0, oh = 0, ox0 = 0, oy0 = 0; // outline (dilated) box
    std::vector<uint8_t> ocov;
};
} // namespace

struct TextRenderer::Impl {
    TextStyle style;
    std::vector<uint8_t> data;           // font file bytes (shared by stb and HarfBuzz)
    stbtt_fontinfo info{};
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    hb_font_t* font = nullptr;
    std::vector<hb_feature_t> features;
    unsigned upem = 1000;
    double scale = 1.0;                  // px per font unit
    double ascent = 0;                   // px
    std::unordered_map<uint64_t, Glyph> cache;

    ~Impl() { release(); }
    void release() {
        if (font) hb_font_destroy(font);
        if (face) hb_face_destroy(face);
        if (blob) hb_blob_destroy(blob);
        font = nullptr; face = nullptr; blob = nullptr;
        cache.clear();
    }

    const Glyph& glyph(uint32_t id, int phase) {
        const uint64_t key = ((uint64_t)id << 8) | (uint64_t)phase;
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        Glyph g;
        const float shift = (float)phase / kPhases;
        int ix0, iy0, ix1, iy1;
        stbtt_GetGlyphBitmapBoxSubpixel(&info, (int)id, (float)scale, (float)scale, shift, 0.0f, &ix0, &iy0, &ix1, &iy1);
        g.x0 = ix0; g.y0 = iy0; g.w = ix1 - ix0; g.h = iy1 - iy0;
        if (g.w > 0 && g.h > 0) {
            g.cov.assign((size_t)g.w * g.h, 0);
            stbtt_MakeGlyphBitmapSubpixel(&info, g.cov.data(), g.w, g.h, g.w, (float)scale, (float)scale, shift, 0.0f, (int)id);
        }
        if (style.outline && style.outline_width > 0 && g.w > 0 && g.h > 0) {
            // Disc dilation of the coverage: every output pixel takes the max
            // coverage within outline_width (+0.5 px soft edge).
            const double r = style.outline_width;
            const int R = (int)std::ceil(r + 0.5);
            g.ox0 = g.x0 - R; g.oy0 = g.y0 - R; g.ow = g.w + 2 * R; g.oh = g.h + 2 * R;
            g.ocov.assign((size_t)g.ow * g.oh, 0);
            for (int sy = 0; sy < g.h; ++sy)
                for (int sx = 0; sx < g.w; ++sx) {
                    const unsigned c = g.cov[(size_t)sy * g.w + sx];
                    if (!c) continue;
                    for (int dy = -R; dy <= R; ++dy)
                        for (int dx = -R; dx <= R; ++dx) {
                            const double wgt = r + 0.5 - std::sqrt((double)(dx * dx + dy * dy));
                            if (wgt <= 0) continue;
                            const unsigned v = wgt >= 1 ? c : (unsigned)(c * wgt);
                            uint8_t& o = g.ocov[(size_t)(sy + R + dy) * g.ow + (sx + R + dx)];
                            if (v > o) o = (uint8_t)v;
                        }
                }
        }
        return cache.emplace(key, std::move(g)).first->second;
    }
};

TextRenderer::TextRenderer() : d(new Impl) {}
TextRenderer::~TextRenderer() = default;
bool TextRenderer::loaded() const { return d->font != nullptr; }
const TextStyle& TextRenderer::style() const { return d->style; }

static void read_font_file(const std::string& path, std::vector<uint8_t>& data) {
    data.clear();
    std::ifstream f(path_from_utf8(path), std::ios::binary);   // UTF-8 path (wide on Windows)
    data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool TextRenderer::load(const TextStyle& style, std::string* err) {
    d->release();
    d->style = style;
    read_font_file(style.font_file, d->data);
    if (d->data.empty()) { if (err) *err = "cannot read font file: " + style.font_file; return false; }

    const int off = stbtt_GetFontOffsetForIndex(d->data.data(), style.face_index);
    if (off < 0 || !stbtt_InitFont(&d->info, d->data.data(), off)) {
        if (err) *err = "unsupported font: " + style.font_file;
        return false;
    }
    d->blob = hb_blob_create((const char*)d->data.data(), (unsigned)d->data.size(), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    d->face = hb_face_create(d->blob, (unsigned)style.face_index);
    d->font = hb_font_create(d->face);
    d->upem = hb_face_get_upem(d->face);
    hb_font_set_scale(d->font, (int)d->upem, (int)d->upem);   // positions come back in font units

    // Size like libass/VSFilter: (Windows ascent + descent) = size px, from
    // OS/2 usWinAscent/usWinDescent when present, else hhea.
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&d->info, &asc, &desc, &gap);
    const stbtt_uint32 os2 = stbtt__find_table(d->info.data, (stbtt_uint32)d->info.fontstart, "OS/2");
    if (os2) {
        const int wa = ttUSHORT(d->info.data + os2 + 74), wd = ttUSHORT(d->info.data + os2 + 76);
        if (wa + wd > 0) { asc = wa; desc = -wd; }
    }
    if (asc - desc <= 0) { asc = (int)d->upem; desc = 0; }
    d->scale = style.size / (double)(asc - desc);
    d->ascent = asc * d->scale;

    d->features.clear();
    for (const std::string& fs : style.features) {
        hb_feature_t ft;
        if (hb_feature_from_string(fs.c_str(), (int)fs.size(), &ft)) d->features.push_back(ft);
    }
    return true;
}

// Shapes `s` onto line `sl` starting at pen `pen` (letter spacing between
// clusters, never inside one -- ligatures, combining marks -- and never after
// the last); returns the pen after the text.
static double shape_run(const TextRenderer::Impl& d, hb_buffer_t* buf, const std::string& s, ShapedLine& sl,
                        double pen) {
    hb_buffer_reset(buf);
    hb_buffer_add_utf8(buf, s.c_str(), (int)s.size(), 0, (int)s.size());
    hb_buffer_guess_segment_properties(buf);
    hb_shape(d.font, buf, d.features.empty() ? nullptr : d.features.data(), (unsigned)d.features.size());
    unsigned n = 0;
    const hb_glyph_info_t* gi = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* gp = hb_buffer_get_glyph_positions(buf, &n);
    const double ls = d.style.letter_spacing;
    sl.glyphs.reserve(sl.glyphs.size() + n);
    for (unsigned i = 0; i < n; ++i) {
        if (ls != 0 && i > 0 && gi[i].cluster != gi[i - 1].cluster) pen += ls;
        sl.glyphs.push_back({gi[i].codepoint, pen + gp[i].x_offset * d.scale, -gp[i].y_offset * d.scale});
        pen += gp[i].x_advance * d.scale;
    }
    return pen;
}

static Layout empty_layout(const TextRenderer::Impl& d) {
    Layout L;
    L.line_height = d.style.size * (d.style.line_spacing > 0 ? d.style.line_spacing : 1.0);
    L.em = d.style.size;
    L.ascent = d.ascent;
    return L;
}

Layout TextRenderer::layout(const std::vector<std::string>& lines) const {
    Layout L = empty_layout(*d);
    hb_buffer_t* buf = hb_buffer_create();
    for (const std::string& s : lines) {
        ShapedLine sl;
        sl.advance = shape_run(*d, buf, s, sl, 0.0);
        L.width = std::max(L.width, sl.advance);
        L.lines.push_back(std::move(sl));
    }
    hb_buffer_destroy(buf);
    return L;
}

Layout TextRenderer::layout(const PieceLines& lines, const std::vector<std::vector<double>>& cell_w,
                            int cell_align) const {
    Layout L = empty_layout(*d);
    hb_buffer_t* buf = hb_buffer_create();
    const double ls = d->style.letter_spacing;
    for (size_t li = 0; li < lines.size(); ++li) {
        ShapedLine sl;
        double pen = 0;
        bool started = false;   // letter spacing also between pieces (cells count even when empty)
        for (const TextPiece& p : lines[li]) {
            const bool cell = p.cell >= 0 && li < cell_w.size() && (size_t)p.cell < cell_w[li].size();
            if (p.text.empty() && !cell) continue;
            if (started) pen += ls;
            started = true;
            if (!cell) {
                pen = shape_run(*d, buf, p.text, sl, pen);
                continue;
            }
            ShapedLine run;
            const double a = shape_run(*d, buf, p.text, run, 0.0);
            const double w = std::max(cell_w[li][p.cell], a);
            const double off = cell_align == 2 ? w - a : cell_align == 1 ? (w - a) / 2.0 : 0.0;
            for (ShapedGlyph g : run.glyphs) {
                g.x += pen + off;
                sl.glyphs.push_back(g);
            }
            pen += w;
        }
        sl.advance = pen;
        L.width = std::max(L.width, pen);
        L.lines.push_back(std::move(sl));
    }
    hb_buffer_destroy(buf);
    return L;
}

double TextRenderer::advance(const std::string& text) const {
    hb_buffer_t* buf = hb_buffer_create();
    ShapedLine sl;
    const double a = shape_run(*d, buf, text, sl, 0.0);
    hb_buffer_destroy(buf);
    return a;
}

static bool align_right(int a) { return a == 3 || a == 6 || a == 9; }
static bool align_center(int a) { return a == 2 || a == 5 || a == 8; }
static bool align_bottom(int a) { return a == 1 || a == 2 || a == 3; }
static bool align_middle(int a) { return a == 4 || a == 5 || a == 6; }

void TextRenderer::place(const Layout& L, const Placement& p, double& x0, double& y0) const {
    const double w = L.width, h = L.height();
    if (p.pos_mode == 1) {
        // Reference point on the frame, then the block's anchor point on it.
        const int fa = p.frame_anchor, ba = p.block_anchor;
        const double X = align_right(fa) ? p.frame_w - p.pos_x
                       : align_center(fa) ? p.frame_w / 2.0 + p.pos_x : (double)p.pos_x;
        const double Y = align_bottom(fa) ? p.frame_h - p.pos_y
                       : align_middle(fa) ? p.frame_h / 2.0 + p.pos_y : (double)p.pos_y;
        x0 = X - (align_right(ba) ? w : align_center(ba) ? w / 2.0 : 0.0);
        y0 = Y - (align_bottom(ba) ? h : align_middle(ba) ? h / 2.0 : 0.0);
    } else {
        const double ax = align_right(p.alignment) ? p.frame_w - p.margin
                        : align_center(p.alignment) ? p.frame_w / 2.0 : (double)p.margin;
        const double ay = align_bottom(p.alignment) ? p.frame_h - p.margin : (double)p.margin;
        x0 = align_right(p.alignment) ? ax - w : align_center(p.alignment) ? ax - w / 2.0 : ax;
        y0 = align_bottom(p.alignment) ? ay - h : ay;
    }
    // Keep the whole block (and its outline) on screen whenever it fits; a
    // block larger than the frame starts at the top-left edge. The 1 px edge
    // covers glyph bitmaps, which round outward beyond the em box.
    const double pad = 1.0 + (d->style.outline ? std::ceil(d->style.outline_width + 0.5) : 0.0);
    x0 = std::max(pad, std::min(x0, p.frame_w - w - pad));
    y0 = std::max(pad, std::min(y0, p.frame_h - h - pad));
    if (w + 2 * pad > p.frame_w) x0 = pad;
    if (h + 2 * pad > p.frame_h) y0 = pad;
}

double TextRenderer::line_offset(const Layout& L, size_t i, const Placement& p) const {
    if (i >= L.lines.size()) return 0.0;
    const int a = p.pos_mode == 1 ? p.block_anchor : p.alignment;
    const double slack = L.width - L.lines[i].advance;
    return align_right(a) ? slack : align_center(a) ? slack / 2.0 : 0.0;
}

// Visits every glyph's integer pen origin + cached bitmap.
template <class F>
static void each_glyph(TextRenderer::Impl& d, const TextRenderer& tr, const Layout& L, const Placement& p,
                       double x0, double y0, F f) {
    for (size_t li = 0; li < L.lines.size(); ++li) {
        const double lx = x0 + tr.line_offset(L, li, p);
        const double base = y0 + L.line_height * (double)li + L.ascent;
        for (const ShapedGlyph& g : L.lines[li].glyphs) {
            const double gx = lx + g.x;
            const double fx = std::floor(gx);
            int phase = (int)std::lround((gx - fx) * kPhases);
            int ix = (int)fx;
            if (phase == kPhases) { phase = 0; ++ix; }
            const int iy = (int)std::lround(base + g.y);
            f(d.glyph(g.id, phase), ix, iy);
        }
    }
}

Rect TextRenderer::bounds(const Layout& L, const Placement& p, double x0, double y0) const {
    Rect r{};
    each_glyph(*d, *this, L, p, x0, y0, [&](const Glyph& g, int ix, int iy) {
        if (!g.ocov.empty()) r = r.united({ix + g.ox0, iy + g.oy0, g.ow, g.oh});
        else if (g.w > 0) r = r.united({ix + g.x0, iy + g.y0, g.w, g.h});
    });
    return r;
}

Rect TextRenderer::draw(Canvas& c, const Layout& L, const Placement& p, double x0, double y0) {
    const TextStyle& st = d->style;
    auto stamp = [&](const uint8_t* cov, int w, int h, int gx, int gy, RGBA col) {
        for (int y = 0; y < h; ++y) {
            const int cy = gy + y;
            if (cy < 0 || cy >= c.h) continue;
            uint8_t* rowp = c.row(cy);
            for (int x = 0; x < w; ++x) {
                const int cx = gx + x;
                if (cx < 0 || cx >= c.w) continue;
                const unsigned a = cov[(size_t)y * w + x];
                if (a) blend(rowp + (size_t)cx * 4, col, a);
            }
        }
    };
    // Outline first (under every glyph's fill), then the fill.
    if (st.outline)
        each_glyph(*d, *this, L, p, x0, y0, [&](const Glyph& g, int ix, int iy) {
            if (!g.ocov.empty()) stamp(g.ocov.data(), g.ow, g.oh, ix + g.ox0, iy + g.oy0, st.outline_colour);
        });
    each_glyph(*d, *this, L, p, x0, y0, [&](const Glyph& g, int ix, int iy) {
        if (!g.cov.empty()) stamp(g.cov.data(), g.w, g.h, ix + g.x0, iy + g.y0, st.colour);
    });
    return bounds(L, p, x0, y0).clipped(c.w, c.h);
}

// ---- FrameRenderer -----------------------------------------------------------

bool FrameRenderer::init(const TextStyle& style, const Placement& placement, RGBA background, std::string* err) {
    place_ = placement;
    bg_ = background;
    canvas_.resize(placement.frame_w, placement.frame_h);
    canvas_.fill(bg_);
    last_ = {};
    dirty_ = {0, 0, canvas_.w, canvas_.h};
    cell_w_.clear();
    scale_ = 1.0;
    block_w_ = block_h_ = 0;
    return text_.load(style, err);
}

bool FrameRenderer::init(const TextStyle& style, const Placement& placement, RGBA background,
                         const TextBlockSpec& spec, std::string* err) {
    if (!init(style, placement, background, err)) return false;
    // Cell widths (widest candidate per stat) and the largest block the
    // video can show, at the current font size.
    std::vector<std::vector<double>> widths;
    auto measure = [&]() {
        widths.assign(spec.pieces.size(), {});
        for (size_t li = 0; li < spec.pieces.size() && li < spec.extremes.size(); ++li)
            for (const auto& cands : spec.extremes[li]) {
                double w = 0;
                for (const std::string& c : cands) w = std::max(w, text_.advance(c));
                widths[li].push_back(w);
            }
        Layout L;
        if (place_.cells) {          // exact: literals + fixed cells
            PieceLines empty = spec.pieces;
            for (auto& line : empty)
                for (auto& p : line)
                    if (p.cell >= 0) p.text.clear();
            L = text_.layout(empty, widths, place_.cell_align);
        } else {                     // each stat at its widest candidate
            std::vector<std::string> worst;
            for (size_t li = 0; li < spec.pieces.size(); ++li) {
                std::string s;
                for (const TextPiece& p : spec.pieces[li]) {
                    if (p.cell < 0 || li >= spec.extremes.size() || (size_t)p.cell >= spec.extremes[li].size()) {
                        s += p.text;
                        continue;
                    }
                    const auto& cands = spec.extremes[li][p.cell];
                    std::string best = p.text;
                    double bw = text_.advance(best);
                    for (const std::string& c : cands) {
                        const double w = text_.advance(c);
                        if (w > bw) { bw = w; best = c; }
                    }
                    s += best;
                }
                worst.push_back(s);
            }
            L = text_.layout(worst);
        }
        block_w_ = L.width;
        block_h_ = L.height();
    };
    measure();
    if (place_.fit && block_w_ > 0 && block_h_ > 0) {
        // Available area: the frame, minus the corner margin in preset mode.
        double aw = place_.frame_w, ah = place_.frame_h;
        if (place_.pos_mode == 0) {
            aw -= 2.0 * place_.margin;
            ah -= 2.0 * place_.margin;
            if (aw <= 0) aw = place_.frame_w;
            if (ah <= 0) ah = place_.frame_h;
        }
        const double pad = 2.0 + (style.outline ? 2.0 * (style.outline_width + 1.0) : 0.0);
        const double s = std::min({1.0, (aw - pad) / block_w_, (ah - pad) / block_h_});
        if (s < 1.0) {
            // One scale for the whole video (metrics are linear in the size);
            // 2% headroom for glyph ink beyond the advance box.
            scale_ = std::max(0.01, s * 0.98);
            TextStyle st = style;
            st.size *= scale_;
            st.letter_spacing *= scale_;
            st.outline_width *= scale_;
            if (!text_.load(st, err)) return false;
            measure();
        }
    }
    if (place_.cells) cell_w_ = widths;
    return true;
}

const Canvas& FrameRenderer::draw(const Layout& L) {
    canvas_.fill_rect(last_, bg_);
    double x0 = 0, y0 = 0;
    text_.place(L, place_, x0, y0);
    const Rect now = text_.draw(canvas_, L, place_, x0, y0);
    dirty_ = last_.united(now);
    last_ = now;
    return canvas_;
}

const Canvas& FrameRenderer::render(const std::vector<std::string>& lines) {
    return draw(text_.layout(lines));
}

Layout FrameRenderer::layout_pieces(const PieceLines& pieces) const {
    if (place_.cells && !cell_w_.empty()) return text_.layout(pieces, cell_w_, place_.cell_align);
    std::vector<std::string> lines;   // without cells: whole-line shaping (kerning across stats)
    lines.reserve(pieces.size());
    for (const auto& line : pieces) {
        std::string s;
        for (const TextPiece& p : line) s += p.text;
        lines.push_back(std::move(s));
    }
    return text_.layout(lines);
}

const Canvas& FrameRenderer::render_pieces(const PieceLines& pieces) {
    return draw(layout_pieces(pieces));
}

// ---- typography options ------------------------------------------------------------

std::vector<std::string> split_features(const std::string& list) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : list) {
        if (c == ' ' || c == ',' || c == ';' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool valid_feature(const std::string& f) {
    hb_feature_t ft;
    return !f.empty() && hb_feature_from_string(f.c_str(), (int)f.size(), &ft);
}

std::vector<std::string> Typography::features() const {
    std::vector<std::string> v;
    if (!kerning) v.push_back("-kern");
    if (!ligatures) v.push_back("-liga");
    if (slashed_zero) v.push_back("zero");
    if (numerals == 1) v.push_back("lnum");
    if (numerals == 2) v.push_back("onum");
    if (numeral_width == 1) v.push_back("pnum");
    if (numeral_width == 2) v.push_back("tnum");
    for (int n = 1; n <= 20; ++n)
        if (stylistic_sets & (1u << (n - 1))) {
            char tag[8];
            std::snprintf(tag, sizeof tag, "ss%02d", n);
            v.push_back(tag);
        }
    for (const std::string& f : split_features(extra))   // later features override earlier ones
        if (valid_feature(f)) v.push_back(f);
    return v;
}

void Typography::apply(TextStyle& st) const {
    st.features = features();
    st.letter_spacing = letter_spacing;
    st.line_spacing = line_spacing > 0 ? line_spacing : 1.0;
}

bool Typography::operator==(const Typography& o) const {
    return kerning == o.kerning && ligatures == o.ligatures && slashed_zero == o.slashed_zero &&
           numerals == o.numerals && numeral_width == o.numeral_width &&
           stylistic_sets == o.stylistic_sets && extra == o.extra &&
           letter_spacing == o.letter_spacing && line_spacing == o.line_spacing;
}

static std::string tag_string(hb_tag_t t) {
    char s[5];
    hb_tag_to_string(t, s);
    s[4] = 0;
    return s;
}

std::vector<FontFeature> font_features(const std::string& font_file, int face_index) {
    std::vector<FontFeature> out;
    std::vector<uint8_t> data;
    read_font_file(font_file, data);
    if (data.empty()) return out;
    hb_blob_t* blob = hb_blob_create((const char*)data.data(), (unsigned)data.size(), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hb_face_t* face = hb_face_create(blob, (unsigned)face_index);
    std::unordered_map<std::string, std::string> seen;   // tag -> name
    for (hb_tag_t table : {HB_OT_TAG_GSUB, HB_OT_TAG_GPOS}) {
        const unsigned total = hb_ot_layout_table_get_feature_tags(face, table, 0, nullptr, nullptr);
        std::vector<hb_tag_t> tags(total);
        unsigned n = total;
        hb_ot_layout_table_get_feature_tags(face, table, 0, &n, tags.data());
        for (unsigned i = 0; i < n; ++i) {   // feature index == position in this list
            const std::string tag = tag_string(tags[i]);
            std::string& name = seen[tag];
            if (!name.empty()) continue;
            hb_ot_name_id_t label = HB_OT_NAME_ID_INVALID;
            if (table == HB_OT_TAG_GSUB &&
                hb_ot_layout_feature_get_name_ids(face, table, i, &label, nullptr, nullptr, nullptr, nullptr) &&
                label != HB_OT_NAME_ID_INVALID) {
                unsigned len = hb_ot_name_get_utf8(face, label, HB_LANGUAGE_INVALID, nullptr, nullptr);
                if (len) {
                    std::string s(len + 1, '\0');
                    unsigned cap = len + 1;
                    hb_ot_name_get_utf8(face, label, HB_LANGUAGE_INVALID, &cap, &s[0]);
                    s.resize(cap);
                    name = s;
                }
            }
        }
    }
    // legacy TrueType 'kern' table: HarfBuzz applies it as the "kern" feature
    if (!seen.count("kern")) {
        hb_blob_t* k = hb_face_reference_table(face, HB_TAG('k', 'e', 'r', 'n'));
        if (hb_blob_get_length(k) > 0) seen["kern"];
        hb_blob_destroy(k);
    }
    hb_face_destroy(face);
    hb_blob_destroy(blob);
    for (auto& kv : seen) out.push_back({kv.first, kv.second});
    std::sort(out.begin(), out.end(), [](const FontFeature& a, const FontFeature& b) { return a.tag < b.tag; });
    return out;
}

} // namespace fmcg_text
