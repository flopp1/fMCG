// panels.cpp -- settings persistence glue and pattern store helpers shared by
// the panels and the frame (see panels.h).
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

const char* const k_alignment_items[] = {
    "Top Left", "Top Right", "Bottom Left", "Bottom Right", "Top Center", "Bottom Center"
};
const int k_alignment_count = 6;

// --- persistence glue -------------------------------------------------------

// The widgets edit g_ui.s.global directly, so saving writes it as is.
void save_globals_now() {
    g_ui.globals_dirty = false;
    g_ui.last_globals_save = ImGui::GetTime();
    save_global_settings(g_ui.s.global);
}

// Globals autosave: debounced to at most one write/second while dragging.
void maybe_autosave_globals() {
    if (!g_ui.globals_dirty) return;
    double now = ImGui::GetTime();
    if (now - g_ui.last_globals_save >= 1.0) save_globals_now();
}

void mark_globals_dirty() { g_ui.globals_dirty = true; }

// The pattern being edited: g_ui.s.pattern with the layout editor's text.
PatternData current_pattern_from_ui() {
    PatternData p = g_ui.s.pattern;
    p.layout_text = g_ui.layout_buf;
    return p;
}

void apply_pattern_to_ui(const PatternData& p) {
    g_ui.s.pattern = p;
    strncpy(g_ui.layout_buf, p.layout_text.c_str(), sizeof(g_ui.layout_buf) - 1);
    g_ui.layout_buf[sizeof(g_ui.layout_buf) - 1] = '\0';
    if (g_ui.s.pattern.alignment < 0 || g_ui.s.pattern.alignment >= k_alignment_count) g_ui.s.pattern.alignment = 0;
    g_ui.alignment_idx = g_ui.s.pattern.alignment;
    g_ui.variants_for.clear();   // the style combo re-reads the family's styles
    // Family: prefer an exact match in the enumerated list; else keep the name
    // (custom-input fallback path still renders it if the file exists).
    int idx = -1;
    for (size_t i = 0; i < g_ui.system_fonts.size(); ++i)
        if (g_ui.system_fonts[i] == p.font_family) { idx = (int)i; break; }
    if (idx >= 0) g_ui.selected_font_idx = idx;
}

bool pattern_dirty() {
    return !(current_pattern_from_ui() == g_ui.pattern_baseline);
}

void set_active_pattern(const std::string& name) {
    g_ui.active_pattern = name;
    PatternData p;
    if (load_pattern(name, p)) {
        apply_pattern_to_ui(p);
        g_ui.pattern_baseline = p;
    }
}

// Keep the combo index pointing at the active pattern (cheap: every frame).
void sync_pattern_index() {
    g_ui.active_pattern_idx = 0;
    for (size_t i = 0; i < g_ui.pattern_names.size(); ++i)
        if (g_ui.pattern_names[i] == g_ui.active_pattern) g_ui.active_pattern_idx = (int)i;
}

double g_pattern_list_time = -1e9;   // last patterns-folder scan (ImGui time)

void refresh_pattern_list() {
    g_ui.pattern_names = list_patterns();
    g_pattern_list_time = ImGui::GetTime();
    sync_pattern_index();
}

bool font_list_loaded = false;   // font-list lazy-init (set at startup)

// Advanced typography (pattern-saved): OpenType features and spacing for the
// in-built renderer. Options the selected font lacks are listed as such
// (HarfBuzz ignores them, so they are harmless but do nothing).
