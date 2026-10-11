// main.cpp -- fMCG GUI entry point: GLFW/OpenGL3 bootstrap, the settings UI,
// pattern manager, colour editors, action buttons, progress bar and log view.
// Worker operations live in jobs.cpp, the preview popup in preview.cpp,
// dialogs in dialogs.cpp, shared state in app_state.h/cpp, persistence in
// settings_store.cpp.
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

static const char* k_alignment_items[] = {
    "Top Left", "Top Right", "Bottom Left", "Bottom Right", "Top Center", "Bottom Center"
};
static const int k_alignment_count = 6;

static const char* k_default_layout =
    "Time: {time-milli}/{time-milli-max}\n"
    "Notes: {nc}/{nc-total}/{nc-rem}\n"
    "NPS: {nps}/{nps-max}\n"
    "Polyphony: {plph}/{plph-max}\n"
    "BPM: {bpm}";

// --- persistence glue -------------------------------------------------------

static void save_globals_now() {
    // Pull the live UI values into the store first: the checkboxes/fields bind
    // to g_ui.s, and without this sync every save would re-write the values
    // captured at startup (silently reverting CC/vel-0/delay across restarts).
    g_ui.globals.width = g_ui.s.width;
    g_ui.globals.height = g_ui.s.height;
    g_ui.globals.fps = g_ui.s.fps;
    g_ui.globals.vel0_note_off = g_ui.s.vel0_note_off;
    g_ui.globals.close_held_notes = g_ui.s.close_held_notes;
    g_ui.globals.start_delay = g_ui.s.start_delay;
    g_ui.globals.end_delay = g_ui.s.end_delay;
    g_ui.globals.ffmpeg_threads = g_ui.s.ffmpeg_threads;
    g_ui.globals.parse_threads  = g_ui.s.parse_threads;
    g_ui.globals.track_dedup    = g_ui.s.track_dedup;
    g_ui.globals.output_format  = g_ui.s.output_format;
    g_ui.globals.transparent_bg = g_ui.s.transparent_bg;
    g_ui.globals.x264_crf       = g_ui.s.x264_crf;
    g_ui.globals.x264_preset    = g_ui.s.x264_preset;
    g_ui.globals.x264_tune      = g_ui.s.x264_tune;
    g_ui.globals.keyframe_seconds = g_ui.s.keyframe_seconds;
    g_ui.globals_dirty = false;
    g_ui.last_globals_save = ImGui::GetTime();
    save_global_settings(g_ui.globals);
}

// Globals autosave: debounced to at most one write/second while dragging.
static void maybe_autosave_globals() {
    if (!g_ui.globals_dirty) return;
    double now = ImGui::GetTime();
    if (now - g_ui.last_globals_save >= 1.0) save_globals_now();
}

static void mark_globals_dirty() { g_ui.globals_dirty = true; }

// Copy the current pattern-defining fields into a PatternData snapshot.
static PatternData current_pattern_from_ui() {
    PatternData p;
    p.layout_text = g_ui.layout_buf;
    p.alignment = g_ui.s.alignment;
    p.pos_mode = g_ui.s.pos_mode;
    p.pos_x = g_ui.s.pos_x;
    p.pos_y = g_ui.s.pos_y;
    p.frame_anchor = g_ui.s.frame_anchor;
    p.block_anchor = g_ui.s.block_anchor;
    p.cells = g_ui.s.cells;
    p.cell_align = g_ui.s.cell_align;
    p.font_family = g_ui.s.font_family;
    p.font_variant = g_ui.s.font_variant;
    p.font_size = g_ui.s.font_size;
    p.text_color_aabbggrr = g_ui.s.text_color_aabbggrr;
    p.bg_color_aabbggrr = g_ui.s.bg_color_aabbggrr;
    p.outline_enabled = g_ui.s.outline_enabled;
    p.outline_width = g_ui.s.outline_width;
    p.outline_color_aabbggrr = g_ui.s.outline_color_aabbggrr;
    p.typo = g_ui.s.typo;
    p.commas = g_ui.s.commas;
    p.pad = g_ui.s.pad;
    p.bpm = g_ui.s.bpm;
    return p;
}

static void apply_pattern_to_ui(const PatternData& p) {
    strncpy(g_ui.layout_buf, p.layout_text.c_str(), sizeof(g_ui.layout_buf) - 1);
    g_ui.layout_buf[sizeof(g_ui.layout_buf) - 1] = '\0';
    g_ui.s.alignment = p.alignment;
    if (g_ui.s.alignment < 0 || g_ui.s.alignment >= k_alignment_count) g_ui.s.alignment = 0;
    g_ui.alignment_idx = g_ui.s.alignment;
    g_ui.s.pos_mode = p.pos_mode;
    g_ui.s.pos_x = p.pos_x;
    g_ui.s.pos_y = p.pos_y;
    g_ui.s.frame_anchor = p.frame_anchor;
    g_ui.s.block_anchor = p.block_anchor;
    g_ui.s.cells = p.cells;
    g_ui.s.cell_align = p.cell_align;
    g_ui.s.font_size = p.font_size;
    g_ui.s.text_color_aabbggrr = p.text_color_aabbggrr;
    g_ui.s.bg_color_aabbggrr = p.bg_color_aabbggrr;
    g_ui.s.outline_enabled = p.outline_enabled;
    g_ui.s.outline_width = p.outline_width;
    g_ui.s.outline_color_aabbggrr = p.outline_color_aabbggrr;
    g_ui.s.typo = p.typo;
    g_ui.s.commas = p.commas;
    g_ui.s.pad = p.pad;
    g_ui.s.bpm = p.bpm;
    // Family: prefer an exact match in the enumerated list; else keep the name
    // (custom-input fallback path still renders it if the file exists).
    apply_font_variant(g_ui.s, p.font_family, p.font_variant);
    int idx = -1;
    for (size_t i = 0; i < g_ui.system_fonts.size(); ++i)
        if (g_ui.system_fonts[i] == p.font_family) { idx = (int)i; break; }
    if (idx >= 0) g_ui.selected_font_idx = idx;
}

static bool pattern_dirty() {
    return !(current_pattern_from_ui() == g_ui.pattern_baseline);
}

static void set_active_pattern(const std::string& name) {
    g_ui.active_pattern = name;
    PatternData p;
    if (load_pattern(name, p)) {
        apply_pattern_to_ui(p);
        g_ui.pattern_baseline = p;
    }
}

static void refresh_pattern_list() {
    g_ui.pattern_names = list_patterns();
    // keep the combo index pointing at the active pattern
    g_ui.active_pattern_idx = 0;
    for (size_t i = 0; i < g_ui.pattern_names.size(); ++i)
        if (g_ui.pattern_names[i] == g_ui.active_pattern) g_ui.active_pattern_idx = (int)i;
}

