// test_layout -- text block placement and sizing in the in-built renderer:
//  * goal 9: custom position = frame anchor + offset, block anchor on it;
//    lines align to the block anchor's side,
//  * goal 7: fixed-width stat cells keep every line's width constant over a
//    whole video (and each stat fits its cell),
//  * goal 5: the block never leaves the frame: clamping in every mode, and a
//    block larger than the frame is scaled down once for the whole video.
// Needs a TrueType font ($FMCG_TEST_FONT or a common system font); SKIPs otherwise.
#include "src/fmcg_format.h"
#include "src/fmcg_text.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace fmcg_text;

static int fails = 0;
static void check(const std::string& what, bool ok) {
    std::printf("%-66s [%s]\n", what.c_str(), ok ? "OK" : "FAIL");
    if (!ok) ++fails;
}

static std::string find_font() {
    if (const char* e = std::getenv("FMCG_TEST_FONT")) return e;
    const char* cands[] = {"C:/Windows/Fonts/arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf"};
    for (const char* c : cands)
        if (FILE* f = std::fopen(c, "rb")) { std::fclose(f); return c; }
    return "";
}

// A synthetic song: counters that gain digits, polyphony/NPS/BPM that move
// around, and a start delay (negative time and ticks in the lead-in).
static std::vector<FrameStats> make_frames(size_t n, double fps) {
    std::vector<FrameStats> v;
    std::mt19937 rng(7);
    int64_t peak_poly = 0;
    double peak_nps = 0;
    for (size_t i = 0; i < n; ++i) {
        FrameStats f;
        f.frame_index = i;
        f.timestamp_sec = (double)i / fps;
        f.cumulative_notes = (uint64_t)i * i * i * 7;
        f.cumulative_cc = i * 3;
        f.polyphony = (int64_t)(rng() % 20000);
        f.notes_per_second = (double)(rng() % 3000000);
        peak_poly = std::max(peak_poly, f.polyphony);
        peak_nps = std::max(peak_nps, f.notes_per_second);
        f.peak_polyphony = peak_poly;
        f.peak_nps = peak_nps;
        f.bpm = 60.0 + (double)(rng() % 400000) / 100.0;
        f.tick = (int64_t)i * 37;
        v.push_back(f);
    }
    return v;
}

static FrameTextSource make_source(const std::vector<FrameStats>& frames, double fps) {
    FrameTextSource src;
    src.frames = &frames;
    src.template_lines = {"Notes {nc} / {nc-total}  Poly {plph}", "NPS {nps} max {nps-max}  BPM {bpm}",
                          "{time-milli} {tick} rem {sec-rem}s |"};
    src.total_notes = frames.back().cumulative_notes;
    src.total_cc = frames.back().cumulative_cc;
    src.total_ticks = (uint64_t)frames.back().tick;
    src.fps = fps;
    src.start_delay = 2.0;
    return src;
}

static bool align_right(int a) { return a == 3 || a == 6 || a == 9; }
static bool align_center(int a) { return a == 2 || a == 5 || a == 8; }
static bool align_bottom(int a) { return a == 1 || a == 2 || a == 3; }
static bool align_middle(int a) { return a == 4 || a == 5 || a == 6; }

