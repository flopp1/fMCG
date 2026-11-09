// test_layout -- text block placement and sizing in the in-built renderer:
//  * goal 9: custom position = frame anchor + offset, block anchor on it;
//    lines align to the block anchor's side,
//  * goal 7: fixed-width stat cells keep every line's width constant over a
//    whole video (and each stat fits its cell),
//  * goal 5: the block never leaves the frame: clamping in every mode, and a
//    block larger than the frame is scaled down once for the whole video.
//  * position headers ([top-left], [center], ...): lines go to separate
//    blocks; each block is drawn exactly where a one-block render of it would
//    be; incremental (dirty-rect) rendering equals a fresh render of every
//    frame, overlapping blocks included; one fit scale for all blocks.
//  * line alignment ([top-left, center] / Placement::line_align): lines align
//    inside a block that keeps its widest width, so a centred label stays put
//    and centred over its stat on every frame.
// Needs a TrueType font ($FMCG_TEST_FONT or a common system font); SKIPs otherwise.
#include "src/fmcg_format.h"
#include "src/fmcg_text.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    src.song.notes = frames.back().cumulative_notes;
    src.song.cc = frames.back().cumulative_cc;
    src.song.ticks = (uint64_t)frames.back().tick;
    src.fps = fps;
    src.start_delay = 2.0;
    return src;
}