static bool font_list_loaded = false;   // font-list lazy-init (set at startup)

// Advanced typography (pattern-saved): OpenType features and spacing for the
// in-built renderer. Options the selected font lacks are listed as such
// (HarfBuzz ignores them, so they are harmless but do nothing).
static void draw_typography_options() {
    if (!ImGui::TreeNode("Advanced typography")) return;
    fmcg_text::Typography& t = g_ui.s.typo;
    const float k_ty_label = 150.0f;   // label column inside the indented tree

    // Features of the selected font, re-read only when the family/style changes.
    static std::string feats_for;
    static std::vector<fmcg_text::FontFeature> feats;
    const std::string fkey = g_ui.s.font_family + "|" + g_ui.s.font_variant;
    if (fkey != feats_for) {
        feats_for = fkey;
        int face = 0;
        const std::string file = find_font_file_for_variant(g_ui.s.font_family, g_ui.s.font_variant, &face);
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

// Pattern section: the overlay look, saved with the selected pattern (right column).
static void draw_pattern_section() {
    // --- Pattern section: everything here is saved under the selected pattern
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f),
        "Pattern - the overlay look, saved with the selected pattern");
    ImGui::Spacing();
    {
        refresh_pattern_list();
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
        if (fam_changed) g_ui.s.font_family = g_ui.system_fonts[g_ui.selected_font_idx];
    } else {
        char font_buf[256];
        strncpy(font_buf, g_ui.s.font_family.c_str(), sizeof(font_buf) - 1);
        font_buf[sizeof(font_buf) - 1] = '\0';
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 130);
        if (ImGui::InputText("##font_custom", font_buf, sizeof(font_buf))) {
            g_ui.s.font_family = font_buf;
            fam_changed = true;
        }
    }

    // Style/weight combo on the same row, populated from the family's files.
    // The cstr array is rebuilt here, right before Combo consumes it, so it
    // can never point at freed std::string heap (dangling c_str() caching
    // made the dropdown show garbage from other allocations).
    if (fam_changed || g_ui.variants_for != g_ui.s.font_family) {
        g_ui.variants_for = g_ui.s.font_family;
        g_ui.variant_names.clear();
        for (const auto& v : enumerate_font_variants(g_ui.s.font_family))
            g_ui.variant_names.push_back(v.style);
        g_ui.variant_idx = 0;
        for (size_t i = 0; i < g_ui.variant_names.size(); ++i)
            if (g_ui.variant_names[i] == g_ui.s.font_variant) g_ui.variant_idx = (int)i;
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
            apply_font_variant(g_ui.s, g_ui.s.font_family, g_ui.variant_names[g_ui.variant_idx]);
    }

    ImGui::Text("Font Size");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("##fsize", &g_ui.s.font_size, 1, 10);
    g_ui.s.font_size = std::max(1, std::min(2000, g_ui.s.font_size));

    draw_typography_options();

    // Layout: one line per overlay row (pattern-saved).
    ImGui::Text("Layout (one line per overlay row; {nc}, {time-milli}, {bpm}, ...)");
    ImGui::InputTextMultiline("##layout", g_ui.layout_buf, sizeof(g_ui.layout_buf), ImVec2(-1, 120));
    if (ImGui::Button("Stats list"))
        ImGui::OpenPopup("Valid layout stats");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show every stat token usable in the layout");
    if (ImGui::BeginPopupModal("Valid layout stats", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static const char* const k_stats[] = {
            "Notes",  "{nc} / {nc-total} / {nc-rem}",
            "CC",     "{cc} / {cc-total} / {cc-rem}",
            "Time",   "{time} / {time-max} / {time-rem}",
            "Time (ms)", "{time-milli} / {time-milli-max} / {time-milli-rem}",
            "Seconds", "{sec} / {sec-max} / {sec-rem}",
            "NPS",     "{nps} / {nps-max}",
            "Ticks",   "{tick} / {tick-total} / {tick-rem}",
            "Polyphony", "{plph} / {plph-max}",
            "BPM",     "{bpm}",
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
                  "Add-Type -AssemblyName System.Windows.Forms; "
                  "$f = New-Object System.Windows.Forms.OpenFileDialog; "
                  "$f.Filter = 'Text files (*.txt)|*.txt|All files (*.*)|*.*'; "
                  "$f.Title = 'Select layout template'; "
                  "if ($f.ShowDialog() -eq 'OK') { $f.FileName }\"";
#elif defined(__APPLE__)
            cfg_cmd = "osascript -e 'tell application \"System Events\" to set f to (choose file with prompt \"Select layout template\") as alias' -e 'POSIX path of f'";
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
            std::ifstream lf(result.c_str());
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
        if (fmcg_ui::ass_to_rrggbb(g_ui.s.text_color_aabbggrr, rgb)) {
            ImGui::Text("Text Colour");
            ImGui::SameLine(120);
            if (fmcg_ui::EditColour("##textcol", rgb))
                g_ui.s.text_color_aabbggrr = fmcg_ui::rrggbb_to_ass(rgb);
        }
        if (fmcg_ui::ass_to_rrggbb(g_ui.s.bg_color_aabbggrr, rgb)) {
            ImGui::Text("Background");
            ImGui::SameLine(120);
            if (fmcg_ui::EditColour("##bgcol", rgb))
                g_ui.s.bg_color_aabbggrr = fmcg_ui::rrggbb_to_ass(rgb);
        }
        // Text outline (pattern setting; off by default).
        ImGui::Checkbox("Outline##ol", &g_ui.s.outline_enabled);
        ImGui::SameLine(120);
        if (!g_ui.s.outline_enabled) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(90);
        float ow = (float)g_ui.s.outline_width;
        if (ImGui::InputFloat("px##olw", &ow, 0.0f, 0.0f, "%.1f")) {
            if (ow < 0.0f) ow = 0.0f;
            if (ow > 20.0f) ow = 20.0f;
            g_ui.s.outline_width = ow;
        }
        ImGui::SameLine();
        if (fmcg_ui::ass_to_rrggbb(g_ui.s.outline_color_aabbggrr, rgb) && fmcg_ui::EditColour("##olcol", rgb))
            g_ui.s.outline_color_aabbggrr = fmcg_ui::rrggbb_to_ass(rgb);
        if (!g_ui.s.outline_enabled) ImGui::EndDisabled();
    }

    ImGui::Text("Comma separators:");
    ImGui::SameLine();
    ImGui::Checkbox("Notes##c", &g_ui.s.commas.notes);
    ImGui::SameLine();
    ImGui::Checkbox("Poly##c", &g_ui.s.commas.polyphony);
    ImGui::SameLine();
    ImGui::Checkbox("NPS##c", &g_ui.s.commas.nps);
    ImGui::SameLine();
    ImGui::Checkbox("Ticks##c", &g_ui.s.commas.ticks);
    ImGui::SameLine();
    ImGui::Checkbox("CC##c", &g_ui.s.commas.cc);
    ImGui::Checkbox("Leading zeros (pad each stat to its own maximum)", &g_ui.s.pad.enabled);
    ImGui::Checkbox("Fixed-width stats", &g_ui.s.cells);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Each stat gets a cell as wide as its widest value in this video\n"
                          "(measured in the chosen font), so text after it and the line's\n"
                          "width stay put while digits change.");
    ImGui::SameLine();
    if (!g_ui.s.cells) ImGui::BeginDisabled();
    ImGui::Text("align");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    static const char* k_cell_align[] = {"Left", "Center", "Right"};
    ImGui::Combo("##cellalign", &g_ui.s.cell_align, k_cell_align, 3);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Where a value sits inside its cell (Right lines numbers up by their last digit).");
    if (!g_ui.s.cells) ImGui::EndDisabled();

    ImGui::Text("BPM decimals:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    int bdec = g_ui.s.bpm.decimals;
    if (ImGui::InputInt("##bpmdec", &bdec, 0, 0)) {
        if (bdec < 0) bdec = 0;
        if (bdec > 6) bdec = 6;   // beyond 6 is display noise
        g_ui.s.bpm.decimals = bdec;
    }

    ImGui::Text("Counter position:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Corners##pm", g_ui.s.pos_mode == 0)) g_ui.s.pos_mode = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Custom x,y##pm", g_ui.s.pos_mode == 1)) g_ui.s.pos_mode = 1;
    if (g_ui.s.pos_mode == 0) {
        ImGui::SetNextItemWidth(160);
        if (ImGui::Combo("##align", &g_ui.alignment_idx, k_alignment_items, k_alignment_count)) {
            g_ui.s.alignment = g_ui.alignment_idx;
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
        ImGui::Combo("##fanchor", &g_ui.s.frame_anchor, k_anchor_items, 9);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The point of the FRAME that x,y count from.\n"
                              "From an edge, x,y count inward (from the right edge +x moves left,\n"
                              "from the bottom +y moves up); from a centre line, +x is right, +y down.\n"
                              "Keeps the counter in place when the resolution changes.");
        ImGui::SameLine();
        ImGui::Text("Block anchor");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::Combo("##banchor", &g_ui.s.block_anchor, k_anchor_items, 9);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The point of the TEXT BLOCK that sits at the position.\n"
                              "Lines align to its side (left, centre or right).");
        // Ranges: 0..size from an edge, -size/2..size/2 from a centre line.
        const bool cx = g_ui.s.frame_anchor % 3 == 1, cy = g_ui.s.frame_anchor / 3 == 1;
        const int min_x = cx ? -g_ui.s.width / 2 : 0, max_x = cx ? g_ui.s.width / 2 : g_ui.s.width;
        const int min_y = cy ? -g_ui.s.height / 2 : 0, max_y = cy ? g_ui.s.height / 2 : g_ui.s.height;
        ImGui::Text("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        int xin = g_ui.s.pos_x;
        if (ImGui::InputInt("##posx", &xin, 0, 0)) g_ui.s.pos_x = std::max(min_x, std::min(max_x, xin));
        ImGui::SameLine();
        ImGui::Text("y");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        int yin = g_ui.s.pos_y;
        if (ImGui::InputInt("##posy", &yin, 0, 0)) g_ui.s.pos_y = std::max(min_y, std::min(max_y, yin));
        ImGui::SameLine();
        ImGui::TextDisabled("x: %d..%d  y: %d..%d  (the block always stays fully on screen)", min_x, max_x, min_y, max_y);
    }
}

