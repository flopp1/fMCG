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
// the editor loses focus, so "Insert position" knows where the user was.
static int g_layout_cursor = -1;

// Inserts `line` as its own line, above the line the cursor was on (at the
// end when the editor was never used).
static void insert_layout_line(const std::string& line) {
    std::string buf = g_ui.layout_buf;
    size_t at = (g_layout_cursor < 0 || (size_t)g_layout_cursor > buf.size()) ? buf.size() : (size_t)g_layout_cursor;
    while (at > 0 && buf[at - 1] != '\n') --at;   // start of that line
    std::string ins = line + "\n";
    if (at == buf.size() && !buf.empty() && buf.back() != '\n') ins = "\n" + line;   // a new last line
    if (buf.size() + ins.size() >= sizeof(g_ui.layout_buf)) return;   // no room
    buf.insert(at, ins);
    std::memcpy(g_ui.layout_buf, buf.c_str(), buf.size() + 1);
    g_layout_cursor = (int)(at + ins.size());
}

static void draw_layout_editor() {
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
    ImGui::BeginChild("##layout_area", ImVec2(-1, 120), ImGuiChildFlags_Borders,
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
void draw_pattern_section() {
    // --- Pattern section: everything here is saved under the selected pattern
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f),
        "Pattern - the overlay look, saved with the selected pattern");
    ImGui::Spacing();
    {
        // Our own saves/deletes refresh the list immediately; the periodic
        // re-scan only picks up files changed outside the program (it used to
        // list the folder every frame).
        if (ImGui::GetTime() - g_pattern_list_time > 2.0) refresh_pattern_list();
        else sync_pattern_index();
        ImGui::Text("Active:");
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

    draw_typography_options();

    // Layout: one line per overlay row (pattern-saved).
    ImGui::Text("Layout (one line per overlay row; {nc}, {time-milli}, {bpm}, ...)");
    draw_layout_editor();
    if (ImGui::Button("Stats list"))
        ImGui::OpenPopup("Valid layout stats");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show every stat token usable in the layout");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("##insert_pos", "Insert position")) {
        for (int i = 0; i < 9; ++i)
            if (ImGui::Selectable(k_position_names[i]))
                insert_layout_line(std::string("[") + k_position_names[i] + "]");
        ImGui::Separator();
        if (ImGui::Selectable("default (the placement below)")) insert_layout_line("[default]");
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Adds a position line such as [bottom-right] above the line the cursor is on.\n"
                          "The lines below it, up to the next position line, are drawn at that\n"
                          "point of the frame (at the margin); position lines themselves are not shown.\n"
                          "Lines before the first one use the placement settings below;\n"
                          "[default] returns to them.");
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
        ImGui::TextDisabled("Position lines: [top-left] [top-center] [top-right] [center-left] [center]");
        ImGui::TextDisabled("[center-right] [bottom-left] [bottom-center] [bottom-right] [default]");
        ImGui::TextDisabled("send the lines below them to that point of the frame.");
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

    // --- Colours: swatch + RGB-slider popup, live preview (pattern-saved) --
    {
        std::string rgb;
        if (fmcg_ui::ass_to_rrggbb(g_ui.s.pattern.text_color_aabbggrr, rgb)) {
            ImGui::Text("Text Colour");
            ImGui::SameLine(120);
            if (fmcg_ui::EditColour("##textcol", rgb))
                g_ui.s.pattern.text_color_aabbggrr = fmcg_ui::rrggbb_to_ass(rgb);
        }
        if (fmcg_ui::ass_to_rrggbb(g_ui.s.pattern.bg_color_aabbggrr, rgb)) {
            ImGui::Text("Background");
            ImGui::SameLine(120);
            if (fmcg_ui::EditColour("##bgcol", rgb))
                g_ui.s.pattern.bg_color_aabbggrr = fmcg_ui::rrggbb_to_ass(rgb);
        }
        // Text outline (pattern setting; off by default).
        ImGui::Checkbox("Outline##ol", &g_ui.s.pattern.outline_enabled);
        ImGui::SameLine(120);
        if (!g_ui.s.pattern.outline_enabled) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(90);
        float ow = (float)g_ui.s.pattern.outline_width;
        if (ImGui::InputFloat("px##olw", &ow, 0.0f, 0.0f, "%.1f")) {
            if (ow < 0.0f) ow = 0.0f;
            if (ow > 20.0f) ow = 20.0f;
            g_ui.s.pattern.outline_width = ow;
        }
        ImGui::SameLine();
        if (fmcg_ui::ass_to_rrggbb(g_ui.s.pattern.outline_color_aabbggrr, rgb) && fmcg_ui::EditColour("##olcol", rgb))
            g_ui.s.pattern.outline_color_aabbggrr = fmcg_ui::rrggbb_to_ass(rgb);
        if (!g_ui.s.pattern.outline_enabled) ImGui::EndDisabled();
    }

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

    ImGui::Text("Counter position:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Corners##pm", g_ui.s.pattern.pos_mode == 0)) g_ui.s.pattern.pos_mode = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Custom x,y##pm", g_ui.s.pattern.pos_mode == 1)) g_ui.s.pattern.pos_mode = 1;
    if (g_ui.s.pattern.pos_mode == 0) {
        ImGui::SetNextItemWidth(160);
        if (ImGui::Combo("##align", &g_ui.alignment_idx, k_alignment_items, k_alignment_count)) {
            g_ui.s.pattern.alignment = g_ui.alignment_idx;
        }
    } else {
        // Custom position: x,y measured from a point of the frame, and the
        // block's own anchor point placed there (fmcg_text::Placement).
        static const char* k_anchor_items[] = {"Top Left", "Top Center", "Top Right",
                                               "Middle Left", "Center", "Middle Right",
                                               "Bottom Left", "Bottom Center", "Bottom Right"};
        ImGui::Text("Measured from");
        ImGui::SameLine(130);
        ImGui::SetNextItemWidth(150);
        ImGui::Combo("##fanchor", &g_ui.s.pattern.frame_anchor, k_anchor_items, 9);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The point of the FRAME that x,y count from.\n"
                              "From an edge, x,y count inward (from the right edge +x moves left,\n"
                              "from the bottom +y moves up); from a centre line, +x is right, +y down.\n"
                              "Keeps the counter in place when the resolution changes.");
        ImGui::SameLine();
        ImGui::Text("Block anchor");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::Combo("##banchor", &g_ui.s.pattern.block_anchor, k_anchor_items, 9);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The point of the TEXT BLOCK that sits at the position.\n"
                              "Lines align to its side (left, centre or right).");
        // Ranges: 0..size from an edge, -size/2..size/2 from a centre line.
        const bool cx = g_ui.s.pattern.frame_anchor % 3 == 1, cy = g_ui.s.pattern.frame_anchor / 3 == 1;
        const int min_x = cx ? -g_ui.s.global.width / 2 : 0, max_x = cx ? g_ui.s.global.width / 2 : g_ui.s.global.width;
        const int min_y = cy ? -g_ui.s.global.height / 2 : 0, max_y = cy ? g_ui.s.global.height / 2 : g_ui.s.global.height;
        ImGui::Text("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        int xin = g_ui.s.pattern.pos_x;
        if (ImGui::InputInt("##posx", &xin, 0, 0)) g_ui.s.pattern.pos_x = std::max(min_x, std::min(max_x, xin));
        ImGui::SameLine();
        ImGui::Text("y");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        int yin = g_ui.s.pattern.pos_y;
        if (ImGui::InputInt("##posy", &yin, 0, 0)) g_ui.s.pattern.pos_y = std::max(min_y, std::min(max_y, yin));
        ImGui::SameLine();
        ImGui::TextDisabled("x: %d..%d  y: %d..%d  (the block always stays fully on screen)", min_x, max_x, min_y, max_y);
    }
}

// Global section: session-wide settings, autosaved, not in patterns (left column).
