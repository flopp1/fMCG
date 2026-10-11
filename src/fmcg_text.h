// fmcg_text.h -- in-built text renderer: font loading, OpenType shaping
// (HarfBuzz), layout/placement, and rasterization into an RGBA canvas
// (stb_truetype). The SAME code renders the preview and the video frames,
// so what the preview shows is exactly what gets encoded.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fmcg_text {

// Straight (non-premultiplied) RGBA, packed 0xRRGGBBAA.
using RGBA = uint32_t;
inline RGBA rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return ((RGBA)r << 24) | ((RGBA)g << 16) | ((RGBA)b << 8) | a;
}
// "&HAABBGGRR" / "&HBBGGRR" (ASS colour, AA = transparency) -> RGBA; opaque fallback white.
RGBA from_ass_colour(const std::string& ass);

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    Rect united(const Rect& o) const;
    Rect clipped(int W, int H) const;
};

// RGBA8 image, row-major, 4 bytes per pixel in R,G,B,A order.
struct Canvas {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    void resize(int width, int height) { w = width; h = height; px.assign((size_t)w * h * 4, 0); }
    void fill(RGBA c) { fill_rect({0, 0, w, h}, c); }
    void fill_rect(Rect r, RGBA c);
    uint8_t* row(int y) { return px.data() + (size_t)y * w * 4; }
    const uint8_t* row(int y) const { return px.data() + (size_t)y * w * 4; }
};

struct TextStyle {
    std::string font_file;          // ttf / otf / ttc
    int face_index = 0;             // face inside a .ttc collection
    double size = 36.0;             // libass/VSFilter convention: ascender - descender = size px
    std::vector<std::string> features;   // HarfBuzz feature strings: "kern", "-liga", "ss01", "onum", "tnum"
    double letter_spacing = 0.0;    // px added between characters (grapheme clusters), may be negative
    double line_spacing = 1.0;      // line height = size * line_spacing
    RGBA colour = 0xFFFFFFFFu;
    bool outline = false;           // pattern setting, default off
    double outline_width = 1.0;     // px at video resolution
    RGBA outline_colour = 0x000000FFu;
};

// The GUI's "Advanced typography" options (saved in patterns). apply() turns
// them into TextStyle features / spacing; defaults leave the font as designed.
struct Typography {
    bool kerning = true;            // "kern" (off -> "-kern")
    bool ligatures = true;          // standard ligatures "liga" (off -> "-liga")
    bool slashed_zero = false;      // "zero"
    int numerals = 0;               // 0 font default, 1 lining "lnum", 2 old-style "onum"
    int numeral_width = 0;          // 0 font default, 1 proportional "pnum", 2 tabular "tnum"
    uint32_t stylistic_sets = 0;    // bit n-1 set = "ssNN" on (ss01..ss20)
    std::string extra;              // more feature strings, space/comma separated: "cv01 salt -calt"
    double letter_spacing = 0.0;    // px
    double line_spacing = 1.0;      // multiplier

    std::vector<std::string> features() const;
    void apply(TextStyle& st) const;
    bool operator==(const Typography& o) const;
    bool operator!=(const Typography& o) const { return !(*this == o); }
};
// Tokens of a space/comma separated feature list.
std::vector<std::string> split_features(const std::string& list);
// True if HarfBuzz accepts the feature string ("kern", "-liga", "ss01=0", "aalt=2", "kern[3:5]").
bool valid_feature(const std::string& f);

// OpenType features a font offers (GSUB + GPOS, plus a legacy 'kern' table as
// "kern"), sorted by tag. `name` is the font's own label (stylistic sets /
// character variants), often empty.
struct FontFeature { std::string tag, name; };
std::vector<FontFeature> font_features(const std::string& font_file, int face_index = 0);

// Where the text block goes (same meaning as the GUI settings).
struct Placement {
    int frame_w = 1920, frame_h = 1080;
    int pos_mode = 0;               // 0 = corner/centre alignment, 1 = custom (top-left of the block)
    int alignment = 7;              // ASS numpad alignment: 7 8 9 / 1 2 3 (corners + top/bottom centre)
    int pos_x = 30, pos_y = 30;     // pos_mode 1
    int margin = 30;                // pos_mode 0 margin, as libass MarginL/R/V
};

struct ShapedGlyph { uint32_t id; double x, y; };   // pen position in px, relative to the line origin
struct ShapedLine {
    std::vector<ShapedGlyph> glyphs;
    double advance = 0;             // total advance width (px)
};
struct Layout {
    std::vector<ShapedLine> lines;
    double width = 0;               // max line advance
    double line_height = 0;         // line pitch = style size * line_spacing (libass spacing at 1.0)
    double em = 0;                  // one line's own height = style size
    double ascent = 0;              // baseline offset from a line's top
    // block height: no line spacing after the last line
    double height() const { return lines.empty() ? 0.0 : em + line_height * (double)(lines.size() - 1); }
};

class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    bool load(const TextStyle& style, std::string* err = nullptr);
    bool loaded() const;
    const TextStyle& style() const;

    Layout layout(const std::vector<std::string>& lines) const;
    // Top-left of the block for this layout (pos_mode 1 clamps the block on screen).
    void place(const Layout& L, const Placement& p, double& x0, double& y0) const;
    // Line i's horizontal offset inside the block (right/centre alignments).
    double line_offset(const Layout& L, size_t i, const Placement& p) const;
    // Draws the block at (x0, y0); returns the touched pixel rect (incl. outline).
    Rect draw(Canvas& c, const Layout& L, const Placement& p, double x0, double y0);
    // Conservative pixel bounds of a drawn block (for clearing before the next frame).
    Rect bounds(const Layout& L, const Placement& p, double x0, double y0) const;

    struct Impl;   // opaque (font data, HarfBuzz objects, glyph cache)
private:
    std::unique_ptr<Impl> d;
};

// Renders consecutive frames into one canvas: only the area touched by the
// previous and the current text is re-cleared and redrawn. `dirty()` is that
// area (for texture uploads and change detection).
class FrameRenderer {
public:
    bool init(const TextStyle& style, const Placement& placement, RGBA background,
              std::string* err = nullptr);
    const Canvas& render(const std::vector<std::string>& lines);
    const Canvas& canvas() const { return canvas_; }
    Rect dirty() const { return dirty_; }
    TextRenderer& text() { return text_; }

private:
    TextRenderer text_;
    Placement place_;
    RGBA bg_ = 0x000000FFu;
    Canvas canvas_;
    Rect last_{}, dirty_{};
};

} // namespace fmcg_text