// Global section: session-wide settings, autosaved, not in patterns (left column).
static void draw_global_section() {
    // --- Global section: session-wide settings, autosaved, NOT in patterns --
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f),
        "Global settings - autosaved, separate from patterns");
    ImGui::Spacing();

    ImGui::Text("Start delay (seconds): ");
    ImGui::SameLine(170);
    ImGui::SetNextItemWidth(100);
    double delay_input = g_ui.s.start_delay;
    if (ImGui::InputDouble("##delay", &delay_input, 0, 0, "%.2f")) {
        if (delay_input >= 0) { g_ui.s.start_delay = delay_input; mark_globals_dirty(); }
    }

    ImGui::Text("End delay (seconds): ");
    ImGui::SameLine(170);
    ImGui::SetNextItemWidth(100);
    double edelay_input = g_ui.s.end_delay;
    if (ImGui::InputDouble("##edelay", &edelay_input, 0, 0, "%.2f")) {
        if (edelay_input >= 0) { g_ui.s.end_delay = edelay_input; mark_globals_dirty(); }
    }

    if (ImGui::Checkbox("Vel-0 as Note-Off", &g_ui.s.vel0_note_off)) mark_globals_dirty();
    ImGui::SameLine();
    if (ImGui::Checkbox("Close held notes", &g_ui.s.close_held_notes)) mark_globals_dirty();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Release notes that are still held when their track ends.\n"
                          "Off (default): polyphony is note-ons minus note-offs, as the MIDI spec defines it.\n"
                          "Affects polyphony only.");

    ImGui::Text("Resolution");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(100);
    if (ImGui::InputInt("##w", &g_ui.s.width, 0, 0)) { mark_globals_dirty(); }
    ImGui::SameLine();
    ImGui::Text("x");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (ImGui::InputInt("##h", &g_ui.s.height, 0, 0)) { mark_globals_dirty(); }
    // Encoder limits: 16..16384 px; H.264 (4:2:0) also needs even sizes.
    g_ui.s.width = std::max(16, std::min(16384, g_ui.s.width));
    g_ui.s.height = std::max(16, std::min(16384, g_ui.s.height));
    if (g_ui.s.output_format == 0 && ((g_ui.s.width | g_ui.s.height) & 1)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "MP4 needs an even width and height");
    }

    ImGui::Text("FPS");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(120);
    double fps_input = g_ui.s.fps;
    if (ImGui::InputDouble("##fps", &fps_input, 0, 0, "%.1f")) {
        if (fps_input > 0) { g_ui.s.fps = fps_input; mark_globals_dirty(); }
    }

    ImGui::Text("Parsing threads");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(120);
    int par_input = g_ui.s.parse_threads;
    if (ImGui::InputInt("##parthreads", &par_input, 0, 0)) {
        // 0 = all cores; 1 = sequential (historic behaviour); negative makes no sense.
        if (par_input < 0) par_input = 0;
        if (par_input > 64) par_input = 64;
        if (par_input != g_ui.s.parse_threads) {
            g_ui.s.parse_threads = par_input;
            mark_globals_dirty();
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Worker threads for parsing plain (uncompressed) MIDI files.\n0 = all cores (default). 1 = sequential.\nCompressed inputs and the two-pass fallback always parse sequentially.");

    ImGui::Checkbox("Dedup", &g_ui.s.track_dedup);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Skip re-processing repeated content.\nPlain MIDI: byte-identical tracks are parsed once and replayed.\nCompressed/layered files: only unique members are decoded and parsed,\nduplicates are counted by multiplicity, and uniform spam grids fold\nanalytically. Off by default -- enable for dedup-style constructions." );

    ImGui::Text("Encoder threads");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(120);
    int thr_input = g_ui.s.ffmpeg_threads;
    if (ImGui::InputInt("##threads", &thr_input, 0, 0)) {
        // 0 = auto (x264 picks); negative makes no sense.
        if (thr_input < 0) thr_input = 0;
        if (thr_input > 64) thr_input = 64;
        if (thr_input != g_ui.s.ffmpeg_threads) {
            g_ui.s.ffmpeg_threads = thr_input;
            mark_globals_dirty();
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Video encoder threads (H.264). 0 = auto (recommended).\nApplies at render time.");

    // Output format (global). Switching it swaps the output file extension.
    ImGui::Text("Format");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(300);
    {
        static const char* k_formats[] = {"MP4 (H.264)", "MOV (QuickTime Animation, lossless)"};
        int fmt = g_ui.s.output_format;
        if (ImGui::Combo("##format", &fmt, k_formats, 2) && fmt != g_ui.s.output_format) {
            g_ui.s.output_format = fmt;
            std::string out = g_ui.output_buf;
            const std::string want = fmcg_video::extension((fmcg_video::Format)fmt);
            const size_t dot = out.find_last_of('.');
            if (dot != std::string::npos && (out.substr(dot) == ".mp4" || out.substr(dot) == ".mov")) {
                out = out.substr(0, dot) + want;
                strncpy(g_ui.output_buf, out.c_str(), sizeof(g_ui.output_buf) - 1);
                g_ui.output_buf[sizeof(g_ui.output_buf) - 1] = '\0';
                g_ui.s.output_video = out;
            }
            mark_globals_dirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("MP4: H.264, plays everywhere (lossy).\n"
                              "MOV: QuickTime Animation, lossless and fast, for video editors;\n"
                              "supports a transparent background. Not playable in browsers.");
        ImGui::SameLine();
        const bool mov = g_ui.s.output_format == 1;
        if (!mov) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Transparent background", &g_ui.s.transparent_bg)) mark_globals_dirty();
        if (!mov) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("MOV only: the background is fully transparent (alpha 0),\n"
                              "so the counter can be overlaid in an editor without keying.");
    }

    // Encoder settings (global). CRF / preset / tune are H.264 only; the
    // keyframe interval applies to both formats.
    if (ImGui::TreeNode("Encoder settings")) {
        const bool mp4 = g_ui.s.output_format == 0;
        if (!mp4) ImGui::BeginDisabled();
        ImGui::Text("Quality (CRF)");
        ImGui::SameLine(170);
        ImGui::SetNextItemWidth(160);
        int crf = g_ui.s.x264_crf;
        if (ImGui::SliderInt("##crf", &crf, 0, 51, "%d", ImGuiSliderFlags_AlwaysClamp) && crf != g_ui.s.x264_crf) {
            g_ui.s.x264_crf = crf;
            mark_globals_dirty();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("H.264 constant quality. Lower = better quality, bigger file.\n"
                              "18 (default) is visually lossless for text; 23 is x264's usual default.\n"
                              "0 = mathematically lossless (High 4:4:4 profile: many players and\n"
                              "browsers cannot play it). Ctrl+click to type a value.");
        ImGui::Text("Speed preset");
        ImGui::SameLine(170);
        ImGui::SetNextItemWidth(160);
        {
            const int np = (int)(sizeof(fmcg_video::k_presets) / sizeof(fmcg_video::k_presets[0]));
            int idx = 2;
            for (int i = 0; i < np; ++i)
                if (g_ui.s.x264_preset == fmcg_video::k_presets[i]) idx = i;
            if (ImGui::Combo("##preset", &idx, fmcg_video::k_presets, np)
                && g_ui.s.x264_preset != fmcg_video::k_presets[idx]) {
                g_ui.s.x264_preset = fmcg_video::k_presets[idx];
                mark_globals_dirty();
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("x264 speed/size trade-off at the same quality.\n"
                              "Slower presets make smaller files but render slower.\n"
                              "veryfast (default) renders 1080p60 at several times realtime.");
        ImGui::Text("Tune");
        ImGui::SameLine(170);
        ImGui::SetNextItemWidth(160);
        {
            static const char* k_tune_labels[] = {"None", "Animation", "Still image", "Film", "Grain", "Fast decode"};
            const int nt = (int)(sizeof(fmcg_video::k_tunes) / sizeof(fmcg_video::k_tunes[0]));
            int idx = 0;
            for (int i = 0; i < nt; ++i)
                if (g_ui.s.x264_tune == fmcg_video::k_tunes[i]) idx = i + 1;
            if (ImGui::Combo("##tune", &idx, k_tune_labels, nt + 1)) {
                const std::string t = idx == 0 ? "" : fmcg_video::k_tunes[idx - 1];
                if (t != g_ui.s.x264_tune) { g_ui.s.x264_tune = t; mark_globals_dirty(); }
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("x264 tuning for the content. None (default) suits text overlays;\n"
                              "Still image / Animation favour flat areas and sharp edges.");
        if (!mp4) ImGui::EndDisabled();
        ImGui::Text("Keyframe every");
        ImGui::SameLine(170);
        ImGui::SetNextItemWidth(100);
        double kf = g_ui.s.keyframe_seconds;
        if (ImGui::InputDouble("##keyint", &kf, 0, 0, "%.2f")) {
            if (kf < 0.1) kf = 0.1;
            if (kf > 60.0) kf = 60.0;
            if (kf != g_ui.s.keyframe_seconds) { g_ui.s.keyframe_seconds = kf; mark_globals_dirty(); }
        }
        ImGui::SameLine();
        ImGui::Text("seconds");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Maximum distance between keyframes (0.1..60 s), both formats.\n"
                              "Shorter = faster seeking in editors, slightly bigger files.");
        if (ImGui::Button("Reset encoder defaults")) {
            g_ui.s.x264_crf = 18;
            g_ui.s.x264_preset = "veryfast";
            g_ui.s.x264_tune.clear();
            g_ui.s.keyframe_seconds = 2.0;
            mark_globals_dirty();
        }
        ImGui::TreePop();
    }
    ImGui::Spacing();
    ImGui::Text("Output Path");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80);
    ImGui::InputText("##output", g_ui.output_buf, sizeof(g_ui.output_buf));
    ImGui::SameLine();
    {
        bool odialog_active = g_app.dialog_busy.load();
        if (odialog_active) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
        if (ImGui::Button("Browse##out") && !odialog_active) {
            std::string cmd;
#if defined(_WIN32)
            cmd = "powershell -NoProfile -Command \""
                  "Add-Type -AssemblyName System.Windows.Forms; "
                  "$f = New-Object System.Windows.Forms.SaveFileDialog; "
                  "$f.Filter = 'MP4 video (*.mp4)|*.mp4|All files (*.*)|*.*'; "
                  "$f.Title = 'Select output video'; "
                  "if ($f.ShowDialog() -eq 'OK') { $f.FileName }\"";
#elif defined(__APPLE__)
            cmd = "osascript -e 'tell application \"System Events\" to set f to (choose file name with prompt \"Select output video\") as alias' -e 'POSIX path of f'";
#else
            cmd = "zenity --file-selection --save --title='Select output video' --file-filter='MP4 video | *.mp4' 2>/dev/null || "
                  "kdialog --getsavefilename . 'Select output video' 2>/dev/null";
#endif
            launch_dialog(cmd, "output");
        }
        if (odialog_active) ImGui::PopStyleVar();
    }
    // Poll output-path dialog result.
    {
        std::string result = poll_dialog_result("output");
        if (!result.empty()) {
            strncpy(g_ui.output_buf, result.c_str(), sizeof(g_ui.output_buf) - 1);
            g_ui.output_buf[sizeof(g_ui.output_buf) - 1] = '\0';
            g_ui.s.output_video = g_ui.output_buf;
        }
    }
}

// --- per-frame drawing -------------------------------------------------------
// Extracted from the main loop so it can also run from the window-refresh
// callback: on Windows, border-drag/resize enters a modal loop that never
// returns to glfwPollEvents, and GLFW repaints through this callback there.
static void draw_frame(GLFWwindow* window) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("fMCG", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::Text("fMCG - Fast MIDI Counter Generator");
    ImGui::Separator();
    ImGui::Spacing();

    // --- MIDI File ---
    ImGui::Text("MIDI File:");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80);
    ImGui::InputText("##midi", g_ui.midi_buf, sizeof(g_ui.midi_buf), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();

    {
        bool dialog_active = g_app.dialog_busy.load();
        if (dialog_active) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
        if (ImGui::Button("Browse") && !dialog_active) {
            std::string cmd;
#if defined(_WIN32)
            cmd = "powershell -NoProfile -Command \""
                  "Add-Type -AssemblyName System.Windows.Forms; "
                  "$f = New-Object System.Windows.Forms.OpenFileDialog; "
                  "$f.Filter = 'MIDI & archives (*.mid;*.midi;*.7z;*.xz;*.rar;*.gz;*.bz2;*.zst;*.lz4)|*.mid;*.midi;*.7z;*.xz;*.rar;*.gz;*.bz2;*.zst;*.lz4|All files (*.*)|*.*'; "
                  "$f.Title = 'Select MIDI file'; "
                  "if ($f.ShowDialog() -eq 'OK') { $f.FileName }\"";
#elif defined(__APPLE__)
            cmd = "osascript -e 'tell application \"System Events\" to set f to (choose file with prompt \"Select MIDI file\") as alias' -e 'POSIX path of f'";
#else
            cmd = "zenity --file-selection --title='Select MIDI file' --file-filter='MIDI & archives | *.mid *.midi *.7z *.xz *.rar *.gz *.bz2 *.zst *.lz4' 2>/dev/null || "
                  "kdialog --getopenfilename . 'MIDI & archives (*.mid *.midi *.7z *.xz *.rar *.gz *.bz2 *.zst *.lz4)' 'Select MIDI file' 2>/dev/null";
#endif
            launch_dialog(cmd, "midi");
        }
        if (dialog_active) ImGui::PopStyleVar();
    }

    // Poll MIDI file dialog result
    {
        std::string result = poll_dialog_result("midi");
        if (!result.empty()) {
            std::string err;
            if (validate_midi(result, err)) {
                g_ui.s.midi_file = result;
                strncpy(g_ui.midi_buf, result.c_str(), sizeof(g_ui.midi_buf) - 1);
                std::string dir = extract_dir(result);
                std::string stem = extract_stem(result);
                g_ui.s.output_video = dir + stem + "_fMCG" + fmcg_video::extension((fmcg_video::Format)g_ui.s.output_format);
                strncpy(g_ui.output_buf, g_ui.s.output_video.c_str(), sizeof(g_ui.output_buf) - 1);
                g_ui.globals.midi_dir = dir;
                mark_globals_dirty();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // MIDI file stays on top; below it Global (left) and Pattern (right) side
    // by side, so the window no longer has to fit both groups stacked.
    if (ImGui::BeginTable("##settings_cols", 2,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Global", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Pattern", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        draw_global_section();
        ImGui::TableSetColumnIndex(1);
        draw_pattern_section();
        ImGui::EndTable();
    }

    maybe_autosave_globals();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Action Buttons ---
    {
        bool can_process = (g_ui.midi_buf[0] != '\0') && !g_app.busy.load();
        bool can_render  = g_app.processed.load() && !g_app.busy.load();

        // CC is always counted, so {cc*} tokens always render real values.

        if (!can_process) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
        if (ImGui::Button("Process", ImVec2(140, 30)) && can_process) {
            g_ui.s.midi_file = g_ui.midi_buf;
            g_ui.s.output_video = g_ui.output_buf;
            g_ui.s.layout_text = g_ui.layout_buf;
            g_app.processed = false;
            g_app.done = false;
            g_app.log_lines.clear();
            g_app.preview_playing = false;
            g_app.preview_time = 0.0;
            g_app.busy = true;  // set before detach so the button disables immediately
            std::thread(run_process, g_ui.s).detach();
        }
        if (!can_process) ImGui::PopStyleVar();

        ImGui::SameLine();
        // Cancel while processing or rendering (checked at ~1M-event pings
        // in the scan; within 500ms in the ffmpeg wait loop).
        if (g_app.busy.load()) {
            ImGui::Button("Cancel", ImVec2(140, 30));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Abort the current operation");
            if (ImGui::IsItemClicked()) {
                g_app.cancel = true;
                if (g_app.render_active) g_app.gui_log("Cancelling render...", false);
            }
        }

        // Preview: allowed even without a processed MIDI -- it then shows
        // the current pattern with all stats at zero (and, during the start
        // delay, the negative countdown). While a render runs it reopens
        // the live render-progress view.
        bool can_preview = (!g_app.busy.load()) || g_app.render_active;
        bool preview_starts_render_view = g_app.render_active && g_app.busy.load();
        ImGui::SameLine();
        if (!can_preview) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
        if (ImGui::Button("Preview", ImVec2(140, 30)) && can_preview) {
            // Stamp the watch flag on the click itself only -- assigning it
            // every frame would clobber it the instant the render finishes,
            // and the watching popup would miss its close window.
            g_app.preview_watching_render = preview_starts_render_view;
            // A stale completion latch (render finished with the popup closed)
            // still owes the timeline its saved position: apply the same
            // restore the popup would have done, then clear the latch.
            if (!preview_starts_render_view && g_app.render_watch_done.exchange(false))
                g_app.preview_time = g_app.preview_saved_pos;
            {
                // Snap the live mirror to the current pattern so Preview
                // (with or without a processed MIDI) shows the edited look.
                // Always: the pattern-only preview needs template_lines too.
                g_app.template_lines.clear();
                {
                    std::istringstream tstream(g_ui.layout_buf);
                    std::string ln;
                    while (std::getline(tstream, ln)) {
                        while (!ln.empty() && ln.back() == '\r') ln.pop_back();
                        if (!ln.empty()) g_app.template_lines.push_back(ln);
                    }
                }
                g_app.preview_commas = g_ui.s.commas;
                g_app.preview_pad = g_ui.s.pad;
                g_app.preview_bpm = g_ui.s.bpm;
                g_app.text_colour_ass = g_ui.s.text_color_aabbggrr;
                g_app.bg_colour_ass = g_ui.s.bg_color_aabbggrr;
                g_app.outline_enabled = g_ui.s.outline_enabled;
                g_app.outline_width = g_ui.s.outline_width;
                g_app.outline_colour_ass = g_ui.s.outline_color_aabbggrr;
                g_app.typo = g_ui.s.typo;
                g_app.font_size = g_ui.s.font_size;
                g_app.font_family = g_ui.s.font_family;
                g_app.font_variant = g_ui.s.font_variant;
                g_app.font_bold = g_ui.s.font_bold;
                g_app.font_italic = g_ui.s.font_italic;
                g_app.pos_mode = g_ui.s.pos_mode;
                g_app.pos_x = g_ui.s.pos_x;
                g_app.pos_y = g_ui.s.pos_y;
                g_app.frame_anchor = g_ui.s.frame_anchor;
                g_app.block_anchor = g_ui.s.block_anchor;
                g_app.cells = g_ui.s.cells;
                g_app.cell_align = g_ui.s.cell_align;
                g_app.alignment = g_ui.s.alignment;
                g_app.ass_alignment = gui_alignment_to_ass(g_ui.s.alignment);
                g_app.vid_width = g_ui.s.width;
                g_app.vid_height = g_ui.s.height;
                g_app.start_delay = g_ui.s.start_delay;
                g_app.end_delay = g_ui.s.end_delay;
            }
            g_app.show_preview = true;
            // Auto-play on open (pattern-only or full preview alike).
            g_app.preview_playing = !preview_starts_render_view;
            g_app.preview_start_time = glfwGetTime();
            g_app.preview_start_pos = g_app.preview_time;
            ImGui::OpenPopup("Preview");
        }
        if (!can_preview) ImGui::PopStyleVar();

        ImGui::SameLine();
        if (!can_render) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
        if (ImGui::Button("Render Video", ImVec2(140, 30)) && can_render) {
            g_app.show_preview = true;
            g_app.render_active = true;
            g_app.render_watch_done = false;        // fresh render, fresh watch
            g_app.preview_watching_render = true;   // popup closes when render ends
            g_app.preview_playing = false;
            g_app.preview_time = 0.0;
            ImGui::OpenPopup("Preview");

            RenderSettings rs;
            rs.output_video = g_ui.output_buf;
            rs.midi_dir = g_app.midi_dir;
            rs.midi_stem = g_app.midi_stem;
            rs.width = g_app.vid_width;
            rs.height = g_app.vid_height;
            rs.fps = g_app.fps;
            rs.total_duration = g_app.total_duration + g_app.start_delay;
            // NOTE: g_app.total_duration is frames.back().timestamp_sec, which
            // ALREADY includes the end-delay tail (the engine extends the
            // frames through it). Adding end_delay here again would make
            // ffmpeg's -d run past the last ASS dialogue -> black tail.
            rs.ffmpeg_threads = g_ui.s.ffmpeg_threads;
            rs.bg_color_aabbggrr = g_app.bg_colour_ass;
            rs.output_format = g_ui.s.output_format;
            rs.transparent_bg = g_ui.s.transparent_bg;
            rs.x264_crf = g_ui.s.x264_crf;
            rs.x264_preset = g_ui.s.x264_preset;
            rs.x264_tune = g_ui.s.x264_tune;
            rs.keyframe_seconds = g_ui.s.keyframe_seconds;
            g_app.done = false;
            g_app.log_lines.clear();
            // Set busy on the GUI thread *before* detaching: the preview popup
            // closes itself when it sees render-active && !busy, and the detached
            // thread may not have run yet on the first frame (race closed the
            // popup instantly, so it never appeared).
            g_app.busy = true;
            std::thread(run_render, rs).detach();
        }
        if (!can_render) ImGui::PopStyleVar();

        ImGui::SameLine();
        // Unload: clear the loaded MIDI and any processed state. Warn
        // first -- re-processing takes time on big files.
        bool has_file = (g_ui.midi_buf[0] != '\0') || g_app.processed.load();
        if (!has_file || g_app.busy.load()) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
        if (ImGui::Button("Unload", ImVec2(140, 30)) && has_file && !g_app.busy.load()) {
            ImGui::OpenPopup("Unload file?");
        }
        if (!has_file || g_app.busy.load()) ImGui::PopStyleVar();

        ImGui::SameLine();
        if (g_app.processed.load()) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "Processed");
        } else if (g_app.busy.load()) {
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "Working...");
        } else if (g_app.done.load() && g_app.result_ret.load() == 0 && !g_app.processed.load()) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Done!");
        }
    }

    // --- Progress Bar ---
    if (g_app.busy.load() || g_app.progress.load() > 0.0f) {
        float p = g_app.display_progress();
        std::string overlay;
        {
            std::lock_guard<std::mutex> lock(g_app.scan_mutex);
            // Scan stats belong on the bar only while processing, never
            // during an ffmpeg render.
            if (g_app.scan_active && !g_app.render_active)
                overlay = g_app.compose_scan_line();
        }
        char pct[48];
        float spd = g_app.render_speed.load();
        if (g_app.render_active && spd > 0.0f)
            snprintf(pct, sizeof(pct), "%d%%  (%.2fx speed)", (int)(p * 100), spd);
        else
            snprintf(pct, sizeof(pct), "%d%%", (int)(p * 100));
        ImGui::ProgressBar(p, ImVec2(-1, 0), overlay.empty() ? pct : overlay.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // MIDI spec-violation modal: the processing worker crossed the 28-bit
    // tick limit and is blocked on the user's choice. Answer propagates
    // through app state back to the worker. Spawned at the exact center of
    // the window so it is unmissable.
    if (g_app.spec_prompt_open.load()) {
        if (!g_app.spec_popup_started) {
            ImGui::OpenPopup("MIDI exceeds spec");
            g_app.spec_popup_started = true;
        }
        // Assert the centered position on EVERY frame the modal is open:
        // a one-shot set before OpenPopup can be overridden by the popup
        // layer re-applying a stored position on the open frame.
        {
            ImVec2 dsp = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(dsp.x * 0.5f, dsp.y * 0.5f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        if (ImGui::BeginPopupModal("MIDI exceeds spec", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            // Minimum content width for the three-button row: auto-resize
            // otherwise sizes the window to the two-button row and a flush-
            // right Cancel would overlap "Two-pass".
            const float btn_w = 190.0f;
            ImGui::Dummy(ImVec2(btn_w * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f, 1.0f));
            ImGui::Text("This MIDI's tick count breaks the MIDI spec (28-bit delta-time limit).\n"
                        "The normal single-pass engine would need many gigabytes of RAM.");
            ImGui::Spacing();
            // No manual line breaks: the button row below fixes the modal's
            // width, and TextWrapped fills that width for the whole block.
            ImGui::TextWrapped("Proceeding with the single-pass engine will try to allocate "
                               "memory proportional to the song's tick count. On a file like "
                               "this that can reach tens of gigabytes; if RAM runs out the "
                               "system will stutter, swap (thrash) heavily, and the operation "
                               "may fail or freeze the whole PC -- not just this program.");
            ImGui::Spacing();
            ImGui::TextWrapped("Two-pass mode re-reads the file (roughly doubling the "
                               "decode/parse time) but keeps RAM small regardless of tick "
                               "count. Cancel aborts now.");
            ImGui::Spacing();
            if (ImGui::Button("Proceed (single-pass)", ImVec2(btn_w, 0))) {
                g_app.spec_choice.store(0);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Two-pass (low memory)", ImVec2(btn_w, 0))) {
                g_app.spec_choice.store(1);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                                 + ImGui::GetContentRegionAvail().x - btn_w);   // flush right
            if (ImGui::Button("Cancel", ImVec2(btn_w, 0))) {
                g_app.spec_choice.store(2);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // Unload confirmation modal (opened from the action row above).
    if (ImGui::BeginPopupModal("Unload file?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Unload the current MIDI file?");
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
            "Processed data will be discarded; the file must be re-processed to render.");
        ImGui::Spacing();
        if (ImGui::Button("Unload", ImVec2(140, 0))) {
            g_ui.midi_buf[0] = '\0';
            g_ui.output_buf[0] = '\0';
            g_ui.s.midi_file.clear();
            g_ui.s.output_video.clear();
            g_app.processed = false;
            g_app.done = false;
            g_app.result_ret = -1;
            g_app.op_result = AppState::OP_NONE;
            g_app.progress.store(0.0f);   // reset the progress bar too
            g_app.result_path.clear();
            g_app.frames_data.clear();
            g_app.template_lines.clear();
            g_app.total_notes = 0;
            g_app.total_duration = 0.0;
            g_app.preview_playing = false;
            g_app.preview_time = 0.0;
            g_app.show_preview = false;
            g_app.render_active = false;
            g_app.preview_watching_render = false;
            g_app.render_watch_done = false;
            g_app.log_lines.clear();
            g_app.gui_log("File unloaded.", false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(140, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // --- Log ---
    ImGui::Text("Log Output:");
    ImGui::BeginChild("log_area", ImVec2(0, std::max(150.0f, ImGui::GetContentRegionAvail().y - 25)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lock(g_app.log_mutex);
        // While a scan is live, the last log entry is a self-overwriting
        // status line (per the user's request) instead of a growing list.
        std::string scan_line;
        bool scan_live = false;
        {
            std::lock_guard<std::mutex> slock(g_app.scan_mutex);
            scan_live = g_app.scan_active && g_app.busy.load() && g_app.processed.load() == false;
            // While the spec-violation modal parks the worker, the live line
            // is stale: show the full log (including the warning that asked
            // the question) instead of hiding the newest entry behind it.
            if (g_app.spec_prompt_open.load()) scan_live = false;
            if (scan_live) scan_line = g_app.compose_scan_line();
        }
        size_t n = g_app.log_lines.size();
        if (scan_live && n > 0) n -= 1;   // the live line replaces the last one
        for (size_t i = 0; i < n; ++i)
            ImGui::TextWrapped("%s", g_app.log_lines[i].c_str());
        if (scan_live) ImGui::TextWrapped("%s", scan_line.c_str());
        else if (!g_app.log_lines.empty() && n != g_app.log_lines.size())
            ImGui::TextWrapped("%s", g_app.log_lines.back().c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    // Final status line, phrased per operation so a failed/cancelled
    // PROCESS is never mislabelled as "Render failed". Shown for every
    // completed op except PROCESS_OK ("processed" is already true then and
    // the header tag covers it) -- notably it MUST show after a successful
    // render even though processed==true there.
    if (g_app.done.load() && !g_app.busy.load()) {
        const int op = g_app.op_result.load();
        if (op == AppState::OP_PROCESS_OK) {
            // no status text: the header already shows the Processed tag
        } else if (op == AppState::OP_RENDER_OK) {
            // Effective speed: video length divided by the render's wall-clock
            // time (>1x = faster than realtime). ffmpeg's own instantaneous
            // speed is shown live on the progress bar during the render.
            const double wc = g_app.render_wallclock.load();
            const double speed = (wc > 0.0 && g_app.total_duration > 0.0)
                               ? (g_app.total_duration + g_app.start_delay) / wc : 0.0;
            char spd_txt[32] = "";
            if (speed > 0.0) snprintf(spd_txt, sizeof(spd_txt), " (%.2fx speed)", speed);
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Rendered: %s%s",
                g_app.result_path.c_str(), spd_txt);
        } else if (op == AppState::OP_RENDER_FAIL) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Render failed. Check log.");
        } else if (op == AppState::OP_RENDER_CANCELLED) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Render cancelled.");
        } else if (op == AppState::OP_PROCESS_FAIL) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Processing failed. Check log.");
        } else if (op == AppState::OP_PROCESS_CANCELLED) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Processing cancelled.");
        }
        // OP_NONE: nothing completed yet; OP_PROCESS_OK is excluded above
        // by !processed (processed=true shows the "Processed" tag instead).
    }

    // --- Preview popup ---
    render_preview_popup();

    ImGui::End();

    ImGui::Render();
    int dw, dh;
    glfwGetFramebufferSize(window, &dw, &dh);
    glViewport(0, 0, dw, dh);
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
}

// --- startup ----------------------------------------------------------------

#if defined(_WIN32)
// Windows runs border-drag/title-bar-move inside a modal loop buried in
// DefWindowProc that blocks glfwPollEvents(). GLFW repaints via the window-
// refresh callback, but the OS only delivers WM_PAINT once the message queue
// goes idle -- so a fast drag can end with DWM holding no presented frame for
// the re-exposed region, which then shows black. This subclass starts a
// periodic timer for the duration of the modal loop; each tick invalidates
// the window, producing the WM_PAINT that drives the refresh callback and a
// fresh swap. KillTimer on exit lets normal pacing resume.
static WNDPROC s_glfw_prev_proc = nullptr;
static constexpr UINT_PTR k_modal_paint_timer = 1;

static LRESULT CALLBACK fmcg_modal_paint_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ENTERSIZEMOVE:
            SetTimer(hwnd, k_modal_paint_timer, USER_TIMER_MINIMUM, nullptr);
            break;
        case WM_EXITSIZEMOVE:
        case WM_DESTROY:
            KillTimer(hwnd, k_modal_paint_timer);
            break;
        case WM_TIMER:
            if (wp == k_modal_paint_timer) {
                InvalidateRect(hwnd, nullptr, FALSE);
                UpdateWindow(hwnd);
                return 0;
            }
            break;
        default: break;
    }
    return CallWindowProc(s_glfw_prev_proc, hwnd, msg, wp, lp);
}
#endif

int main() {
#if defined(_WIN32) && defined(FMCG_DEBUG)
    // Debug builds keep a console for printf/stderr tracing; release builds
    // (linked with -mwindows) have no console and none is created.
    if (AllocConsole()) {
        FILE* f_out = nullptr; FILE* f_err = nullptr; FILE* f_in = nullptr;
        freopen_s(&f_out, "CONOUT$", "w", stdout);
        freopen_s(&f_err, "CONOUT$", "w", stderr);
        freopen_s(&f_in, "CONIN$", "r", stdin);
    }
#endif
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // Default size: fit the whole UI vertically so no scrollbar is needed on
    // a typical desktop. Take the monitor's work area (excludes the
    // taskbar), capped at a sane height; the window stays user-resizable.
    int win_w = 1100, win_h = 800;
    if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
        const GLFWvidmode* vm = glfwGetVideoMode(mon);
        int wx, wy, ww, wh;
        glfwGetMonitorWorkarea(mon, &wx, &wy, &ww, &wh);
        if (vm && wh > 0) {
            win_w = (ww > 0) ? ww : vm->width;
            win_h = wh - 80;                       // headroom: title bar + margins
            if (win_w > 1400) win_w = 1400;
            if (win_h > 900)  win_h = 900;
            if (win_h < 600) win_h = 600;
        }
    }

    GLFWwindow* window = glfwCreateWindow(win_w, win_h, "fMCG - Fast MIDI Counter Generator", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

#if defined(_WIN32)
    // Chain onto GLFW's window proc to keep frames flowing during the OS
    // modal move/resize loop (see fmcg_modal_paint_proc above).
    {
        HWND hwnd = glfwGetWin32Window(window);
        s_glfw_prev_proc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                                      (LONG_PTR)fmcg_modal_paint_proc);
    }
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");
    // Keep repainting while the OS modal drag/resize loop blocks the main
    // loop (Windows); without this the UI freezes during border drags.
    glfwSetWindowRefreshCallback(window, [](GLFWwindow* w) {
        draw_frame(w);
    });

    // --- persistent state -----------------------------------------------------
    load_global_settings(g_ui.globals);
    ensure_default_pattern();

    g_ui.system_fonts = enumerate_system_fonts();
    for (auto& f : g_ui.system_fonts) g_ui.font_cstrs.push_back(f.c_str());   // safe: system_fonts is never mutated after this

    // Bootstrap the UI from the Default pattern (or first available).
    {
        auto names = list_patterns();
        if (!names.empty()) set_active_pattern(names[0]);
        else {
            PatternData p;                 // no store available: in-memory defaults
            p.layout_text = k_default_layout;
            apply_pattern_to_ui(p);
            g_ui.pattern_baseline = p;
        }
    }
    g_ui.s.width = g_ui.globals.width;
    g_ui.s.height = g_ui.globals.height;
    g_ui.s.fps = g_ui.globals.fps;
    g_ui.s.vel0_note_off = g_ui.globals.vel0_note_off;
    g_ui.s.close_held_notes = g_ui.globals.close_held_notes;
    g_ui.s.start_delay = g_ui.globals.start_delay;
    g_ui.s.end_delay = g_ui.globals.end_delay;
    g_ui.s.ffmpeg_threads = g_ui.globals.ffmpeg_threads;
    g_ui.s.parse_threads  = g_ui.globals.parse_threads;
    g_ui.s.track_dedup    = g_ui.globals.track_dedup;
    g_ui.s.output_format  = g_ui.globals.output_format;
    g_ui.s.transparent_bg = g_ui.globals.transparent_bg;
    g_ui.s.x264_crf       = g_ui.globals.x264_crf;
    g_ui.s.x264_preset    = g_ui.globals.x264_preset;
    g_ui.s.x264_tune      = g_ui.globals.x264_tune;
    g_ui.s.keyframe_seconds = g_ui.globals.keyframe_seconds;

    // Refresh the variant cache for the bootstrapped family.
    g_ui.variants_for = g_ui.s.font_family;
    g_ui.variant_names.clear();
    for (const auto& v : enumerate_font_variants(g_ui.s.font_family))
        g_ui.variant_names.push_back(v.style);
    g_ui.variant_idx = 0;
    for (size_t i = 0; i < g_ui.variant_names.size(); ++i)
        if (g_ui.variant_names[i] == g_ui.s.font_variant) g_ui.variant_idx = (int)i;

    // Restore last MIDI/output paths if the files still exist.
    if (!g_ui.globals.midi_dir.empty()) {
        // stored as a directory hint only; the file itself is picked per-session
    }

    font_list_loaded = !g_ui.font_cstrs.empty();   // draw_frame()'s lazy-init flag

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        draw_frame(window);
    }

    // Flush pending settings on exit.
    if (g_ui.globals_dirty) save_globals_now();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
