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
    RGBA colour = 0xFFFFFFFFu;
    bool outline = false;           // pattern setting, default off
    double outline_width = 1.0;     // px at video resolution
    RGBA outline_colour = 0x000000FFu;
};

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
    double line_height = 0;         // = style size (libass line spacing)
    double ascent = 0;              // baseline offset from a line's top
    double height() const { return line_height * (double)lines.size(); }
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
