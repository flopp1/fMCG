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

// Where the text block goes and how its lines are laid out (same meaning as
// the GUI settings). Anchors use the numpad convention: 7 8 9 = top
// left/centre/right, 4 5 6 = middle, 1 2 3 = bottom.
struct Placement {
    int frame_w = 1920, frame_h = 1080;
    int pos_mode = 0;               // 0 = corner preset (alignment + margin), 1 = custom position
    int alignment = 7;              // pos_mode 0: ASS numpad alignment (corners + top/bottom centre)
    int margin = 30;                // pos_mode 0 margin, as libass MarginL/R/V
    // pos_mode 1: (pos_x, pos_y) is measured from `frame_anchor` -- inward from
    // an edge (left/top: +x right/+y down; right/bottom: +x left/+y up), or
    // from the centre line (+x right/+y down) -- and the block's `block_anchor`
    // point is put there. Lines align to the block anchor's side.
    int pos_x = 30, pos_y = 30;
    int frame_anchor = 7;
    int block_anchor = 7;
    // Fixed-width stat cells: each stat takes the width of its widest value in
    // the video (TextBlockSpec), aligned inside it -- lines stop shifting.
    bool cells = false;
    int cell_align = 2;             // 0 left, 1 centre, 2 right
    // Scale the font down (once, for the whole video) if the largest block the
    // video shows would not fit the frame (minus margins). Needs a TextBlockSpec.
    bool fit = true;
};

// One run of a line: literal text (cell -1) or a stat value (cell = index of
// the stat on its line).
struct TextPiece {
    std::string text; int cell = -1;
    bool operator==(const TextPiece& o) const { return cell == o.cell && text == o.text; }
};
using PieceLines = std::vector<std::vector<TextPiece>>;
// What a whole video will show: the template's pieces (stat text = any value)
// and, per line and cell, candidate strings whose widest rendering bounds
// every value that stat takes (FrameTextSource::block_spec()).
struct TextBlockSpec {
    PieceLines pieces;
    std::vector<std::vector<std::vector<std::string>>> extremes;
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
    // Pieces shaped one by one; a piece with cell c >= 0 occupies
    // cell_w[line][c] (if given and wider than the text), aligned inside it.
    Layout layout(const PieceLines& lines, const std::vector<std::vector<double>>& cell_w, int cell_align) const;
    double advance(const std::string& text) const;   // shaped width of one line (px)
    // Top-left of the block for this layout, clamped so the block (incl. outline)
    // stays on screen whenever it fits.
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
    // With the video's TextBlockSpec: fixed-width cells (placement.cells) and
    // fitting the largest block into the frame (placement.fit).
    bool init(const TextStyle& style, const Placement& placement, RGBA background,
              const TextBlockSpec& spec, std::string* err = nullptr);
    // Text identical to the previous call's: nothing is shaped or drawn and
    // dirty() is empty (encoders then store the frame as unchanged).
    const Canvas& render(const std::vector<std::string>& lines);
    const Canvas& render_pieces(const PieceLines& pieces);
    Layout layout_pieces(const PieceLines& pieces) const;   // the layout render_pieces draws
    const Placement& placement() const { return place_; }
    const Canvas& canvas() const { return canvas_; }
    Rect dirty() const { return dirty_; }
    TextRenderer& text() { return text_; }
    double scale() const { return scale_; }          // font scale applied by fitting (1 = none)
    double font_size() const { return text_.loaded() ? text_.style().size : 0.0; }
    double block_width() const { return block_w_; }  // largest block (px, after fitting), 0 without spec
    double block_height() const { return block_h_; }

private:
    const Canvas& draw(const Layout& L);
    TextRenderer text_;
    Placement place_;
    RGBA bg_ = 0x000000FFu;
    Canvas canvas_;
    Rect last_{}, dirty_{};
    std::vector<std::vector<double>> cell_w_;
    double scale_ = 1.0, block_w_ = 0, block_h_ = 0;
    bool have_prev_ = false;               // prev_pieces_ is what the canvas shows
    PieceLines prev_pieces_;
    // Per line: the pieces last shaped there and the result. Usually only one
    // or two lines change between frames (time, notes), so the rest are reused.
    mutable std::vector<std::pair<std::vector<TextPiece>, ShapedLine>> line_cache_;
};

} // namespace fmcg_text
