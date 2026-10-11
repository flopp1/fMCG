// test_text -- in-built text renderer (src/fmcg_text): layout, placement,
// incremental frame rendering and outline. Needs a TrueType font: $FMCG_TEST_FONT,
// else a common system font; prints SKIP and passes when none exists.
// With FMCG_TEXT_DUMP=<dir> it writes sample frames as .ppm for inspection.
#include "src/fmcg_text.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <random>
#include <string>
#include <vector>

using namespace fmcg_text;

static int fails = 0;
static void check(const char* what, bool ok) {
    std::printf("%-58s [%s]\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++fails;
}

static std::string find_font() {
    if (const char* e = std::getenv("FMCG_TEST_FONT")) return e;
    const char* cands[] = {"C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeui.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                           "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf"};
    for (const char* c : cands)
        if (FILE* f = std::fopen(c, "rb")) { std::fclose(f); return c; }
    return "";
}

static void dump(const Canvas& c, const std::string& name) {
    const char* dir = std::getenv("FMCG_TEXT_DUMP");
    if (!dir) return;
    FILE* f = std::fopen((std::string(dir) + "/" + name + ".ppm").c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", c.w, c.h);
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) std::fwrite(c.row(y) + x * 4, 1, 3, f);
    std::fclose(f);
}

// Bounding box of pixels that differ from the background colour.
static Rect ink(const Canvas& c, RGBA bg) {
    const uint8_t b[4] = {(uint8_t)(bg >> 24), (uint8_t)(bg >> 16), (uint8_t)(bg >> 8), (uint8_t)bg};
    int x1 = c.w, y1 = c.h, x2 = -1, y2 = -1;
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) {
            const uint8_t* p = c.row(y) + x * 4;
            if (p[0] != b[0] || p[1] != b[1] || p[2] != b[2] || p[3] != b[3]) {
                x1 = std::min(x1, x); y1 = std::min(y1, y); x2 = std::max(x2, x); y2 = std::max(y2, y);
            }
        }
    if (x2 < 0) return {};
    return {x1, y1, x2 - x1 + 1, y2 - y1 + 1};
}

int main() {
    const std::string font = find_font();
    if (font.empty()) { std::printf("SKIP: no TrueType font found (set FMCG_TEST_FONT)\n"); return 0; }
    std::printf("font: %s\n", font.c_str());

    TextStyle st;
    st.font_file = font;
    st.size = 36;
    Placement pl;
    pl.frame_w = 640; pl.frame_h = 360;
    const RGBA bg = 0x102030FFu;

    // 1. incremental rendering (only the dirty area is redrawn) == fresh full render
    {
        {
            FrameRenderer probe;
            std::string err;
            check("load font", probe.init(st, pl, bg, &err));
        }
        std::mt19937 rng(7);
        std::vector<std::string> lines;
        bool all_equal = true;
        for (int k = 0; k < 60; ++k) {
            lines = {"Notes: " + std::to_string(rng() % 100000000), "NPS " + std::to_string(rng() % 1000),
                     std::string(rng() % 3 == 0 ? "" : "Poly: 1,234,567")};
            pl.alignment = (k % 3 == 0) ? 3 : (k % 3 == 1) ? 8 : 7;
            FrameRenderer a;                 // one sequence of 5 frames per alignment
            a.init(st, pl, bg);
            for (int j = 0; j < 5; ++j) {
                lines[0] = "Notes: " + std::to_string(rng() % 1000000000);
                a.render(lines);
            }
            FrameRenderer fresh;
            fresh.init(st, pl, bg);
            fresh.render(lines);
            if (a.canvas().px != fresh.canvas().px) all_equal = false;
        }
        check("incremental frames == fresh render (60 sequences)", all_equal);
    }

    // 2. custom position: the block always stays fully on screen
    {
        bool inside = true;
        std::mt19937 rng(11);
        for (int k = 0; k < 40; ++k) {
            Placement p = pl;
            p.pos_mode = 1;
            p.pos_x = (int)(rng() % 700);
            p.pos_y = (int)(rng() % 400);
            FrameRenderer r;
            r.init(st, p, bg);
            r.render({"Notes: 93,823,560,428,205", "Poly 1"});
            const Rect in = ink(r.canvas(), bg);
            if (in.empty() || in.x < 0 || in.y < 0 || in.x + in.w > p.frame_w || in.y + in.h > p.frame_h) inside = false;
        }
        check("custom x,y: block clamped inside the frame", inside);
    }

    // 3. corner alignments put the ink near the right corner
    {
        Placement p = pl;
        p.alignment = 3;   // bottom right
        FrameRenderer r;
        r.init(st, p, bg);
        r.render({"123", "4567890"});
        const Rect in = ink(r.canvas(), bg);
        check("bottom-right: ink ends near width - margin", std::abs(in.x + in.w - (p.frame_w - p.margin)) <= 4);
        check("bottom-right: ink above height - margin", in.y + in.h <= p.frame_h - p.margin + 10);
        dump(r.canvas(), "bottom_right");
        p.alignment = 7;
        FrameRenderer t;
        t.init(st, p, bg);
        t.render({"Top left"});
        const Rect tl = ink(t.canvas(), bg);
        check("top-left: ink starts near the margin", std::abs(tl.x - p.margin) <= 4 && tl.y >= p.margin);
    }

    // 4. outline grows the ink box by about its width and is off by default
    {
        FrameRenderer plain, outl;
        plain.init(st, pl, bg);
        TextStyle so = st;
        so.outline = true;
        so.outline_width = 3;
        so.outline_colour = 0x000000FFu;
        outl.init(so, pl, bg);
        plain.render({"Outline"});
        outl.render({"Outline"});
        const Rect a = ink(plain.canvas(), bg), b = ink(outl.canvas(), bg);
        check("outline: ink box grows by ~2 x width", b.w >= a.w + 4 && b.w <= a.w + 8 && b.h >= a.h + 4);
        dump(outl.canvas(), "outline");
    }

    // 5. OpenType features are applied (disabling kerning changes some pair's advance)
    {
        TextRenderer k, nk;
        TextStyle s2 = st;
        k.load(s2);
        s2.features = {"-kern"};
        nk.load(s2);
        const double a = k.layout({"AVAVAV To Ty"}).width, b = nk.layout({"AVAVAV To Ty"}).width;
        std::printf("  kerned width %.2f, unkerned %.2f\n", a, b);
        check("features: '-kern' changes kerned text width (font has kerning)", a != b);
    }

    // 6. transparent background keeps alpha 0 outside the text
    {
        FrameRenderer r;
        r.init(st, pl, 0x00000000u);
        r.render({"alpha"});
        const Canvas& c = r.canvas();
        check("transparent bg: corner pixel alpha 0", c.row(0)[3] == 0 && c.row(c.h - 1)[(c.w - 1) * 4 + 3] == 0);
        bool opaque_text = false;
        for (size_t i = 3; i < c.px.size(); i += 4) if (c.px[i] == 255) { opaque_text = true; break; }
        check("transparent bg: glyph interiors opaque", opaque_text);
    }

    // 7. Typography options -> HarfBuzz feature strings (defaults change nothing)
    {
        fmcg_text::Typography t;
        check("typography: defaults produce no features", t.features().empty());
        t.kerning = false; t.ligatures = false; t.slashed_zero = true;
        t.numerals = 2; t.numeral_width = 2;
        t.stylistic_sets = (1u << 0) | (1u << 19);   // ss01, ss20
        t.extra = "cv01, salt  -calt !!bad";
        const std::vector<std::string> want = {"-kern", "-liga", "zero", "onum", "tnum", "ss01", "ss20",
                                               "cv01", "salt", "-calt"};
        check("typography: options map to features (invalid extra token dropped)", t.features() == want);
        check("typography: valid_feature accepts 'ss01=0' / rejects '!!bad'",
              fmcg_text::valid_feature("ss01=0") && !fmcg_text::valid_feature("!!bad"));
        TextStyle s2 = st;
        t.letter_spacing = 3; t.line_spacing = 1.5;
        t.apply(s2);
        check("typography: apply() sets features + spacing",
              s2.features == want && s2.letter_spacing == 3 && s2.line_spacing == 1.5);
    }

    // 8. font feature discovery
    {
        const auto ff = fmcg_text::font_features(st.font_file);
        std::string tags;
        bool kern = false;
        for (const auto& f : ff) { tags += f.tag + " "; kern |= f.tag == "kern"; }
        std::printf("  features of the test font: %s\n", tags.c_str());
        check("font_features: lists the test font's kerning", kern);
        check("font_features: missing file -> empty", fmcg_text::font_features("/no/such/font.ttf").empty());
    }

    // 9. letter spacing: + spacing between clusters (not after the last); line spacing: pitch only
    {
        TextRenderer a, b;
        TextStyle s2 = st;
        s2.features = {"-kern"};
        a.load(s2);
        s2.letter_spacing = 5;
        s2.line_spacing = 1.5;
        b.load(s2);
        const Layout la = a.layout({"Hello", "x"}), lb = b.layout({"Hello", "x"});
        std::printf("  'Hello' width %.2f -> %.2f with 5 px spacing\n", la.lines[0].advance, lb.lines[0].advance);
        check("letter spacing: width grows by 4 x 5 px for 5 characters",
              std::fabs(lb.lines[0].advance - la.lines[0].advance - 20.0) < 1e-6);
        check("line spacing: block height = size + (lines-1) x size x 1.5",
              std::fabs(lb.height() - (st.size + st.size * 1.5)) < 1e-6 && std::fabs(la.height() - 2 * st.size) < 1e-6);
    }

    // 10. old-style numerals change the digit glyphs, in a font that has them (skipped otherwise)
    {
        const char* cands[] = {"C:/Windows/Fonts/calibri.ttf", "C:/Windows/Fonts/georgia.ttf", "C:/Windows/Fonts/constan.ttf",
                               "/usr/share/fonts/truetype/crosextra/Carlito-Regular.ttf",
                               "/usr/share/fonts/opentype/linux-libertine/LinLibertine_R.otf"};
        std::string onum_font;
        for (const char* c : cands) {
            for (const auto& f : fmcg_text::font_features(c))
                if (f.tag == "onum") onum_font = c;
            if (!onum_font.empty()) break;
        }
        if (onum_font.empty()) {
            std::printf("  (no font with 'onum' found; old-style numeral check skipped)\n");
        } else {
            TextRenderer a, b;
            TextStyle s2 = st;
            s2.font_file = onum_font;
            a.load(s2);
            s2.features = {"onum"};
            b.load(s2);
            s2.features = {"lnum"};
            TextRenderer c;
            c.load(s2);
            const Layout la = a.layout({"0123456789"}), lb = b.layout({"0123456789"}), lc = c.layout({"0123456789"});
            bool differs = false, lnum_differs = false;
            for (size_t i = 0; i < la.lines[0].glyphs.size() && i < lb.lines[0].glyphs.size(); ++i) {
                differs |= la.lines[0].glyphs[i].id != lb.lines[0].glyphs[i].id;
                lnum_differs |= lb.lines[0].glyphs[i].id != lc.lines[0].glyphs[i].id;
            }
            std::printf("  onum font: %s\n", onum_font.c_str());
            check("onum: old-style numerals use different digit glyphs than lnum", lnum_differs);
            std::printf("  font default digits %s old-style\n", differs ? "are not" : "are already");
        }
    }

    std::printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
