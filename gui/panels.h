// panels.h -- the main window's settings panels (gui/main.cpp draws the frame).
//   panels.cpp         : settings persistence glue + pattern store helpers
//   pattern_panel.cpp  : right column (pattern: layout, font, colours, placement, numbers)
//   global_panel.cpp   : left column (global: video, scan, encoder, output)
#pragma once
#include "settings_store.h"

#include <string>

extern double g_pattern_list_time;   // last patterns-folder scan (ImGui time)
extern bool font_list_loaded;        // font-list lazy-init (set at startup)

// Globals: the widgets edit g_ui.s.global; saving is debounced.
void save_globals_now();
void maybe_autosave_globals();
void mark_globals_dirty();

// Patterns.
PatternData current_pattern_from_ui();
void apply_pattern_to_ui(const PatternData& p);
// The layout editor shows the selected box's text: commit_layout_buf() stores
// the editor's text into that box; select_box() commits, then switches.
void commit_layout_buf();
void select_box(int index);
void load_layout_buf();   // the selected box's text -> the editor (no commit)
bool pattern_dirty();
void set_active_pattern(const std::string& name);
void sync_pattern_index();
void refresh_pattern_list();

void draw_pattern_section();   // right column
void draw_global_section();    // left column
