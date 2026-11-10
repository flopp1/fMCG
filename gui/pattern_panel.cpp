// pattern_panel.cpp -- the right column: everything a pattern saves (layout
// editor, font, colours, outline, placement, typography, number formats) and
// the pattern manager.
#include "app_state.h"
#include "jobs.h"
#include "dialogs.h"
#include "preview.h"
#include "colour_edit.h"
#include "settings_store.h"

#include "fMCG_core.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include <thread>
#include <mutex>
#include <atomic>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>

#include <GL/gl.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include "panels.h"

static void draw_typography_options() {
    if (!ImGui::TreeNode("Advanced typography")) return;
    fmcg_text::Typography& t = g_ui.s.pattern.typo;
    const float k_ty_label = 150.0f;   // label column inside the indented tree

    // Features of the selected font, re-read only when the family/style changes.
    static std::string feats_for;
    static std::vector<fmcg_text::FontFeature> feats;
    const std::string fkey = g_ui.s.pattern.font_family + "|" + g_ui.s.pattern.font_variant;
    if (fkey != feats_for) {
        feats_for = fkey;
        int face = 0;
        const std::string file = find_font_file_for_variant(g_ui.s.pattern.font_family, g_ui.s.pattern.font_variant, &face);
        feats = fmcg_text::font_features(file, face);
    }
    auto has = [&](const std::string& tag) {
        for (const auto& f : feats)
            if (f.tag == tag) return true;
        return false;
    };
    std::vector<std::string> missing;   // chosen options this font does not have

    ImGui::Checkbox("Kerning##ty", &t.kerning);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pair kerning from the font (OpenType 'kern'). On by default.");
    ImGui::SameLine();
    ImGui::Checkbox("Ligatures##ty", &t.ligatures);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Standard ligatures such as fi / fl ('liga'). On by default.");
    ImGui::SameLine();
    ImGui::Checkbox("Slashed zero##ty", &t.slashed_zero);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The font's slashed zero, to tell it from O ('zero').\n"
                          "Fonts whose zero is already slashed or dotted may show no change.\n"
                          "Many fonts keep other zero styles in stylistic sets or character\n"
                          "variants instead (e.g. Victor Mono ss02..ss05, Iosevka cv10=1..30\n"
                          "via More features).");
    if (t.slashed_zero && !has("zero")) missing.push_back("slashed zero");

    ImGui::Text("Numerals");
    ImGui::SameLine(k_ty_label);
    ImGui::SetNextItemWidth(130);
    static const char* k_num_style[] = {"Font default", "Lining", "Old-style"};
    ImGui::Combo("##tynum", &t.numerals, k_num_style, 3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lining: capital-height digits ('lnum').\n"
                          "Old-style: digits with ascenders/descenders, like lowercase text ('onum').");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    static const char* k_num_width[] = {"Font default", "Proportional", "Tabular"};
    ImGui::Combo("##tynumw", &t.numeral_width, k_num_width, 3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Tabular: every digit has the same width ('tnum'), so counters do not\n"
                          "shift sideways as their digits change.\n"
                          "Proportional: each digit takes its natural width ('pnum').");
    if (t.numerals == 1 && !has("lnum")) missing.push_back("lining numerals");
    if (t.numerals == 2 && !has("onum")) missing.push_back("old-style numerals");
    if (t.numeral_width == 1 && !has("pnum")) missing.push_back("proportional numerals");
    if (t.numeral_width == 2 && !has("tnum")) missing.push_back("tabular numerals");

    // Stylistic sets: the font's own, plus any the pattern enables that this font lacks.
    {
        bool any = false, prev_unnamed = false;
        for (int n = 1; n <= 20; ++n) {
            char tag[8];
            std::snprintf(tag, sizeof tag, "ss%02d", n);
            const uint32_t bit = 1u << (n - 1);
            const bool in_font = has(tag);
            if (!in_font && !(t.stylistic_sets & bit)) continue;
            std::string label = tag;
            for (const auto& f : feats)
                if (f.tag == tag && !f.name.empty()) label += "  " + f.name;
            if (!in_font) label += "  (not in this font)";
            label += std::string("##ty") + tag;
            bool on = (t.stylistic_sets & bit) != 0;
            // unnamed sets flow on one row; named ones get their own line;
            // all start in the label column
            const bool named = label.find("  ") != std::string::npos;
            if (!any) { ImGui::Text("Stylistic sets"); ImGui::SameLine(k_ty_label); any = true; }
            else if (prev_unnamed && !named) ImGui::SameLine();
            else { ImGui::Dummy(ImVec2(0, 0)); ImGui::SameLine(k_ty_label); }
            if (ImGui::Checkbox(label.c_str(), &on)) t.stylistic_sets = on ? (t.stylistic_sets | bit) : (t.stylistic_sets & ~bit);
            prev_unnamed = !named;
        }
        if (!any) ImGui::TextDisabled("Stylistic sets: none in this font");
    }

    ImGui::Text("More features");
    ImGui::SameLine(k_ty_label);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    {
        static char buf[256];
        if (t.extra != buf) {   // pattern switched / reset
            std::strncpy(buf, t.extra.c_str(), sizeof buf - 1);
            buf[sizeof buf - 1] = '\0';
        }
        if (ImGui::InputTextWithHint("##tyextra", "e.g. cv01 salt -calt smcp", buf, sizeof buf)) t.extra = buf;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Any other OpenType features, separated by spaces or commas.\n"
                              "'tag' turns a feature on, '-tag' off, 'tag=2' picks alternate 2.\n"
                              "These are applied last, so they override the options above.");
        std::string bad;
        for (const std::string& f : fmcg_text::split_features(t.extra)) {
            if (!fmcg_text::valid_feature(f)) { bad += (bad.empty() ? "" : ", ") + f; continue; }
            std::string tag = f;
            if (!tag.empty() && (tag[0] == '-' || tag[0] == '+')) tag.erase(0, 1);
            tag = tag.substr(0, std::min(tag.find_first_of("=["), tag.size()));
            if (!has(tag)) missing.push_back("'" + tag + "'");
        }
        if (!bad.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Ignored (not a valid feature): %s", bad.c_str());
    }

    ImGui::Text("Letter spacing");
    ImGui::SameLine(k_ty_label);
    ImGui::SetNextItemWidth(90);
    float ls = (float)t.letter_spacing;
    if (ImGui::InputFloat("px##tyls", &ls, 0.0f, 0.0f, "%.1f")) t.letter_spacing = std::max(-50.0f, std::min(200.0f, ls));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Extra space between characters, in video pixels (negative = tighter).");
    ImGui::SameLine();
    ImGui::Text("Line spacing");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    float lsp = (float)t.line_spacing;
    if (ImGui::InputFloat("x##tylsp", &lsp, 0.0f, 0.0f, "%.2f")) t.line_spacing = std::max(0.5f, std::min(4.0f, lsp));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distance between lines as a multiple of the font size (1.00 = default).");

    if (!missing.empty()) {
        std::string m;
        for (const std::string& s : missing) m += (m.empty() ? "" : ", ") + s;
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.0f), "Not in this font (no effect): %s", m.c_str());
    }
    {
        // Registered (lowercase) tags only -- uppercase ones are font-private --
        // with numbered runs collapsed: "cv01..cv99".
        std::string tags;
        for (size_t i = 0; i < feats.size(); ++i) {
            const std::string& tg = feats[i].tag;
            if (tg.empty() || (tg[0] >= 'A' && tg[0] <= 'Z')) continue;
            size_t j = i;
            auto numbered = [](const std::string& a) { return a.size() == 4 && isdigit((unsigned char)a[2]) && isdigit((unsigned char)a[3]); };
            if (numbered(tg))
                while (j + 1 < feats.size() && numbered(feats[j + 1].tag) && feats[j + 1].tag.compare(0, 2, tg, 0, 2) == 0 &&
                       std::atoi(feats[j + 1].tag.c_str() + 2) == std::atoi(feats[j].tag.c_str() + 2) + 1)
                    ++j;
            tags += (tags.empty() ? "" : " ") + tg;
            if (j > i + 1) tags += ".." + feats[j].tag;
            else if (j == i + 1) tags += " " + feats[j].tag;
            i = j;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Font features: %s", tags.empty() ? "(none found)" : tags.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::Button("Reset typography")) t = fmcg_text::Typography();
    ImGui::TreePop();
}

// Layout editor with real horizontal + vertical scrollbars. ImGui's multiline
// input has none horizontally: it jumps sideways by a quarter of its width to
// follow the cursor and snaps back when it loses focus. Instead the input is
// sized to its text (never scrolls itself) inside a child window that scrolls
// both ways; the child follows the text cursor while typing.
static int layout_cursor_cb(ImGuiInputTextCallbackData* d) {
    *(int*)d->UserData = d->CursorPos;
    return 0;
}

// Cursor in the layout editor (byte offset; -1 = never clicked). Kept after
// the editor loses focus, so "Insert stat" knows where the user was.
static int g_layout_cursor = -1;

// Inserts `text` at the cursor (on a new last line when the editor was never
// used) and moves the cursor after it, so several stats can be added in a row.
static void insert_layout_text(const std::string& text) {
    std::string buf = g_ui.layout_buf;
    std::string ins = text;
    size_t at;
    if (g_layout_cursor < 0 || (size_t)g_layout_cursor > buf.size()) {
        at = buf.size();
        if (!buf.empty() && buf.back() != '\n') ins = "\n" + ins;
    } else {
        at = (size_t)g_layout_cursor;
    }
    if (buf.size() + ins.size() >= sizeof(g_ui.layout_buf)) return;   // no room
    buf.insert(at, ins);
    std::memcpy(g_ui.layout_buf, buf.c_str(), buf.size() + 1);
    g_layout_cursor = (int)(at + ins.size());
}

// Every stat token, in the order of the Stats list, with a short description.
static const char* const k_insert_stats[][2] = {
    {"{nc}", "notes played"}, {"{nc-total}", "notes in the song"}, {"{nc-rem}", "notes remaining"},
    {"{nps}", "notes per second"}, {"{nps-max}", "peak notes per second"},
    {"{plph}", "polyphony"}, {"{plph-max}", "peak polyphony"},
    {"{cc}", "CC events so far"}, {"{cc-total}", "CC events in the song"}, {"{cc-rem}", "CC events remaining"},
    {"{cps}", "CC per second"}, {"{cps-max}", "peak CC per second"},
    {"{time}", "time (m:ss)"}, {"{time-max}", "song length (m:ss)"}, {"{time-rem}", "time remaining (m:ss)"},
    {"{time-milli}", "time (m:ss.mmm)"}, {"{time-milli-max}", "song length (m:ss.mmm)"},
    {"{time-milli-rem}", "time remaining (m:ss.mmm)"},
    {"{sec}", "seconds"}, {"{sec-max}", "song length in seconds"}, {"{sec-rem}", "seconds remaining"},
    {"{tick}", "MIDI tick"}, {"{tick-total}", "ticks in the song"}, {"{tick-rem}", "ticks remaining"},
    {"{bpm}", "tempo"}, {"{ppqn}", "ticks per quarter note"},
    {"{beat}", "beat"}, {"{beat-total}", "beats in the song"}, {"{beat-rem}", "beats remaining"},
    {"{measure}", "measure"}, {"{measure-total}", "measures in the song"}, {"{measure-rem}", "measures remaining"},
    {"{measure-beat}", "beat within the measure"}, {"{timesig}", "time signature"},
};

static void draw_layout_editor(float height) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const char* text = g_ui.layout_buf;
    const float line_h = ImGui::GetTextLineHeight();
    float max_w = 0.0f;
    int lines = 1;
    for (const char* p = text;;) {
        const char* e = std::strchr(p, '\n');
        max_w = std::max(max_w, ImGui::CalcTextSize(p, e ? e : p + std::strlen(p)).x);
        if (!e) break;
        ++lines;
        p = e + 1;
    }
    const float slack = ImGui::CalcTextSize("MM").x;   // room for the next typed characters

    ImGui::PushStyleColor(ImGuiCol_ChildBg, st.Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##layout_area", ImVec2(-1, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(avail.x, max_w + 2.0f * st.FramePadding.x + slack),
                      std::max(avail.y, lines * line_h + 2.0f * st.FramePadding.y + line_h));
    int& cursor = g_layout_cursor;
    static int last_cursor = -1;
    static int follow_frames = 0;   // re-apply for a few frames: the box grows a frame after the text
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);   // the child draws the border
    ImGui::InputTextMultiline("##layout", g_ui.layout_buf, sizeof(g_ui.layout_buf), size,
                              ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_NoHorizontalScroll,
                              layout_cursor_cb, &cursor);
    ImGui::PopStyleVar();
    if (ImGui::IsItemActive() && cursor >= 0 && cursor != last_cursor) follow_frames = 3;
    if (ImGui::IsItemActive() && cursor >= 0 && follow_frames > 0) {
        --follow_frames;
        // Scroll the child so the cursor (moved by typing / arrow keys) stays visible.
        const char* line_start = text;
        int line = 0;
        for (const char* p = text; p < text + cursor && *p; ++p)
            if (*p == '\n') { ++line; line_start = p + 1; }
        const float cx = st.FramePadding.x + ImGui::CalcTextSize(line_start, text + cursor).x;
        const float cy = st.FramePadding.y + line * line_h;
        const float vw = ImGui::GetWindowWidth() - (ImGui::GetScrollMaxY() > 0 ? st.ScrollbarSize : 0.0f);
        const float vh = ImGui::GetWindowHeight() - (ImGui::GetScrollMaxX() > 0 ? st.ScrollbarSize : 0.0f);
        const float margin = slack;
        if (cx - margin < ImGui::GetScrollX()) ImGui::SetScrollX(std::max(0.0f, cx - margin));
        else if (cx + margin > ImGui::GetScrollX() + vw) ImGui::SetScrollX(cx + margin - vw);
        if (cy < ImGui::GetScrollY()) ImGui::SetScrollY(cy);
        else if (cy + line_h + st.FramePadding.y > ImGui::GetScrollY() + vh)
            ImGui::SetScrollY(cy + line_h + st.FramePadding.y - vh);
    }
    last_cursor = ImGui::IsItemActive() ? cursor : -1;
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// Pattern section: the overlay look, saved with the selected pattern (right column).
// ---- Box editor ----------------------------------------------------------------
// A thumbnail of the frame (the pattern as it is being edited, at the last
// processed frame or with zero stats) with the boxes drawn over it. Click a
// box to edit its text below; drag the line between two boxes to resize them
// (snapping to 1/2, 1/3, 1/4, ... of the frame and to other box edges; hold
// Alt to move freely).

