// Pattern / global settings files written by earlier versions still load:
// missing keys take defaults, changed meanings are migrated, junk is clamped,
// and the current format round-trips. Only parses text (no settings folder).
#include "../../gui/settings_store.h"
#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); ++fails; } \
                         else std::printf("[OK]   %s\n", #cond); } while (0)

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
        CHECK(p.layout_text == "Ticks: {tick}/{tick-total}\nTime: {time-milli}/{time-milli-max}");
        CHECK(p.outline_enabled && p.outline_width == 1.0 && p.outline_color_aabbggrr == "&H00000000");
        CHECK(p.commas.ticks);              // key added later: default
        CHECK(p.bpm.decimals == 2);         // key added later: default
        CHECK(p.frame_anchor == 0 && p.block_anchor == 0 && !p.cells && p.cell_align == 2);
        CHECK(p.typo == fmcg_text::Typography());
        CHECK(p.font_family == "Arial" && p.font_size == 36);
    }
    {   // ASS era custom position: \pos anchored the block by the corner alignment.
        const int want[6] = {0, 2, 6, 8, 1, 7};
        for (int a = 0; a < 6; ++a) {
            PatternData p;
            pattern_from_ini("alignment = " + std::to_string(a) + "\npos_mode = 1\npos_x = 100\npos_y = 200\n", p);
            CHECK(p.pos_mode == 1 && p.pos_x == 100 && p.pos_y == 200);
            CHECK(p.block_anchor == want[a] && p.frame_anchor == 0);
        }
    }
    {   // Renderer era before anchors (has outline keys): custom x,y was the block's top-left.
        PatternData p;
        pattern_from_ini("alignment = 3\npos_mode = 1\npos_x = 10\npos_y = 20\n"
                         "outline_enabled = 0\noutline_width = 2.5\noutline_color = &H000000FF\n", p);
        CHECK(p.block_anchor == 0 && p.frame_anchor == 0);
        CHECK(!p.outline_enabled && p.outline_width == 2.5);
    }
    {   // Junk values are clamped, never crash.
        PatternData p;
        pattern_from_ini("alignment = 99\npos_mode = 7\nframe_anchor = -1\nblock_anchor = 12\ncell_align = 5\n"
                         "font_size = -3\noutline_width = 900\nbpm_decimals = 40\ntypo_numerals = 9\n"
                         "line_spacing = 0\nletter_spacing = nan\ntext_color = &HGG\n", p);
        CHECK(p.alignment == 0 && p.pos_mode == 0 && p.frame_anchor == 0 && p.block_anchor == 0);
        CHECK(p.cell_align == 2 && p.font_size == 36 && p.outline_width == 20.0 && p.bpm.decimals == 6);
        CHECK(p.typo.numerals == 0 && p.typo.line_spacing == 1.0 && p.typo.letter_spacing == 0.0);
    }
    {   // Current format round-trips exactly.
        PatternData p;
        p.layout_text = "A {nc}\nB {nps}";
        p.alignment = 3; p.pos_mode = 1; p.pos_x = 7; p.pos_y = 9; p.frame_anchor = 4; p.block_anchor = 8;
        p.cells = true; p.cell_align = 1; p.font_family = "Iosevka"; p.font_variant = "Bold"; p.font_size = 48;
        p.text_color_aabbggrr = "&H0000FF00"; p.outline_enabled = false; p.outline_width = 3.5;
        p.typo.slashed_zero = true; p.typo.numerals = 1; p.typo.stylistic_sets = 0x5; p.typo.extra = "cv01";
        p.typo.letter_spacing = 2.0; p.typo.line_spacing = 1.25; p.commas.cc = true; p.pad.enabled = true;
        p.bpm.decimals = 4;
        PatternData q;
        pattern_from_ini(pattern_to_ini("x", p), q);
        CHECK(q == p);
    }
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
    }
    std::printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
