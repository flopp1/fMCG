// test_layout -- text block placement and sizing in the in-built renderer:
//  * boxes: the block sits at the anchor point of its box inset by the
//    margin, lines align to the anchor's side (or line_align), clamped into
//    the box; the whole-frame box is the old corner preset,
//  * goal 7: fixed-width stat cells keep every line's width constant over a
//    whole video (and each stat fits its cell),
//  * goal 5: the block never leaves the frame (or its box): clamping, and a
//    block larger than its box is scaled down once for the whole video,
//  * several boxes: each block is drawn exactly where a one-block render of
//    it would be; incremental (dirty-rect) rendering equals a fresh render of
//    every frame, overlapping blocks included; one fit scale for all blocks;
//    a label centred over its stat in each third of the frame.
// Needs a TrueType font ($FMCG_TEST_FONT or a common system font); SKIPs otherwise.
#include "src/fmcg_format.h"
#include "src/fmcg_text.h"
#include "src/fmcg_boxes.h"

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

    // ---- boxes: anchors -------------------------------------------------------------
    {
        TextRenderer tr;
        check("load font", tr.load(st));
        const Layout L = tr.layout({"Wide first line", "Short"});
        const double w = L.width, h = L.height();
        bool pos_ok = true, lines_ok = true, clamp_ok = true;
        for (int a = 1; a <= 9; ++a) {
            Placement p;
            p.frame_w = 640; p.frame_h = 360;
            p.box_x = 100; p.box_y = 40; p.box_w = 400; p.box_h = 250;
            p.margin = 12;
            p.alignment = a;
            const double ex = align_right(a) ? 500 - 12 - w : align_center(a) ? 300 - w / 2 : 112;
            const double ey = align_bottom(a) ? 290 - 12 - h : align_middle(a) ? 165 - h / 2 : 52;
            double x0, y0;
            tr.place(L, p, x0, y0);
            if (std::fabs(x0 - ex) > 1e-9 || std::fabs(y0 - ey) > 1e-9) {
                if (pos_ok) std::printf("  anchor %d: got (%.1f, %.1f), want (%.1f, %.1f)\n", a, x0, y0, ex, ey);
                pos_ok = false;
            }
            const double slack = w - L.lines[1].advance;
            const double want = align_right(a) ? slack : align_center(a) ? slack / 2 : 0;
            lines_ok &= std::fabs(tr.line_offset(L, 1, p) - want) < 1e-9;
            // A margin larger than the box: the block still stays inside it.
            p.margin = 200;
            tr.place(L, p, x0, y0);
            clamp_ok &= x0 >= 100 && x0 + w <= 500 && y0 >= 40 && y0 + h <= 290;
        }
        check("boxes: block at the anchor point of the inset box, all 9 anchors", pos_ok);
        check("boxes: lines align to the anchor's side", lines_ok);
        check("boxes: block kept inside its box", clamp_ok);
        bool la_ok = true;
        for (int la = 1; la <= 3; ++la) {
            Placement p;
            p.alignment = 9;
            p.line_align = la;
            const double slack = w - L.lines[1].advance;
            la_ok &= std::fabs(tr.line_offset(L, 1, p) - (la == 3 ? slack : la == 2 ? slack / 2 : 0)) < 1e-9;
        }
        check("boxes: line_align overrides the anchor's side", la_ok);
        // The whole-frame box (no box set) is the old corner preset at the margin.
        Placement p;
        p.frame_w = 640; p.frame_h = 360;
        p.alignment = 3;
        double x0, y0;
        tr.place(L, p, x0, y0);
        check("whole frame, bottom-right: block ends at the 30 px margin",
              std::fabs(x0 + w - 610) < 1e-9 && std::fabs(y0 + h - 330) < 1e-9);
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
        bool in_box = true;
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
                        const int presets[] = {7, 9, 1, 3, 8, 2};
                        p.alignment = presets[rep];
                        p.cells = cells;
                        // mode 1: a random box (fractions), random anchor, random margin
                        BoxPlace bp;
                        if (mode == 1) {
                            bp.x0 = (rng() % 50) / 100.0; bp.x1 = bp.x0 + 0.1 + (rng() % 40) / 100.0;
                            bp.y0 = (rng() % 50) / 100.0; bp.y1 = bp.y0 + 0.1 + (rng() % 40) / 100.0;
                            bp.anchor = 1 + (int)(rng() % 9);
                            p.boxes = {bp};
                            p.margin = (int)(rng() % 20);
                        }
                        FrameRenderer fr;
                        fr.init(s2, p, 0x000000FFu, spec);
                        if (big && !(fr.scale() < 1.0)) scaled = false;
                        if (!big && mode == 0 && fr.scale() != 1.0) scaled = false;
                        // The block's own placement: the box in px (mode 1).
                        const double bx0 = mode ? std::round(bp.x0 * 480) : 0, bx1 = mode ? std::round(bp.x1 * 480) : 480;
                        const double by0 = mode ? std::round(bp.y0 * 270) : 0, by1 = mode ? std::round(bp.y1 * 270) : 270;
                        for (size_t k = 0; k < total; k += 13) {
                            const PieceLines pl = src.pieces(k);
                            const Canvas& c = fr.render_pieces(pl);
                            int lo = c.w, hi = -1, top = c.h, bot = -1;
                            for (int y = 0; y < c.h; ++y)
                                for (int x = 0; x < c.w; ++x)
                                    if (c.row(y)[x * 4] | c.row(y)[x * 4 + 1] | c.row(y)[x * 4 + 2]) {
                                        lo = std::min(lo, x); hi = std::max(hi, x);
                                        top = std::min(top, y); bot = std::max(bot, y);
                                    }
                            if (hi < 0) continue;
                            if (lo < 0 || top < 0 || hi >= p.frame_w || bot >= p.frame_h) {
                                if (inside) std::printf("  case big=%d mode=%d cells=%d rep=%d frame %zu: ink %d..%d, %d..%d\n",
                                                        big, mode, cells, rep, k, lo, hi, top, bot);
                                inside = false;
                            }
                            // Inside its box, give or take the outline and glyph overhang.
                            const double slop = 2 + (s2.outline ? 4 : 0);
                            if (lo < bx0 - slop || hi > bx1 + slop || top < by0 - slop || bot > by1 + slop) {
                                if (in_box) std::printf("  case big=%d mode=%d cells=%d rep=%d frame %zu: ink %d..%d, %d..%d, box %.0f..%.0f, %.0f..%.0f\n",
                                                        big, mode, cells, rep, k, lo, hi, top, bot, bx0, bx1, by0, by1);
                                in_box = false;
                            }
                        }
                        ++cases;
                    }
        check("fit: oversized text is scaled down, normal text is not", scaled);
        check("fit + clamp: ink stays inside the frame (" + std::to_string(cases) + " layouts, every 13th frame)", inside);
        check("fit + clamp: ink stays inside its box", in_box);
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

    // ---- several boxes ----------------------------------------------------------------
    {
        const std::vector<FrameStats> frames = make_frames(300, 30.0);
        const RGBA bg = 0x102030FFu;
        TextStyle s16 = st;
        s16.size = 16;
        // top: three thirds; bottom: two halves
        fmcg_boxes::Node root;
        fmcg_boxes::split_leaf(root, 0, fmcg_boxes::kRows, 2);
        fmcg_boxes::split_leaf(root, 0, fmcg_boxes::kColumns, 3);
        fmcg_boxes::split_leaf(root, 3, fmcg_boxes::kColumns, 2);
        const int anchors[5] = {7, 8, 9, 1, 5};
        for (int i = 0; i < 5; ++i) fmcg_boxes::leaf_at(root, i)->box.anchor = anchors[i];
        Placement p;
        p.frame_w = 640; p.frame_h = 360;
        p.margin = 6;
        p.boxes = fmcg_boxes::place_list(root);
        check("boxes: five boxes in reading order", p.boxes.size() == 5 && p.boxes[1].x0 > 0.33 && p.boxes[3].y0 == 0.5);
        FrameTextSource src = make_source(frames, 30.0);
        src.template_lines = {"Time {time-milli}", "Notes {nc}", "NPS {nps}", "Poly {plph}", "BPM {bpm}", "Tick {tick}", "x"};
        src.template_boxes = {0, 1, 1, 2, 3, 4, 0};
        const TextBlockSpec spec = src.block_spec();
        check("block_spec carries each line's box", spec.boxes == src.template_boxes);
        FrameRenderer fr;
        fr.init(s16, p, bg, spec);
        check("one block per box with lines", fr.block_count() == 5 && fr.scale() == 1.0);

        // Each block is where a one-block render in its box puts it.
        bool same = true;
        for (size_t k : {(size_t)0, (size_t)60, (size_t)200}) {
            const PieceLines all = src.pieces(k);
            const Canvas& c = fr.render_pieces(all);
            Canvas expect;
            expect.resize(p.frame_w, p.frame_h);
            expect.fill(bg);
            for (int b = 0; b < 5; ++b) {
                PieceLines mine;
                for (size_t li = 0; li < all.size(); ++li)
                    if (src.template_boxes[li] == b) mine.push_back(all[li]);
                Placement q = p;
                q.boxes.clear();
                q.box_x = std::round(p.boxes[b].x0 * 640); q.box_w = std::round(p.boxes[b].x1 * 640) - q.box_x;
                q.box_y = std::round(p.boxes[b].y0 * 360); q.box_h = std::round(p.boxes[b].y1 * 360) - q.box_y;
                q.alignment = anchors[b];
                FrameRenderer one;
                one.init(s16, q, bg);
                const Canvas& oc = one.render_pieces(mine);
                const uint8_t bgb[4] = {(uint8_t)(bg >> 24), (uint8_t)(bg >> 16), (uint8_t)(bg >> 8), (uint8_t)bg};
                for (size_t i = 0; i < oc.px.size(); i += 4)
                    if (std::memcmp(&oc.px[i], bgb, 4) != 0) std::memcpy(&expect.px[i], &oc.px[i], 4);
            }
            same &= c.px == expect.px;
        }
        check("each block is drawn where a one-block render in its box is", same);

        // Incremental == fresh, every frame; with fit off a long line runs
        // out of its box into the neighbours (overlapping blocks).
        FrameTextSource ov = src;
        ov.template_lines[0] = "Notes {nc} / {nc-total} and a long tail of text running on";
        for (int variant = 0; variant < 4; ++variant) {
            Placement q = p;
            q.cells = variant & 1;
            q.fit = !(variant & 2);
            FrameRenderer inc;
            inc.init(s16, q, bg, ov.block_spec());
            bool eq = true, dirty_ok = true;
            Canvas prev;
            for (size_t k = 0; k < ov.frame_count() && eq; ++k) {
                const Canvas& c = inc.render_pieces(ov.pieces(k));
                FrameRenderer fresh;
                fresh.init(s16, q, bg, ov.block_spec());
                eq = fresh.render_pieces(ov.pieces(k)).px == c.px;
                if (!eq) std::printf("  first mismatch at frame %zu\n", k);
                if (k > 0) {
                    const Rect d = inc.dirty();
                    for (int y = 0; y < c.h && dirty_ok; ++y)
                        for (int x = 0; x < c.w; ++x) {
                            if (x >= d.x && x < d.x + d.w && y >= d.y && y < d.y + d.h) continue;
                            if (std::memcmp(c.row(y) + x * 4, prev.row(y) + x * 4, 4) != 0) { dirty_ok = false; break; }
                        }
                }
                prev = c;
            }
            const std::string tag = std::string(q.cells ? " cells" : "") + (q.fit ? "" : " no-fit (overlap)");
            check("boxes: incremental == fresh render, every frame" + tag, eq);
            check("boxes: nothing changes outside dirty()" + tag, dirty_ok);
        }

        // A block whose text never changes is not redrawn.
        {
            FrameTextSource fx = src;
            fx.template_lines = {"Notes {nc}", "fixed label"};
            fx.template_boxes = {0, 3};
            FrameRenderer r;
            r.init(s16, p, bg, fx.block_spec());
            r.render_pieces(fx.pieces(0));
            r.render_pieces(fx.pieces(100));
            const Rect d = r.dirty();
            check("an unchanged block is left alone (dirty only around the changing one)", !d.empty() && d.y + d.h <= 180);
        }

        // Fit: one scale so the widest block fits its (narrowest) box.
        {
            TextStyle big = st;
            big.size = 60;
            FrameTextSource fx = src;
            fx.template_lines = {"A longer line {nc} {nc-total}", "Short", "A longer line {nc} {nc-total}"};
            fx.template_boxes = {1, 3, 2};
            Placement q = p;
            q.margin = 30;
            FrameRenderer r;
            r.init(big, q, bg, fx.block_spec());
            check("fit: the widest block fits its third of the frame", r.scale() < 1.0 && r.block_width() <= 213 - 60);
        }

        // No lines at all (every box empty): the frame is just the background.
        {
            FrameTextSource fx = src;
            fx.template_lines.clear();
            fx.template_boxes.clear();
            FrameRenderer r;
            const bool ok = r.init(s16, p, bg, fx.block_spec());
            const Canvas& c = r.render_pieces(fx.pieces(10));
            bool plain = ok && r.block_count() == 0;
            const uint8_t bgb[4] = {(uint8_t)(bg >> 24), (uint8_t)(bg >> 16), (uint8_t)(bg >> 8), (uint8_t)bg};
            for (size_t i = 0; i < c.px.size() && plain; i += 4) plain = std::memcmp(&c.px[i], bgb, 4) == 0;
            check("empty boxes: nothing is drawn", plain);
        }

        // A whole-frame box == no boxes (the old corner preset), all 9 anchors.
        {
            bool eq = true;
            for (int a = 1; a <= 9; ++a) {
                Placement q;
                q.frame_w = 640; q.frame_h = 360;
                q.alignment = a;
                FrameTextSource fx = make_source(frames, 30.0);
                FrameRenderer r0, r1;
                r0.init(s16, q, bg, fx.block_spec());
                BoxPlace whole;
                whole.anchor = a;
                q.boxes = {whole};
                q.alignment = 7;
                r1.init(s16, q, bg, fx.block_spec());
                for (size_t k : {(size_t)0, (size_t)150})
                    eq &= r0.render_pieces(fx.pieces(k)).px == r1.render_pieces(fx.pieces(k)).px;
            }
            check("a whole-frame box draws exactly like no boxes", eq);
        }
    }

    // ---- thirds: a label centred over its stat in each third ----------------------------
    {
        const std::vector<FrameStats> frames = make_frames(300, 30.0);
        const RGBA bg = 0x000000FFu;
        fmcg_boxes::Node root;
        fmcg_boxes::split_leaf(root, 0, fmcg_boxes::kColumns, 3);
        for (int i = 0; i < 3; ++i) fmcg_boxes::leaf_at(root, i)->box.anchor = 8;
        Placement p;
        p.frame_w = 1280; p.frame_h = 720;
        p.boxes = fmcg_boxes::place_list(root);
        p.cells = true;
        p.cell_align = 1;
        FrameTextSource src = make_source(frames, 30.0);
        src.template_lines = {"NOTES", "{nc}", "NPS", "{nps}", "POLYPHONY", "{plph}"};
        src.template_boxes = {0, 0, 1, 1, 2, 2};
        FrameRenderer fr;
        fr.init(st, p, bg, src.block_spec());
        const int l0 = p.margin, l1 = p.margin + (int)st.size, l2 = p.margin + 2 * (int)st.size;
        bool centred = true;
        for (size_t k = 0; k < src.frame_count(); k += 7) {
            const Canvas& c = fr.render_pieces(src.pieces(k));
            for (int b = 0; b < 3; ++b) {
                const int xa = (int)std::round(p.boxes[b].x0 * 1280), xb = (int)std::round(p.boxes[b].x1 * 1280);
                const double mid = (xa + xb) / 2.0;
                int lo, hi, nlo, nhi;
                auto ink_cols = [&](int y0, int y1, int& a, int& z) {
                    a = z = -1;
                    for (int y = y0; y < y1; ++y)
                        for (int x = xa; x < xb; ++x)
                            if (c.row(y)[x * 4] | c.row(y)[x * 4 + 1] | c.row(y)[x * 4 + 2]) {
                                if (a < 0 || x < a) a = x;
                                if (x > z) z = x;
                            }
                };
                ink_cols(l0, l1, lo, hi);
                ink_cols(l1, l2, nlo, nhi);
                const bool ok = lo >= 0 && nlo >= 0 && std::fabs((lo + hi + 1) / 2.0 - mid) <= 1.5 &&
                                std::fabs((nlo + nhi + 1) / 2.0 - mid) <= 2.0;
                if (!ok && centred)
                    std::printf("  frame %zu box %d: label %d..%d, stat %d..%d, centre %.1f\n", k, b, lo, hi, nlo, nhi, mid);
                centred &= ok;
            }
        }
        check("thirds: every label and stat centred in its third, every 7th frame", centred);

        // Anchored left with centred lines: the label stays put over the video.
        fmcg_boxes::leaf_at(root, 0)->box.anchor = 7;
        fmcg_boxes::leaf_at(root, 0)->box.line_align = 2;
        Placement q = p;
        q.boxes = fmcg_boxes::place_list(root);
        q.cells = false;
        FrameRenderer r;
        r.init(st, q, bg, src.block_spec());
        int lo0 = -1, hi0 = -1;
        bool stable = true;
        for (size_t k = 0; k < src.frame_count(); k += 7) {
            const Canvas& c = r.render_pieces(src.pieces(k));
            int lo = -1, hi = -1;
            for (int y = l0; y < l1; ++y)
                for (int x = 0; x < 426; ++x)
                    if (c.row(y)[x * 4] | c.row(y)[x * 4 + 1] | c.row(y)[x * 4 + 2]) {
                        if (lo < 0 || x < lo) lo = x;
                        if (x > hi) hi = x;
                    }
            if (lo0 < 0) { lo0 = lo; hi0 = hi; }
            stable &= lo == lo0 && hi == hi0;
        }
        check("anchor left + centred lines: the label stays put over the video", stable && lo0 > p.margin + 5);
    }

    std::printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