int main() {
    const std::string font = find_font();
    if (font.empty()) { std::printf("SKIP: no TrueType font found\n"); return 0; }
    std::printf("font: %s\n", font.c_str());
    TextStyle st;
    st.font_file = font;
    st.size = 24;

    // ---- goal 9: anchors -------------------------------------------------------------
    {
        TextRenderer tr;
        check("load font", tr.load(st));
        const Layout L = tr.layout({"Wide first line", "Short"});
        const double w = L.width, h = L.height();
        bool pos_ok = true, lines_ok = true;
        for (int fa = 1; fa <= 9; ++fa)
            for (int ba = 1; ba <= 9; ++ba) {
                Placement p;
                p.frame_w = 640; p.frame_h = 360; p.pos_mode = 1;
                p.frame_anchor = fa; p.block_anchor = ba; p.pos_x = 200; p.pos_y = 120;
                const double X = align_right(fa) ? 640 - 200 : align_center(fa) ? 320 + 200 : 200;
                const double Y = align_bottom(fa) ? 360 - 120 : align_middle(fa) ? 180 + 120 : 120;
                double ex = X - (align_right(ba) ? w : align_center(ba) ? w / 2 : 0);
                double ey = Y - (align_bottom(ba) ? h : align_middle(ba) ? h / 2 : 0);
                ex = std::max(1.0, std::min(ex, 640 - w - 1));   // clamped (1 px edge) near the frame edges
                ey = std::max(1.0, std::min(ey, 360 - h - 1));
                double x0, y0;
                tr.place(L, p, x0, y0);
                if (std::fabs(x0 - ex) > 1e-9 || std::fabs(y0 - ey) > 1e-9) {
                    if (pos_ok) std::printf("  fa %d ba %d: got (%.1f, %.1f), want (%.1f, %.1f)\n", fa, ba, x0, y0, ex, ey);
                    pos_ok = false;
                }
                const double slack = w - L.lines[1].advance;
                const double want = align_right(ba) ? slack : align_center(ba) ? slack / 2 : 0;
                lines_ok &= std::fabs(tr.line_offset(L, 1, p) - want) < 1e-9;
            }
        check("anchors: block point placed at the frame point for all 9 x 9", pos_ok);
        check("anchors: lines align to the block anchor's side", lines_ok);
        Placement p;
        p.frame_w = 640; p.frame_h = 360; p.pos_mode = 1;
        p.frame_anchor = 3; p.block_anchor = 3; p.pos_x = 10; p.pos_y = 20;
        double x0, y0;
        tr.place(L, p, x0, y0);
        check("anchors: bottom-right 10,20 -> block ends 10 px from right, 20 from bottom",
              std::fabs(x0 + w - 630) < 1e-9 && std::fabs(y0 + h - 340) < 1e-9);
        p.frame_anchor = 7; p.block_anchor = 7; p.pos_x = 30; p.pos_y = 30;
        tr.place(L, p, x0, y0);
        check("anchors: defaults (top-left / top-left) keep the old top-left meaning", x0 == 30 && y0 == 30);
    }

    // ---- goal 7: fixed-width cells -------------------------------------------------------
    const double fps = 30;
    const std::vector<FrameStats> frames = make_frames(900, fps);
    const FrameTextSource src = make_source(frames, fps);
    const TextBlockSpec spec = src.block_spec();
    const size_t total = src.frame_count();
    for (int align = 0; align < 3; ++align) {
        Placement p;
        p.frame_w = 1920; p.frame_h = 1080; p.alignment = 7; p.cells = true; p.cell_align = align;
        FrameRenderer fr;
        fr.init(st, p, 0x000000FFu, spec);
        const Layout L0 = fr.layout_pieces(src.pieces(0));
        bool constant = true;
        for (size_t k = 0; k < total && constant; ++k) {
            const Layout L = fr.layout_pieces(src.pieces(k));
            for (size_t i = 0; i < L.lines.size(); ++i)
                if (std::fabs(L.lines[i].advance - L0.lines[i].advance) > 1e-9) {
                    std::printf("  frame %zu line %zu: %.3f vs %.3f\n", k, i, L.lines[i].advance, L0.lines[i].advance);
                    constant = false;
                }
        }
        const char* names[] = {"left", "centre", "right"};
        check(std::string("cells (") + names[align] + "): every line keeps its width over all " + std::to_string(total) + " frames", constant);
        if (align == 2) {   // the trailing literal "|" never moves
            bool fixed = true;
            const double x_end = L0.lines[2].glyphs.back().x;
            for (size_t k = 0; k < total; k += 7) fixed &= fr.layout_pieces(src.pieces(k)).lines[2].glyphs.back().x == x_end;
            check("cells: text after a stat stays put", fixed);
        }
    }
    {
        Placement p;
        p.frame_w = 1920; p.frame_h = 1080;
        FrameRenderer fr;
        fr.init(st, p, 0x000000FFu, spec);
        std::set<long long> widths;
        double maxw = 0;
        for (size_t k = 0; k < total; ++k) {
            const Layout L = fr.layout_pieces(src.pieces(k));
            widths.insert(std::llround(L.lines[0].advance * 100));
            maxw = std::max(maxw, L.width);
        }
        check("no cells: line widths do vary (the problem cells solve)", widths.size() > 3);
        check("no cells: the block-size bound covers every frame", maxw <= fr.block_width() + 1e-6);
    }

    // ---- goal 5: the block never leaves the frame -----------------------------------------
    {
        std::mt19937 rng(3);
        bool inside = true, scaled = true;
        int cases = 0;
        for (int big = 0; big < 2; ++big)
            for (int mode = 0; mode < 2; ++mode)
                for (int cells = 0; cells < 2; ++cells)
                    for (int rep = 0; rep < 6; ++rep) {
                        TextStyle s2 = st;
                        s2.size = big ? 160 : 12;
                        s2.outline = rep % 2;
                        s2.outline_width = 3;
                        Placement p;
                        p.frame_w = 480; p.frame_h = 270;
                        p.pos_mode = mode;
                        const int presets[] = {7, 9, 1, 3, 8, 2};
                        p.alignment = presets[rep];
                        p.frame_anchor = 1 + (int)(rng() % 9);
                        p.block_anchor = 1 + (int)(rng() % 9);
                        p.pos_x = (int)(rng() % 900) - 200;
                        p.pos_y = (int)(rng() % 600) - 150;
                        p.cells = cells;
                        FrameRenderer fr;
                        fr.init(s2, p, 0x000000FFu, spec);
                        if (big && !(fr.scale() < 1.0)) scaled = false;
                        if (!big && fr.scale() != 1.0) scaled = false;
                        for (size_t k = 0; k < total; k += 13) {
                            const Layout L = fr.layout_pieces(src.pieces(k));
                            double x0, y0;
                            fr.text().place(L, fr.placement(), x0, y0);
                            const Rect b = fr.text().bounds(L, fr.placement(), x0, y0);   // unclipped ink
                            if (b.x < 0 || b.y < 0 || b.x + b.w > p.frame_w || b.y + b.h > p.frame_h) {
                                if (inside) std::printf("  case big=%d mode=%d cells=%d rep=%d frame %zu: ink %d,%d %dx%d\n",
                                                        big, mode, cells, rep, k, b.x, b.y, b.w, b.h);
                                inside = false;
                            }
                        }
                        ++cases;
                    }
        check("fit: oversized text is scaled down, normal text is not", scaled);
        check("fit + clamp: ink stays inside the frame (" + std::to_string(cases) + " layouts, every 13th frame)", inside);
        TextStyle s2 = st;
        s2.size = 160;
        Placement p;
        p.frame_w = 480; p.frame_h = 270;
        FrameRenderer fr;
        fr.init(s2, p, 0x000000FFu, spec);
        std::printf("  160 px in 480x270 -> scale %.3f (%.1f px), block %.0f x %.0f\n", fr.scale(), fr.font_size(),
                    fr.block_width(), fr.block_height());
        check("fit: the largest block fits inside the margins", fr.block_width() <= 480 - 60 && fr.block_height() <= 270 - 60);
        p.fit = false;
        FrameRenderer nf;
        nf.init(s2, p, 0x000000FFu, spec);
        check("fit off: no scaling", nf.scale() == 1.0 && nf.font_size() == 160);
    }

    std::printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