// Ink extent (min / max x of non-background pixels) in rows [y0, y1); -1 if none.
static void ink_x(const Canvas& c, RGBA bg, int y0, int y1, int& lo, int& hi) {
    const uint8_t b[4] = {(uint8_t)(bg >> 24), (uint8_t)(bg >> 16), (uint8_t)(bg >> 8), (uint8_t)bg};
    lo = hi = -1;
    for (int y = std::max(0, y0); y < std::min(c.h, y1); ++y)
        for (int x = 0; x < c.w; ++x)
            if (std::memcmp(c.row(y) + x * 4, b, 4) != 0) {
                if (lo < 0 || x < lo) lo = x;
                if (x > hi) hi = x;
            }
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

    // ---- position headers ------------------------------------------------------------
    {
        bool parse = position_header("[top-left]") == 7 && position_header("  [Bottom Right] ") == 3 &&
                     position_header("[centre]") == 5 && position_header("[middle_left]") == 4 &&
                     position_header("[top]") == 8 && position_header("[bottom]") == 2 &&
                     position_header("[right]") == 6 && position_header("[default]") == 0 &&
                     position_header("[TOP-CENTER]") == 8 && position_header("[center-right]") == 6 &&
                     position_header("[topleft]") == -1 && position_header("[nc]") == -1 &&
                     position_header("Notes [top-left]") == -1 && position_header("{nc}") == -1 &&
                     position_header("[]") == -1;
        for (int i = 0; i < 9; ++i) parse &= position_header(std::string("[") + k_position_names[i] + "]") == k_position_anchors[i];
        check("headers: names, aliases, case, spacing; non-headers stay text", parse);

        const std::vector<FrameStats> frames = make_frames(300, 30.0);
        FrameTextSource src = make_source(frames, 30.0);
        src.template_lines = {"Time {time-milli}", "[bottom-right]", "Notes {nc}", "NPS {nps}", "[center]",
                              "Poly {plph}", "[top-right]", "BPM {bpm}", "[default]", "Tick {tick}"};
        const std::vector<int> want_a = {0, 3, 3, 5, 9, 0};
        check("headers are not shown; each line gets its header's position",
              src.line_anchors() == want_a && src.pieces(0).size() == 6 && src.lines(40).size() == 6 &&
              src.block_spec().anchors == want_a);

        Placement p;
        p.frame_w = 640; p.frame_h = 360;
        p.alignment = 7;                 // pattern: top-left
        const RGBA bg = 0x102030FFu;
        const TextBlockSpec spec = src.block_spec();
        FrameRenderer fr;
        fr.init(st, p, bg, spec);
        check("blocks: top-left (default + [default]), bottom-right, center, top-right", fr.block_count() == 4);

        // Each block where a one-block render of just its lines puts it.
        bool same = true;
        for (size_t k : {(size_t)0, (size_t)60, (size_t)200}) {
            const PieceLines all = src.pieces(k);
            const Canvas& c = fr.render_pieces(all);
            Canvas expect;
            expect.resize(p.frame_w, p.frame_h);
            expect.fill(bg);
            for (int a : {7, 3, 5, 9}) {
                PieceLines mine;
                for (size_t li = 0; li < all.size(); ++li)
                    if ((want_a[li] == 0 ? 7 : want_a[li]) == a) mine.push_back(all[li]);
                Placement q = p;
                q.alignment = a;
                FrameRenderer one;
                one.init(st, q, bg);
                const Canvas& oc = one.render_pieces(mine);
                const uint8_t bgb[4] = {(uint8_t)(bg >> 24), (uint8_t)(bg >> 16), (uint8_t)(bg >> 8), (uint8_t)bg};
                for (size_t i = 0; i < oc.px.size(); i += 4)   // composite: the block's non-background pixels
                    if (std::memcmp(&oc.px[i], bgb, 4) != 0) std::memcpy(&expect.px[i], &oc.px[i], 4);
            }
            same &= c.px == expect.px;
        }
        check("each block is drawn where a one-block render of its lines is", same);

        // Middle positions in preset mode: vertically centred.
        {
            TextRenderer tr;
            tr.load(st);
            const Layout L = tr.layout({"Centre block", "two lines"});
            bool mid = true;
            for (int a : {4, 5, 6}) {
                Placement q = p;
                q.alignment = a;
                double x0, y0;
                tr.place(L, q, x0, y0);
                mid &= std::fabs(y0 + L.height() / 2.0 - p.frame_h / 2.0) < 1e-6;
                if (a == 4) mid &= std::fabs(x0 - p.margin) < 1e-6;
                if (a == 5) mid &= std::fabs(x0 + L.width / 2.0 - p.frame_w / 2.0) < 1e-6;
                if (a == 6) mid &= std::fabs(x0 + L.width - (p.frame_w - p.margin)) < 1e-6;
            }
            check("preset positions 4/5/6: vertically centred, left/centre/right at the margin", mid);
        }

        // Incremental rendering == a fresh render, every frame, with blocks
        // that overlap (a long top-left line runs into top-center / top-right).
        FrameTextSource ov = make_source(frames, 30.0);
        ov.template_lines = {"Notes {nc} / {nc-total} and a long tail of text here", "[top-center]",
                             "NPS {nps}", "[top-right]", "{time-milli} {bpm}", "[center]", "Poly {plph}",
                             "[center-left]", "Tick {tick} {tick} {tick}"};
        for (int cells = 0; cells < 2; ++cells) {
            Placement q = p;
            q.cells = cells == 1;
            FrameRenderer inc;
            inc.init(st, q, bg, ov.block_spec());
            bool eq = true, dirty_ok = true;
            Canvas prev;
            for (size_t k = 0; k < ov.frame_count() && eq; ++k) {
                const Canvas& c = inc.render_pieces(ov.pieces(k));
                FrameRenderer fresh;
                fresh.init(st, q, bg, ov.block_spec());
                eq = fresh.render_pieces(ov.pieces(k)).px == c.px;
                if (!eq) std::printf("  first mismatch at frame %zu\n", k);
                if (k > 0) {   // nothing outside dirty() changed
                    const Rect d = inc.dirty();
                    for (int y = 0; y < c.h && dirty_ok; ++y)
                        for (int x = 0; x < c.w; ++x) {
                            if (x >= d.x && x < d.x + d.w && y >= d.y && y < d.y + d.h) continue;
                            if (std::memcmp(c.row(y) + x * 4, prev.row(y) + x * 4, 4) != 0) { dirty_ok = false; break; }
                        }
                }
                prev = c;
            }
            check(std::string("overlapping blocks: incremental == fresh render, every frame") + (cells ? " (cells)" : ""), eq);
            check(std::string("overlapping blocks: nothing changes outside dirty()") + (cells ? " (cells)" : ""), dirty_ok);
        }

        // A block whose text never changes is not redrawn (dirty stays away from it).
        {
            FrameTextSource fx = make_source(frames, 30.0);
            fx.template_lines = {"Notes {nc}", "[bottom-right]", "fixed label"};
            FrameRenderer r;
            r.init(st, p, bg, fx.block_spec());
            r.render_pieces(fx.pieces(0));
            r.render_pieces(fx.pieces(100));
            const Rect d = r.dirty();
            check("an unchanged block is left alone (dirty only around the changing one)",
                  !d.empty() && d.y + d.h < p.frame_h / 2);
        }

        // Pattern placement equal to a header's position: one block.
        {
            Placement q = p;
            q.alignment = 3;
            FrameRenderer r;
            r.init(st, q, bg, src.block_spec());
            check("pattern at bottom-right + [bottom-right] lines: one block, not two", r.block_count() == 3);
        }

        // Fit: one scale so that every block fits.
        {
            TextStyle big = st;
            big.size = 90;
            FrameTextSource fx = make_source(frames, 30.0);
            fx.template_lines = {"Short", "[bottom-right]", "A much longer line of text {nc} {nc-total} {plph}"};
            Placement q;
            q.frame_w = 480; q.frame_h = 270;
            FrameRenderer r;
            r.init(big, q, bg, fx.block_spec());
            check("fit: the widest block (not the first) sets the scale", r.scale() < 1.0 && r.block_width() <= 480 - 60);
        }
    }

    // ---- line alignment --------------------------------------------------------------
    {
        int la = -1;
        bool parse = position_header("[top-left, center]", la) == 7 && la == 2;
        parse &= position_header(" [Bottom Right , Right] ", la) == 3 && la == 3;
        parse &= position_header("[top-left,centre]", la) == 7 && la == 2;
        parse &= position_header("[default, left]", la) == 0 && la == 1;
        parse &= position_header("[center]", la) == 5 && la == 0;
        parse &= position_header("[top-left, up]", la) == -1 && position_header("[top-left,]", la) == -1 &&
                 position_header("[, center]", la) == -1 && position_header("[top-left, center, right]", la) == -1;
        check("line alignment after a comma: left / center / right, else not a header", parse);

        const std::vector<FrameStats> frames = make_frames(300, 30.0);
        FrameTextSource src = make_source(frames, 30.0);
        src.template_lines = {"A {nc}", "[top-right, left]", "B", "[bottom-left]", "C", "[default]", "D"};
        check("line_aligns: per line from its header, reset by the next header",
              src.line_aligns() == std::vector<int>({0, 1, 0, 0}) &&
              src.block_spec().line_aligns == std::vector<int>({0, 1, 0, 0}));

        Placement p;
        p.frame_w = 640; p.frame_h = 360;
        p.alignment = 7;
        const RGBA bg = 0x102030FFu;
        const double em = st.size;
        const int l0 = p.margin, l1 = p.margin + (int)em, l2 = p.margin + 2 * (int)em;
        // A label centred over its stat, top-left block: stable over the video
        // and centred on the stat's cell (cells centred) on every frame.
        for (int cells = 0; cells < 2; ++cells) {
            FrameTextSource ls = make_source(frames, 30.0);
            ls.template_lines = {"[top-left, center]", "NOTES", "{nc}"};
            Placement q = p;
            q.cells = cells == 1;
            q.cell_align = 1;
            FrameRenderer r;
            r.init(st, q, bg, ls.block_spec());
            int lo0 = -1, hi0 = -1;
            bool stable = true, centred = true;
            for (size_t k = 0; k < ls.frame_count(); k += 7) {
                const Canvas& c = r.render_pieces(ls.pieces(k));
                int lo, hi, nlo, nhi;
                ink_x(c, bg, l0, l1, lo, hi);
                ink_x(c, bg, l1, l2, nlo, nhi);
                if (lo0 < 0) { lo0 = lo; hi0 = hi; }
                stable &= lo == lo0 && hi == hi0;
                if (cells) centred &= std::fabs((lo + hi) / 2.0 - (nlo + nhi) / 2.0) <= 2.0;
                stable &= nlo >= p.margin - 2;   // the block still hugs the left margin
            }
            check(std::string("[top-left, center]: label stays put over the video") + (cells ? " (cells)" : ""),
                  stable && lo0 > p.margin + 5);
            if (cells) check("[top-left, center] + centred cells: label centred over its stat", centred);
        }
        // Right-anchored block: the widest line still ends at the margin.
        {
            FrameTextSource ls = make_source(frames, 30.0);
            ls.template_lines = {"[top-right, center]", "POLYPHONY", "{plph}"};
            Placement q = p;
            q.cells = true;
            FrameRenderer r;
            r.init(st, q, bg, ls.block_spec());
            int lo0 = -1, hi0 = -1;
            bool stable = true;
            for (size_t k = 0; k < ls.frame_count(); k += 7) {
                const Canvas& c = r.render_pieces(ls.pieces(k));
                int lo, hi;
                ink_x(c, bg, l0, l1, lo, hi);
                if (lo0 < 0) { lo0 = lo; hi0 = hi; }
                stable &= lo == lo0 && hi == hi0;
            }
            check("[top-right, center]: label stays put, block at the right margin",
                  stable && hi0 <= p.frame_w - p.margin && hi0 > p.frame_w / 2);
        }
        // The pattern-wide setting == the same alignment in a header.
        {
            FrameTextSource a = make_source(frames, 30.0), b = make_source(frames, 30.0);
            a.template_lines = {"[top-left, center]", "NOTES", "{nc}", "[bottom-right, left]", "Poly", "{plph}"};
            b.template_lines = {"NOTES", "{nc}", "[bottom-right, left]", "Poly", "{plph}"};
            Placement qa = p, qb = p;
            qb.line_align = 2;
            FrameRenderer ra, rb;
            ra.init(st, qa, bg, a.block_spec());
            rb.init(st, qb, bg, b.block_spec());
            bool same = true;
            for (size_t k : {(size_t)0, (size_t)90, (size_t)299})
                same &= ra.render_pieces(a.pieces(k)).px == rb.render_pieces(b.pieces(k)).px;
            check("Placement::line_align == the same alignment given by a header", same);
        }
        // Incremental == fresh with aligned, overlapping blocks.
        {
            FrameTextSource ov = make_source(frames, 30.0);
            ov.template_lines = {"[top-left, right]", "Notes {nc} / {nc-total} and a long tail of text", "x",
                                 "[top-center, left]", "NPS {nps}", "[top-right, center]", "{time-milli}", "{bpm}"};
            for (int cells = 0; cells < 2; ++cells) {
                Placement q = p;
                q.cells = cells == 1;
                FrameRenderer inc;
                inc.init(st, q, bg, ov.block_spec());
                bool eq = true;
                for (size_t k = 0; k < ov.frame_count() && eq; ++k) {
                    const Canvas& c = inc.render_pieces(ov.pieces(k));
                    FrameRenderer fresh;
                    fresh.init(st, q, bg, ov.block_spec());
                    eq = fresh.render_pieces(ov.pieces(k)).px == c.px;
                    if (!eq) std::printf("  first mismatch at frame %zu\n", k);
                }
                check(std::string("aligned blocks: incremental == fresh render, every frame") + (cells ? " (cells)" : ""), eq);
            }
        }
    }

    std::printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