// "1/3" for simple fractions, else a percentage.
static std::string fraction_label(double f) {
    for (int d : {2, 3, 4, 5, 6, 8, 10, 12})
        for (int k = 1; k < d; ++k)
            if (std::fabs(f - (double)k / d) < 1e-6) {
                int a = k, b = d;   // lowest terms
                for (int g = 2; g <= a; ++g)
                    while (a % g == 0 && b % g == 0) { a /= g; b /= g; }
                return std::to_string(a) + "/" + std::to_string(b);
            }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f%%", f * 100.0);
    return buf;
}

static void draw_box_editor(float max_thumb_h, float* out_thumb_h = nullptr) {
    using namespace fmcg_boxes;
    Node& root = g_ui.s.pattern.boxes;
    const int W = std::max(16, g_ui.s.global.width), H = std::max(16, g_ui.s.global.height);
    if (g_ui.selected_box >= leaf_count(root)) select_box(leaf_count(root) - 1);
    const int sel = g_ui.selected_box;
    // The box selected before the current one: Delete gives the space back to
    // it and selects it again (add a box, delete it = nothing changed).
    static int seen_sel = -1, prev_sel = -1;
    if (sel != seen_sel) { prev_sel = seen_sel; seen_sel = sel; }

    ImGui::Text("Boxes");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The frame is split into boxes; each box has its own text, drawn at the\n"
                          "point of the box chosen under \"Text sits at\".\n"
                          "Click a box to select it and edit its text below.\n"
                          "Drag the line between two boxes to resize them. It snaps to 1/2, 1/3,\n"
                          "1/4, ... of the frame and to other box edges; hold Alt to move freely.\n"
                          "\"Exact size\" below sets the selected box's edges to typed percentages.\n"
                          "Boxes without text show nothing.");

    // Thumbnail size: the panel's width at the video's aspect, at most 260 px high.
    float tw = ImGui::GetContentRegionAvail().x, th = tw * (float)H / (float)W;
    if (th > max_thumb_h) { th = max_thumb_h; tw = th * (float)W / (float)H; }
    if (out_thumb_h) *out_thumb_h = th;
    if (tw < 32.0f || th < 18.0f) return;
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + tw, p0.y + th);
    const ImTextureID tex = fmcg_preview::pattern_thumbnail(current_pattern_from_ui());
    ImGui::InvisibleButton("##boxcanvas", ImVec2(tw, th));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex) dl->AddImage(tex, p0, p1);
    else dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 20, 255));
    dl->PushClipRect(ImVec2(std::floor(p0.x), std::floor(p0.y)), ImVec2(std::ceil(p1.x), std::ceil(p1.y)), true);

    auto sx = [&](double f) { return p0.x + (float)f * tw; };
    auto sy = [&](double f) { return p0.y + (float)f * th; };
    const ImGuiIO& io = ImGui::GetIO();
    const double mx = (io.MousePos.x - p0.x) / tw, my = (io.MousePos.y - p0.y) / th;

    // Boxes: outline, number, the anchor point, the selected one highlighted.
    const std::vector<FRect> rects = leaf_rects(root);
    const float mgx = (float)g_ui.s.pattern.margin / W * tw, mgy = (float)g_ui.s.pattern.margin / H * th;
    for (size_t i = 0; i < rects.size(); ++i) {
        const FRect& r = rects[i];
        // Whole pixels, and the border drawn as bands inside the box: every
        // side the same width, none clipped at the thumbnail's edge.
        const ImVec2 a(std::round(sx(r.x0)), std::round(sy(r.y0))), b(std::round(sx(r.x1)), std::round(sy(r.y1)));
        const bool is_sel = (int)i == sel;
        auto band = [&](ImVec2 q0, ImVec2 q1, float t, ImU32 col) {
            dl->AddRectFilled(q0, ImVec2(q1.x, q0.y + t), col);                       // top
            dl->AddRectFilled(ImVec2(q0.x, q1.y - t), q1, col);                       // bottom
            dl->AddRectFilled(ImVec2(q0.x, q0.y + t), ImVec2(q0.x + t, q1.y - t), col);   // left
            dl->AddRectFilled(ImVec2(q1.x - t, q0.y + t), ImVec2(q1.x, q1.y - t), col);   // right
        };
        if (is_sel) dl->AddRectFilled(a, b, IM_COL32(255, 200, 0, 36));
        const float t = is_sel ? 2.0f : 1.0f;
        band(a, b, t, is_sel ? IM_COL32(255, 200, 0, 255) : IM_COL32(255, 255, 255, 150));
        band(ImVec2(a.x + t, a.y + t), ImVec2(b.x - t, b.y - t), 1.0f, IM_COL32(0, 0, 0, 170));   // contrast on light video
        // Box number in the bottom-right corner (clear of the usual top-left text).
        const std::string num = std::to_string(i + 1);
        const ImVec2 ns = ImGui::CalcTextSize(num.c_str());
        const ImVec2 np(b.x - ns.x - 4, b.y - ns.y - 2);
        dl->AddText(ImVec2(np.x + 1, np.y + 1), IM_COL32(0, 0, 0, 200), num.c_str());
        dl->AddText(np, is_sel ? IM_COL32(255, 200, 0, 255) : IM_COL32(255, 255, 255, 200), num.c_str());
        // Anchor point of the box inset by the margin.
        const int an = leaf_at(root, (int)i)->box.anchor;
        float ix0 = a.x + mgx, ix1 = b.x - mgx, iy0 = a.y + mgy, iy1 = b.y - mgy;
        if (ix1 < ix0) ix0 = ix1 = (a.x + b.x) / 2;
        if (iy1 < iy0) iy0 = iy1 = (a.y + b.y) / 2;
        const int col = (an - 1) % 3, row = an >= 7 ? 0 : an >= 4 ? 1 : 2;
        const ImVec2 ap(col == 0 ? ix0 : col == 1 ? (ix0 + ix1) / 2 : ix1, row == 0 ? iy0 : row == 1 ? (iy0 + iy1) / 2 : iy1);
        dl->AddCircleFilled(ap, 3.5f, IM_COL32(0, 0, 0, 200));
        dl->AddCircleFilled(ap, 2.5f, is_sel ? IM_COL32(255, 200, 0, 255) : IM_COL32(255, 255, 255, 200));
    }

    // Dividers: hover, drag, snap.
    static bool dragging = false;
    static Divider drag;
    const std::vector<Divider> divs = dividers(root);
    int hov = -1;
    float best = 6.0f;
    for (size_t i = 0; i < divs.size() && !dragging; ++i) {
        const Divider& d = divs[i];
        const bool cols = d.dir == kColumns;
        const double along = cols ? my : mx;
        if (along < d.lo || along > d.hi) continue;
        const float dist = cols ? std::fabs(io.MousePos.x - sx(d.pos)) : std::fabs(io.MousePos.y - sy(d.pos));
        if (dist < best) { best = dist; hov = (int)i; }
    }
    if (hovered && hov >= 0)
        ImGui::SetMouseCursor(divs[hov].dir == kColumns ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (hov >= 0) {
            dragging = true;
            drag = divs[hov];
        } else {
            const int b = leaf_at_point(root, mx, my);
            if (b >= 0 && b != sel) select_box(b);
        }
    }
    if (dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging = false;
    if (dragging) {
        const bool cols = drag.dir == kColumns;
        ImGui::SetMouseCursor(cols ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        double target = cols ? mx : my;
        bool snapped = false;
        if (!io.KeyAlt) {
            const double tol = 7.0 / (cols ? tw : th);
            double best_d = tol, snap = target;
            for (int d : {2, 3, 4, 5, 6, 8, 10, 12})
                for (int k = 1; k < d; ++k) {
                    const double f = (double)k / d;
                    if (std::fabs(f - target) < best_d - 1e-12) { best_d = std::fabs(f - target); snap = f; }
                }
            for (const Divider& o : divs)   // other box edges in the same direction
                if (o.dir == drag.dir && !(o.path == drag.path && o.index == drag.index) &&
                    std::fabs(o.pos - target) < best_d - 1e-12) {
                    best_d = std::fabs(o.pos - target);
                    snap = o.pos;
                }
            if (snap != target) { target = snap; snapped = true; }
        }
        commit_layout_buf();
        move_divider(root, drag, target);
        // Where it ended up (the move clamps to the neighbours' minimum size).
        double now = target;
        for (const Divider& o : dividers(root))
            if (o.path == drag.path && o.index == drag.index) now = o.pos;
        if (snapped && std::fabs(now - target) < 1e-9) {
            if (cols) dl->AddLine(ImVec2(sx(now), p0.y), ImVec2(sx(now), p1.y), IM_COL32(0, 220, 255, 255), 2.0f);
            else dl->AddLine(ImVec2(p0.x, sy(now)), ImVec2(p1.x, sy(now)), IM_COL32(0, 220, 255, 255), 2.0f);
        }
        ImGui::SetTooltip("%s %s of the frame", cols ? "x =" : "y =", fraction_label(now).c_str());
    } else if (hovered && hov >= 0) {
        const Divider& d = divs[hov];
        if (d.dir == kColumns) dl->AddLine(ImVec2(sx(d.pos), sy(d.lo)), ImVec2(sx(d.pos), sy(d.hi)), IM_COL32(0, 220, 255, 255), 2.0f);
        else dl->AddLine(ImVec2(sx(d.lo), sy(d.pos)), ImVec2(sx(d.hi), sy(d.pos)), IM_COL32(0, 220, 255, 255), 2.0f);
    }
    dl->PopClipRect();

    // Structure: split, equal sizes, delete.
    const int n = leaf_count(root);
    static int parts = 3;
    ImGui::Text("Box %d of %d:", sel + 1, n);
    ImGui::SameLine();
    if (ImGui::Button("Split into columns")) {
        commit_layout_buf();
        split_leaf(root, sel, kColumns, parts);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Splits the selected box into %d equal boxes side by side.", parts);
    ImGui::SameLine();
    if (ImGui::Button("Split into rows")) {
        commit_layout_buf();
        split_leaf(root, sel, kRows, parts);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Splits the selected box into %d equal boxes stacked.", parts);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    if (ImGui::InputInt("parts", &parts)) parts = std::max(2, std::min(8, parts));
    // A new empty box on one side of the selected one (half its space).
    ImGui::TextUnformatted("Add a box:");
    static const char* const k_add_side[4] = {"Left", "Above", "Right", "Below"};
    for (int side : {0, 2, 1, 3}) {
        ImGui::SameLine();
        ImGui::PushID(side);
        if (ImGui::Button(k_add_side[side])) {
            commit_layout_buf();
            const int from = g_ui.selected_box;
            const int added = insert_box(root, from, side);
            if (added >= 0) {
                // The box it was added beside moved one number up when the new
                // box comes first (left / above).
                prev_sel = added == from ? from + 1 : from;
                seen_sel = added;
                g_ui.selected_box = added;
                load_layout_buf();
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Adds an empty box %s the selected box, in half of its space,\nand selects it.",
                              side == 0 ? "left of" : side == 1 ? "above" : side == 2 ? "right of" : "below");
        ImGui::PopID();
    }
    if (n > 1) {
        if (ImGui::Button("Equal sizes")) equalize_parent(root, sel);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Gives the selected box and the boxes it was split with the same size.");
        ImGui::SameLine();
        auto delete_selected = [&]() {
            const int del = g_ui.selected_box;
            const int back = prev_sel >= 0 && prev_sel < leaf_count(root) && prev_sel != del ? prev_sel : -1;
            const int to = remove_leaf(root, del, back);
            if (to >= 0) {
                // Back to the box selected before (renumbered past the removed one).
                g_ui.selected_box = back >= 0 ? (back > del ? back - 1 : back) : to;
                seen_sel = g_ui.selected_box;
                prev_sel = -1;
                load_layout_buf();
            }
        };
        if (ImGui::Button("Delete box")) {
            commit_layout_buf();
            if (g_ui.layout_buf[0]) ImGui::OpenPopup("Delete box?");
            else delete_selected();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Removes the selected box and its text; its space goes to the neighbouring box.");
        if (ImGui::BeginPopupModal("Delete box?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete box %d and its text?", g_ui.selected_box + 1);
            if (ImGui::Button("Delete", ImVec2(120, 0))) {
                delete_selected();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // The selected box: where its text sits and how its lines align.
    if (Node* leaf = leaf_at(root, g_ui.selected_box)) {
        static const char* const k_anchor_names[9] = {"Top left", "Top center", "Top right",
                                                      "Middle left", "Center", "Middle right",
                                                      "Bottom left", "Bottom center", "Bottom right"};
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Text sits at");
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                const int np = (r == 0 ? 7 : r == 1 ? 4 : 1) + c;
                if (c) ImGui::SameLine(0, 2);
                ImGui::PushID(np);
                if (ImGui::RadioButton("##an", leaf->box.anchor == np)) leaf->box.anchor = np;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s of the box", k_anchor_names[r * 3 + c]);
                ImGui::PopID();
            }
        }
        ImGui::EndGroup();
        ImGui::SameLine(0, 24);
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Lines");
        static const char* k_line_align_items[] = {"Follow position", "Left", "Center", "Right"};
        ImGui::SetNextItemWidth(150);
        ImGui::Combo("##linealign", &leaf->box.line_align, k_line_align_items, 4);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How the box's lines align with each other.\n"
                              "Follow position: to the side the text sits at.\n"
                              "Left / Center / Right: that way inside the text block, which keeps the\n"
                              "width of its widest possible line, so e.g. a label stays centred over\n"
                              "its stat while the block stays at its position.");
        ImGui::EndGroup();

        // Exact edges: typing a value moves the line on that edge, like a drag.
        if (n > 1 && ImGui::TreeNode("Exact size (% of the frame)")) {
            const FRect r = leaf_rects(root)[g_ui.selected_box];
            static const char* const k_side[4] = {"Left", "Top", "Right", "Bottom"};
            for (int side : {0, 2, 1, 3}) {
                Divider d;
                const bool movable = edge_divider(root, g_ui.selected_box, side, d);
                double v = 100.0 * (side == 0 ? r.x0 : side == 1 ? r.y0 : side == 2 ? r.x1 : r.y1);
                if (side == 2 || side == 3) ImGui::SameLine(0, 16);
                ImGui::TextUnformatted(k_side[side]);
                ImGui::SameLine(side == 0 || side == 1 ? 60.0f : 0.0f);
                ImGui::SetNextItemWidth(150);
                if (!movable) ImGui::BeginDisabled();
                ImGui::PushID(side);
                if (ImGui::InputDouble("##edge", &v, 0.1, 1.0, "%.3f%%", ImGuiInputTextFlags_EnterReturnsTrue) && movable)
                    move_divider(root, d, v / 100.0);
                ImGui::PopID();
                if (!movable) ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(movable ? "Type a value and press Enter, or use - / + (0.1%%, Ctrl for 1%%).\n"
                                                "Moves the line on this edge, so the boxes on its other side\n"
                                                "change size too."
                                              : "The edge of the frame.");
            }
            ImGui::TextDisabled("Size: %.3f%% x %.3f%% of the frame", 100.0 * (r.x1 - r.x0), 100.0 * (r.y1 - r.y0));
            ImGui::TreePop();
        }
    }
    ImGui::Spacing();
}

// The pattern picker (top bar): choose, save, save as, delete.
void draw_pattern_bar() {
    {
        // Our own saves/deletes refresh the list immediately; the periodic
        // re-scan only picks up files changed outside the program (it used to
        // list the folder every frame).
        if (ImGui::GetTime() - g_pattern_list_time > 2.0) refresh_pattern_list();
        else sync_pattern_index();
        ImGui::Text("Pattern:");
        ImGui::SameLine(120);

        std::vector<const char*> pat_cstrs;
        pat_cstrs.reserve(g_ui.pattern_names.size());
        for (auto& n : g_ui.pattern_names) pat_cstrs.push_back(n.c_str());
        ImGui::SetNextItemWidth(200);
        if (!pat_cstrs.empty() &&
            ImGui::Combo("##pattern", &g_ui.active_pattern_idx, pat_cstrs.data(), (int)pat_cstrs.size())) {
            std::string chosen = g_ui.pattern_names[g_ui.active_pattern_idx];
            if (chosen != g_ui.active_pattern) {
                if (pattern_dirty()) {           // unsaved edits: confirm
                    g_ui.pending_switch_to = chosen;
                    g_ui.show_switch_confirm = true;
                } else {
                    set_active_pattern(chosen);
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Save##pat")) {
            if (g_ui.active_pattern.empty()) {   // never-named new pattern
                g_ui.show_save_as = true;
                g_ui.save_as_warn = false;
                g_ui.save_as_name[0] = '\0';
            } else {
                save_pattern(g_ui.active_pattern, current_pattern_from_ui());
                g_ui.pattern_baseline = current_pattern_from_ui();
                refresh_pattern_list();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Save As...##pat")) {
            g_ui.show_save_as = true;
            g_ui.save_as_warn = false;
            strncpy(g_ui.save_as_name, g_ui.active_pattern.c_str(), sizeof(g_ui.save_as_name) - 1);
            g_ui.save_as_name[sizeof(g_ui.save_as_name) - 1] = '\0';
        }
        if (!g_ui.active_pattern.empty() && g_ui.active_pattern != "Default") {
            ImGui::SameLine();
            if (ImGui::Button("Delete##pat")) {
                delete_pattern(g_ui.active_pattern);
                g_ui.active_pattern.clear();
                refresh_pattern_list();
                if (!g_ui.pattern_names.empty()) set_active_pattern(g_ui.pattern_names[0]);
            }
        }
        if (pattern_dirty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.0f), "(modified)");
        }

        // Save As modal: empty name refused, existing name warns.
        if (g_ui.show_save_as) {
            ImGui::OpenPopup("Save pattern as");
        }
        if (ImGui::BeginPopupModal("Save pattern as", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (!g_ui.show_save_as) ImGui::CloseCurrentPopup();   // closed elsewhere
            ImGui::Text("Pattern name:");
            ImGui::SetNextItemWidth(260);
            bool enter = ImGui::InputText("##patname", g_ui.save_as_name, sizeof(g_ui.save_as_name),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
            std::string clean = sanitize_pattern_name(g_ui.save_as_name);
            bool name_ok = !clean.empty();
            if (!name_ok)
                ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "A name is required.");
            else if (g_ui.save_as_warn)
                ImGui::TextColored(ImVec4(1, 0.75f, 0.2f, 1), "'%s' already exists -- saving will overwrite it.", clean.c_str());
            ImGui::Separator();
            if (ImGui::Button("Save", ImVec2(120, 0)) || (enter && name_ok)) {
                if (name_ok) {
                    bool existed = pattern_exists(clean);
                    if (existed && !g_ui.save_as_warn) {
                        g_ui.save_as_warn = true;    // first press: warn, don't save
                    } else {
                        save_pattern(clean, current_pattern_from_ui());
                        g_ui.active_pattern = clean;
                        g_ui.pattern_baseline = current_pattern_from_ui();
                        refresh_pattern_list();
                        g_ui.show_save_as = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                g_ui.show_save_as = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        } else {
            g_ui.show_save_as = false;
        }

        // Unsaved-changes confirmation when switching patterns.
        if (g_ui.show_switch_confirm) ImGui::OpenPopup("Unsaved changes");
        if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (!g_ui.show_switch_confirm) ImGui::CloseCurrentPopup();
            ImGui::Text("The current pattern has unsaved changes.\nSwitch anyway and discard them?");
            ImGui::Separator();
            if (ImGui::Button("Discard & switch", ImVec2(140, 0))) {
                set_active_pattern(g_ui.pending_switch_to);
                g_ui.show_switch_confirm = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(140, 0))) {
                g_ui.show_switch_confirm = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        } else {
            g_ui.show_switch_confirm = false;
        }
    }

}

// Look column: font, colours, margin, typography, number format.
void draw_look_section() {
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "Look - saved with the pattern");
    ImGui::Spacing();
    // Font family + weight/variant share one row to stay compact.
    ImGui::Text("Font Family");
    if (!g_ui.font_cstrs.empty()) font_list_loaded = true;
    bool fam_changed = false;
    if (!g_ui.font_cstrs.empty()) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 130);
        if (ImGui::Combo("##font", &g_ui.selected_font_idx, g_ui.font_cstrs.data(), (int)g_ui.font_cstrs.size()))
            fam_changed = true;
        if (fam_changed) g_ui.s.pattern.font_family = g_ui.system_fonts[g_ui.selected_font_idx];
    } else {
        char font_buf[256];
        strncpy(font_buf, g_ui.s.pattern.font_family.c_str(), sizeof(font_buf) - 1);
        font_buf[sizeof(font_buf) - 1] = '\0';
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 130);
        if (ImGui::InputText("##font_custom", font_buf, sizeof(font_buf))) {
            g_ui.s.pattern.font_family = font_buf;
            fam_changed = true;
        }
    }

    // Style/weight combo on the same row, populated from the family's files.
    // The cstr array is rebuilt here, right before Combo consumes it, so it
    // can never point at freed std::string heap (dangling c_str() caching
    // made the dropdown show garbage from other allocations).
    if (fam_changed || g_ui.variants_for != g_ui.s.pattern.font_family) {
        g_ui.variants_for = g_ui.s.pattern.font_family;
        g_ui.variant_names.clear();
        for (const auto& v : enumerate_font_variants(g_ui.s.pattern.font_family))
            g_ui.variant_names.push_back(v.style);
        g_ui.variant_idx = 0;
        for (size_t i = 0; i < g_ui.variant_names.size(); ++i)
            if (g_ui.variant_names[i] == g_ui.s.pattern.font_variant) g_ui.variant_idx = (int)i;
    }
    ImGui::SameLine();
    float style_w = 125.0f;
    if (g_ui.variant_names.empty()) {
        ImGui::SetNextItemWidth(style_w);
        ImGui::TextDisabled("(no styles)");
    } else {
        if ((size_t)g_ui.variant_idx >= g_ui.variant_names.size()) g_ui.variant_idx = 0;
        std::vector<const char*> variant_cstrs;
        variant_cstrs.reserve(g_ui.variant_names.size());
        for (const auto& n : g_ui.variant_names) variant_cstrs.push_back(n.c_str());
        ImGui::SetNextItemWidth(style_w);
        if (ImGui::Combo("##fstyle", &g_ui.variant_idx, variant_cstrs.data(), (int)variant_cstrs.size()))
            g_ui.s.pattern.font_variant = g_ui.variant_names[g_ui.variant_idx];
    }

    ImGui::Text("Font Size");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("##fsize", &g_ui.s.pattern.font_size, 1, 10);
    g_ui.s.pattern.font_size = std::max(1, std::min(2000, g_ui.s.pattern.font_size));

    // Colours on one row: swatches (click for the picker with a hex field).
    {
        std::string rgb;
        ImGui::Text("Colours");
        ImGui::SameLine(120);
        if (fmcg_ui::ass_to_rrggbb(g_ui.s.pattern.text_color_aabbggrr, rgb) && fmcg_ui::EditColour("Text##textcol", rgb, true))
            g_ui.s.pattern.text_color_aabbggrr = fmcg_ui::rrggbb_to_ass(rgb);
        ImGui::SameLine(0, 14);
        if (fmcg_ui::ass_to_rrggbb(g_ui.s.pattern.bg_color_aabbggrr, rgb) && fmcg_ui::EditColour("Background##bgcol", rgb, true))
            g_ui.s.pattern.bg_color_aabbggrr = fmcg_ui::rrggbb_to_ass(rgb);
        ImGui::Text("Outline");
        ImGui::SameLine(120);
        ImGui::Checkbox("##ol", &g_ui.s.pattern.outline_enabled);
        ImGui::SameLine();
        if (!g_ui.s.pattern.outline_enabled) ImGui::BeginDisabled();
        if (fmcg_ui::ass_to_rrggbb(g_ui.s.pattern.outline_color_aabbggrr, rgb) && fmcg_ui::EditColour("##olcol", rgb, true))
            g_ui.s.pattern.outline_color_aabbggrr = fmcg_ui::rrggbb_to_ass(rgb);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        float ow = (float)g_ui.s.pattern.outline_width;
        if (ImGui::InputFloat("px##olw", &ow, 0.0f, 0.0f, "%.1f")) {
            if (ow < 0.0f) ow = 0.0f;
            if (ow > 20.0f) ow = 20.0f;
            g_ui.s.pattern.outline_width = ow;
        }
        if (!g_ui.s.pattern.outline_enabled) ImGui::EndDisabled();
    }
    ImGui::Text("Box margin");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("px##margin", &g_ui.s.pattern.margin))
        g_ui.s.pattern.margin = std::max(0, std::min(1000, g_ui.s.pattern.margin));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Space kept between each box's edges and its text (every box).");

    draw_typography_options();
    // Number format: set once per pattern, so collapsed by default.
    if (ImGui::TreeNode("Number format")) {
        ImGui::Text("Comma separators:");
        ImGui::SameLine();
        ImGui::Checkbox("Notes##c", &g_ui.s.pattern.commas.notes);
        ImGui::SameLine();
        ImGui::Checkbox("Poly##c", &g_ui.s.pattern.commas.polyphony);
        ImGui::SameLine();
        ImGui::Checkbox("NPS##c", &g_ui.s.pattern.commas.nps);
        ImGui::SameLine();
        ImGui::Checkbox("Ticks##c", &g_ui.s.pattern.commas.ticks);
        ImGui::SameLine();
        ImGui::Checkbox("CC##c", &g_ui.s.pattern.commas.cc);
        ImGui::SameLine();
        ImGui::Checkbox("Beats##c", &g_ui.s.pattern.commas.beats);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("{beat*} and {measure*} stats");
        ImGui::Checkbox("Leading zeros (pad each stat to its own maximum)", &g_ui.s.pattern.pad.enabled);
        ImGui::Checkbox("Fixed-width stats", &g_ui.s.pattern.cells);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Each stat gets a cell as wide as its widest value in this video\n"
                              "(measured in the chosen font), so text after it and the line's\n"
                              "width stay put while digits change.");
        ImGui::SameLine();
        if (!g_ui.s.pattern.cells) ImGui::BeginDisabled();
        ImGui::Text("align");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        static const char* k_cell_align[] = {"Left", "Center", "Right"};
        ImGui::Combo("##cellalign", &g_ui.s.pattern.cell_align, k_cell_align, 3);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Where a value sits inside its cell (Right lines numbers up by their last digit).");
        if (!g_ui.s.pattern.cells) ImGui::EndDisabled();

        ImGui::Text("BPM decimals:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        int bdec = g_ui.s.pattern.bpm.decimals;
        if (ImGui::InputInt("##bpmdec", &bdec, 0, 0)) {
            if (bdec < 0) bdec = 0;
            if (bdec > 6) bdec = 6;   // beyond 6 is display noise
            g_ui.s.pattern.bpm.decimals = bdec;
        }
        ImGui::TreePop();
    }
}

// Layout column: the boxes and the selected box's text. `fill`: this is its
// own column, so the thumbnail takes the width and the editor the rest of the
// height; otherwise fixed sizes (stacked under the look settings).
void draw_layout_section(bool fill) {
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "Layout - saved with the pattern");
    ImGui::Spacing();
    // Filling: the thumbnail gets what is left after the other controls
    // (measured last frame) and a 120 px text editor; the editor then takes
    // the rest. other_h = everything in this column but those two.
    static float other_h = 300.0f;
    const float col_start = ImGui::GetCursorPosY();
    const float col_avail = ImGui::GetContentRegionAvail().y;
    float thumb_h = 0.0f;
    draw_box_editor(fill ? std::max(60.0f, std::min(col_avail * 0.6f, col_avail - other_h - 120.0f)) : 260.0f, &thumb_h);

    // Layout of the selected box: one line per overlay row (pattern-saved).
    ImGui::Text("Box %d text (one line per overlay row; {nc}, {time-milli}, {bpm}, ...)", g_ui.selected_box + 1);
    // Filling: the editor takes the column's remaining height above the two
    // button rows below it.
    // rows_below: measured on the previous frame (buttons under the editor).
    static float rows_below = 60.0f;
    const float editor_h = fill ? std::max(120.0f, ImGui::GetContentRegionAvail().y - rows_below) : 120.0f;
    draw_layout_editor(editor_h);
    const float below_start = ImGui::GetCursorPosY();
    if (ImGui::Button("Stats list"))
        ImGui::OpenPopup("Valid layout stats");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show every stat token usable in the layout");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    if (ImGui::BeginCombo("##insert_stat", "Insert stat", ImGuiComboFlags_HeightLarge)) {
        const float desc_x = ImGui::CalcTextSize("{time-milli-max}").x + 2.0f * ImGui::GetStyle().ItemSpacing.x;
        for (const auto& st : k_insert_stats) {
            ImGui::PushID(st[0]);
            if (ImGui::Selectable(st[0])) insert_layout_text(st[0]);
            ImGui::SameLine(desc_x);
            ImGui::TextDisabled("%s", st[1]);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Inserts a stat token at the cursor in the selected box's text.");
    if (ImGui::BeginPopupModal("Valid layout stats", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static const char* const k_stats[] = {
            "Notes",  "{nc} / {nc-total} / {nc-rem}",
            "CC",     "{cc} / {cc-total} / {cc-rem}",
            "CC per second", "{cps} / {cps-max}",
            "Time",   "{time} / {time-max} / {time-rem}",
            "Time (ms)", "{time-milli} / {time-milli-max} / {time-milli-rem}",
            "Seconds", "{sec} / {sec-max} / {sec-rem}",
            "NPS",     "{nps} / {nps-max}",
            "Ticks",   "{tick} / {tick-total} / {tick-rem}",
            "Polyphony", "{plph} / {plph-max}",
            "BPM",     "{bpm}",
            "Beats",   "{beat} / {beat-total} / {beat-rem}",
            "Measures", "{measure} / {measure-total} / {measure-rem}",
            "Beat in measure", "{measure-beat}",
            "Time signature", "{timesig}",
            "PPQN",    "{ppqn}",
        };
        ImGui::TextDisabled("Usable tokens (current / total / remaining where shown):");
        ImGui::Spacing();
        if (ImGui::BeginTable("##stats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            for (const char* const* row = k_stats; row < k_stats + sizeof(k_stats) / sizeof(k_stats[0]); row += 2) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(row[0]);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(row[1]);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    {
        bool cfg_dialog_active = g_app.dialog_busy.load();
        if (cfg_dialog_active) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
        if (ImGui::Button("Load from file...##cfg") && !cfg_dialog_active) {
            std::string cfg_cmd;
#if defined(_WIN32)
            cfg_cmd = "powershell -NoProfile -Command \""
                  "[Console]::OutputEncoding = [Text.Encoding]::UTF8; "   // UTF-8 path on stdout
                  "Add-Type -AssemblyName System.Windows.Forms; "
                  "$f = New-Object System.Windows.Forms.OpenFileDialog; "
                  "$f.Filter = 'Text files (*.txt)|*.txt|All files (*.*)|*.*'; "
                  "$f.Title = 'Select layout template'; "
                  "if ($f.ShowDialog() -eq 'OK') { $f.FileName }\"";
#else
            cfg_cmd = "zenity --file-selection --title='Select layout template' 2>/dev/null || "
                  "kdialog --getopenfilename . 'Text files (*.txt)' 'Select layout template' 2>/dev/null";
#endif
            launch_dialog(cfg_cmd, "layout");
        }
        if (cfg_dialog_active) ImGui::PopStyleVar();
        ImGui::SameLine();
        if (ImGui::Button("Reset to default##cfg")) {
            strncpy(g_ui.layout_buf, k_default_layout, sizeof(g_ui.layout_buf) - 1);
        }
    }

    // Poll layout dialog result
    {
        std::string result = poll_dialog_result("layout");
        if (!result.empty()) {
            std::ifstream lf(path_from_utf8(result));
            std::string content, line;
            while (std::getline(lf, line)) {
                content += line;
                content += '\n';
            }
            strncpy(g_ui.layout_buf, content.c_str(), sizeof(g_ui.layout_buf) - 1);
        }
    }

    rows_below = ImGui::GetCursorPosY() - below_start;
    other_h = ImGui::GetCursorPosY() - col_start - thumb_h - editor_h;
}

// Global section: session-wide settings, autosaved, not in patterns (left column).
