// Pattern / global settings files written by earlier versions still load:
// missing keys take defaults, changed meanings are migrated, junk is clamped,
// and the current format round-trips. Only parses text (no settings folder).
#include "../../gui/settings_store.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); ++fails; } \
                         else std::printf("[OK]   %s\n", #cond); } while (0)

// Every old placement (corner presets; custom x,y with 9 x 9 frame / block
// anchors) converted to boxes draws exactly where the old formula put the
// block: px-identical canvases.
static std::string find_font() {
    if (const char* e = std::getenv("FMCG_TEST_FONT")) return e;
    const char* cands[] = {"C:/Windows/Fonts/arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf"};
    for (const char* c : cands)
        if (FILE* f = std::fopen(c, "rb")) { std::fclose(f); return c; }
    return "";
}

static void migrated_render_check() {
    using namespace fmcg_text;
    const std::string font = find_font();
    if (font.empty()) { std::printf("SKIP migrated render check: no font\n"); return; }
    const int W = 640, H = 360;
    const RGBA bg = 0x102030FFu;
    const std::vector<std::string> lines = {"Notes 1,234,567", "Poly 42"};
    int compared = 0, same = 0, skipped = 0;
    std::mt19937 rng(5);
    for (int outline = 0; outline < 2; ++outline)
        for (int c = 0; c < 6 * 3 + 81 * 4; ++c) {
            TextStyle st;
            st.font_file = font;
            st.size = 20;
            st.outline = outline == 1;
            st.outline_width = 2;
            std::string ini = outline ? "outline_enabled = 1\noutline_width = 2\n" : "outline_enabled = 0\n";
            int mode, align = 0, fa = 0, ba = 0, px = 30, py = 30;
            if (c < 18) { mode = 0; align = c % 6; }
            else {
                mode = 1;
                const int k = (c - 18) % 81;
                fa = k / 9; ba = k % 9;
                const int pick = (c - 18) / 81;   // near edges, centre, random, outside
                px = pick == 0 ? 5 : pick == 1 ? 0 : pick == 2 ? (int)(rng() % 640) - 50 : (int)(rng() % 300);
                py = pick == 0 ? 7 : pick == 1 ? 0 : pick == 2 ? (int)(rng() % 360) - 40 : (int)(rng() % 180);
            }
            ini += "alignment = " + std::to_string(align) + "\npos_mode = " + std::to_string(mode) +
                   "\npos_x = " + std::to_string(px) + "\npos_y = " + std::to_string(py) +
                   "\nframe_anchor = " + std::to_string(fa) + "\nblock_anchor = " + std::to_string(ba) + "\n";
            PatternData p;
            pattern_from_ini(ini, p, W, H);
            // New: the converted boxes.
            Placement pl;
            pl.frame_w = W; pl.frame_h = H;
            pl.boxes = fmcg_boxes::place_list(p.boxes);
            pl.margin = p.margin;
            std::vector<int> lb;
            std::vector<std::string> tl;
            pattern_template(p, tl, lb);
            TextBlockSpec spec;
            for (const std::string& l : lines) spec.pieces.push_back({TextPiece{l, -1}});
            spec.extremes.resize(lines.size());
            spec.boxes.assign(lines.size(), lb.empty() ? 0 : lb[0]);
            FrameRenderer fr;
            fr.init(st, pl, bg, spec);
            if (fr.scale() != 1.0) { ++skipped; continue; }   // the box is too small: fit differs by design
            const Canvas& got = fr.render(lines);
            // Old: the pre-box formula (preset at the 30 px margin, or frame
            // point + block anchor), clamped to the frame with the 1 px edge.
            TextRenderer tr;
            tr.load(st);
            const Layout L = tr.layout(lines);
            const double w = L.width, h = L.height();
            auto R = [](int a) { return a % 3 == 2; };
            auto C = [](int a) { return a % 3 == 1; };
            auto B = [](int a) { return a / 3 == 2; };
            auto M = [](int a) { return a / 3 == 1; };
            double x0, y0;
            int lalign;   // 3x3 index whose side the lines align to
            if (mode == 0) {
                static const int idx[6] = {0, 2, 6, 8, 1, 7};
                const int a = idx[align];
                x0 = R(a) ? W - 30 - w : C(a) ? W / 2.0 - w / 2 : 30;
                y0 = B(a) ? H - 30 - h : 30;
                lalign = a;
            } else {
                const double X = R(fa) ? W - px : C(fa) ? W / 2.0 + px : px;
                const double Y = B(fa) ? H - py : M(fa) ? H / 2.0 + py : py;
                x0 = X - (R(ba) ? w : C(ba) ? w / 2 : 0);
                y0 = Y - (B(ba) ? h : M(ba) ? h / 2 : 0);
                lalign = ba;
            }
            const double pad = 1.0 + (st.outline ? std::ceil(st.outline_width + 0.5) : 0.0);
            x0 = std::max(pad, std::min(x0, W - w - pad));
            y0 = std::max(pad, std::min(y0, H - h - pad));
            Placement op;   // draws at (x0, y0) with the old line alignment
            op.frame_w = W; op.frame_h = H;
            op.alignment = (lalign / 3 == 0 ? 7 : lalign / 3 == 1 ? 4 : 1) + lalign % 3;
            Canvas want;
            want.resize(W, H);
            want.fill(bg);
            tr.draw(want, L, op, x0, y0);
            ++compared;
            if (want.px == got.px) ++same;
            else if (compared - same <= 3)
                std::printf("  differs: mode %d align %d fa %d ba %d x,y %d,%d outline %d\n", mode, align, fa, ba, px, py, outline);
        }
    std::printf("  migrated placements: %d compared, %d identical, %d skipped (box too small to fit)\n",
                compared, same, skipped);
    CHECK(compared > 400 && same == compared);
}

int main() {
    {   // Shipped defaults: 1 px black outline on.
        PatternData d;
        CHECK(d.outline_enabled);
        CHECK(d.outline_width == 1.0);
        CHECK(d.outline_color_aabbggrr == "&H00000000");
    }
    {   // ASS era (first pattern format, Sep 2026): libass drew a 1 px black outline.
        PatternData p;
        pattern_from_ini("# fMCG pattern: Default + Ticks\n"
                         "layout = Ticks: {tick}/{tick-total}\\nTime: {time-milli}/{time-milli-max}\n"
                         "alignment = 0\npos_mode = 0\npos_x = 30\npos_y = 30\nfont_family = Arial\n"
                         "font_variant = \nfont_size = 36\ntext_color = &H00FFFFFF\nbg_color = &H00000000\n"
                         "comma_notes = 1\ncomma_poly = 1\ncomma_nps = 1\ncomma_cc = 0\npad_enabled = 0\n", p);
        CHECK(fmcg_boxes::leaf_count(p.boxes) == 1);
        CHECK(p.boxes.box.text == "Ticks: {tick}/{tick-total}\nTime: {time-milli}/{time-milli-max}");
        CHECK(p.boxes.box.anchor == 7 && p.boxes.box.line_align == 0 && p.margin == 30);
        CHECK(p.outline_enabled && p.outline_width == 1.0 && p.outline_color_aabbggrr == "&H00000000");
        CHECK(p.commas.ticks);              // key added later: default
        CHECK(p.bpm.decimals == 2);         // key added later: default
        CHECK(!p.cells && p.cell_align == 2);
        CHECK(p.typo == fmcg_text::Typography());
        CHECK(p.font_family == "Arial" && p.font_size == 36);
    }
    {   // Corner presets -> one whole-frame box anchored there.
        const int want[6] = {7, 9, 1, 3, 8, 2};
        for (int a = 0; a < 6; ++a) {
            PatternData p;
            pattern_from_ini("alignment = " + std::to_string(a) + "\noutline_enabled = 1\n", p);
            CHECK(fmcg_boxes::leaf_count(p.boxes) == 1 && p.boxes.box.anchor == want[a] && p.margin == 30);
        }
    }
    {   // Custom x,y -> a box whose edge is the point, margin 0. ASS era: anchored
        // by the corner alignment (0 = top-left .. 5 = bottom centre).
        PatternData p;
        pattern_from_ini("alignment = 0\npos_mode = 1\npos_x = 100\npos_y = 200\n", p, 1000, 500);
        CHECK(p.margin == 0);
        const fmcg_boxes::Node* l = nullptr;
        int li = -1;
        for (int i = 0; i < fmcg_boxes::leaf_count(p.boxes); ++i)
            if (!fmcg_boxes::leaf_at(p.boxes, i)->box.text.empty()) { l = fmcg_boxes::leaf_at(p.boxes, i); li = i; }
        CHECK(l && l->box.anchor == 7);
        if (l) {
            const fmcg_boxes::FRect r = fmcg_boxes::leaf_rects(p.boxes)[li];
            CHECK(std::fabs(r.x0 - 0.1) < 1e-9 && std::fabs(r.y0 - 0.4) < 1e-9 && r.x1 == 1.0 && r.y1 == 1.0);
        }
        PatternData q;   // bottom-right alignment: block's bottom-right at (100, 200)
        pattern_from_ini("alignment = 3\npos_mode = 1\npos_x = 100\npos_y = 200\n", q, 1000, 500);
        const std::vector<fmcg_boxes::FRect> rq = fmcg_boxes::leaf_rects(q.boxes);
        bool found = false;
        for (int i = 0; i < (int)rq.size(); ++i)
            if (fmcg_boxes::leaf_at(q.boxes, i)->box.anchor == 3 && !fmcg_boxes::leaf_at(q.boxes, i)->box.text.empty())
                found = rq[i].x0 == 0.0 && std::fabs(rq[i].x1 - 0.1) < 1e-9 && rq[i].y0 == 0.0 && std::fabs(rq[i].y1 - 0.4) < 1e-9;
        CHECK(found);
    }
    {   // Junk values are clamped, never crash.
        PatternData p;
        pattern_from_ini("alignment = 99\npos_mode = 7\nframe_anchor = -1\nblock_anchor = 12\ncell_align = 5\n"
                         "font_size = -3\noutline_width = 900\nbpm_decimals = 40\ntypo_numerals = 9\n"
                         "line_spacing = 0\nletter_spacing = nan\ntext_color = &HGG\n", p);
        CHECK(fmcg_boxes::leaf_count(p.boxes) == 1 && p.boxes.box.anchor == 7);
        CHECK(p.cell_align == 2 && p.font_size == 36 && p.outline_width == 20.0 && p.bpm.decimals == 6);
        CHECK(p.typo.numerals == 0 && p.typo.line_spacing == 1.0 && p.typo.letter_spacing == 0.0);
        PatternData b;   // broken box keys
        pattern_from_ini("boxes = C(0.5:B,junk\nbox1_anchor = 12\nmargin = -5\nlayout = old\n", b);
        CHECK(fmcg_boxes::leaf_count(b.boxes) == 1 && b.boxes.box.text == "old" && b.margin == 30);
        PatternData c;
        pattern_from_ini("boxes = C(1:B,2:B)\nbox1_anchor = 12\nbox2_line_align = 9\nmargin = 5000\n", c);
        CHECK(fmcg_boxes::leaf_count(c.boxes) == 2 && fmcg_boxes::leaf_at(c.boxes, 0)->box.anchor == 7 &&
              fmcg_boxes::leaf_at(c.boxes, 1)->box.line_align == 0 && c.margin == 30);
        CHECK(std::fabs(c.boxes.weights[0] - 1.0 / 3) < 1e-12);
    }
    {   // Current format round-trips exactly.
        PatternData p;
        p.boxes.box.text = "A {nc}\nB {nps}";
        fmcg_boxes::split_leaf(p.boxes, 0, fmcg_boxes::kRows, 2);
        fmcg_boxes::split_leaf(p.boxes, 1, fmcg_boxes::kColumns, 3);
        fmcg_boxes::leaf_at(p.boxes, 2)->box = {"NPS\n{nps}", 8, 2};
        fmcg_boxes::leaf_at(p.boxes, 3)->box = {"x = y", 3, 3};
        p.boxes.weights = {0.3, 0.7};
        p.margin = 12;
        p.cells = true; p.cell_align = 1; p.font_family = "Iosevka"; p.font_variant = "Bold"; p.font_size = 48;
        p.text_color_aabbggrr = "&H0000FF00"; p.outline_enabled = false; p.outline_width = 3.5;
        p.typo.slashed_zero = true; p.typo.numerals = 1; p.typo.stylistic_sets = 0x5; p.typo.extra = "cv01";
        p.typo.letter_spacing = 2.0; p.typo.line_spacing = 1.25; p.commas.cc = true; p.pad.enabled = true;
        p.bpm.decimals = 4;
        PatternData q;
        pattern_from_ini(pattern_to_ini("x", p), q);
        CHECK(q == p);
        std::vector<std::string> lines;
        std::vector<int> boxes;
        pattern_template(q, lines, boxes);
        CHECK(lines == std::vector<std::string>({"A {nc}", "B {nps}", "NPS", "{nps}", "x = y"}));
        CHECK(boxes == std::vector<int>({0, 0, 2, 2, 3}));
        PatternData e;   // no text anywhere: the default layout in box 0
        fmcg_boxes::split_leaf(e.boxes, 0, fmcg_boxes::kColumns, 2);
        pattern_template(e, lines, boxes);
        CHECK(!lines.empty() && boxes == std::vector<int>(lines.size(), 0));
    }
    migrated_render_check();
    {   // Globals: the old single "Dedup" checkbox (track_dedup) meant layered dedup on archives.
        GlobalSettings g;
        globals_from_ini("width = 1280\nheight = 720\ncc_stats = 1\ntrack_dedup = 1\n", g);
        CHECK(g.dedup_mode == 2 && g.width == 1280 && g.height == 720);
        GlobalSettings h;
        globals_from_ini("track_dedup = 0\n", h);
        CHECK(h.dedup_mode == 0);
        GlobalSettings r;
        r.dedup_mode = 1; r.x264_crf = 23; r.x264_preset = "slow"; r.fps = 30;
        GlobalSettings s;
        globals_from_ini(globals_to_ini(r), s);
        CHECK(s.dedup_mode == 1 && s.x264_crf == 23 && s.x264_preset == "slow" && s.fps == 30);
        GlobalSettings lim;   // limits: FPS <= 1000, delays <= 1 hour; nan/inf -> defaults
        globals_from_ini("fps = 5000\nstart_delay = 99999\nend_delay = 7200\n", lim);
        CHECK(lim.fps == 1000.0 && lim.start_delay == 3600.0 && lim.end_delay == 3600.0);
        globals_from_ini("fps = nan\nstart_delay = inf\n", lim);
        CHECK(lim.fps == 60.0 && lim.start_delay == 3.0);
    }
    std::printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
