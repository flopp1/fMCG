#include "app_state.h"

#include <sstream>
#include <iomanip>
#include <algorithm>

AppState g_app;
UiState  g_ui;

// ---------------------------------------------------------------------------
// Alignment + font-variant helpers
// ---------------------------------------------------------------------------

int gui_anchor_to_numpad(int idx) {
    static const int k[9] = {7, 8, 9, 4, 5, 6, 1, 2, 3};
    return (idx >= 0 && idx < 9) ? k[idx] : 7;
}

int gui_alignment_to_ass(int idx) {
    switch (idx) {
        case 0: return 7;    // Top Left
        case 1: return 9;    // Top Right
        case 2: return 1;    // Bottom Left
        case 3: return 3;    // Bottom Right
        case 4: return 8;    // Top Center
        case 5: return 2;    // Bottom Center
        default: return 7;
    }
}

void apply_font_variant(GuiSettings& s, const std::string& family, const std::string& variant) {
    s.font_family = family;
    s.font_variant = variant;
    s.font_bold = 0;
    s.font_italic = 0;
    for (const auto& v : enumerate_font_variants(family)) {
        if (v.style == variant) {
            s.font_bold = (v.weight >= 600) ? 1 : 0;
            s.font_italic = v.italic ? 1 : 0;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Logging + progress plumbing
// ---------------------------------------------------------------------------

void AppState::gui_log(const char* msg, bool /*is_error*/) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::string line = msg;
    while (!line.empty() && line.back() == '\n') line.pop_back();
    if (!line.empty()) log_lines.push_back(line);
}

void AppState::gui_progress(const char* /*pass_name*/, int percent) {
    progress.store(percent / 100.0f);
}

void AppState::gui_scan_progress(uint64_t events, double elapsed_sec, double ev_per_s, double frac) {
    std::lock_guard<std::mutex> lock(scan_mutex);
    scan_events = events;
    scan_elapsed = elapsed_sec;
    scan_evps = ev_per_s;
    scan_frac = frac;
    scan_active = true;
}

void AppState::gui_scan_tracks(uint64_t done, uint64_t total) {
    std::lock_guard<std::mutex> lock(scan_mutex);
    scan_has_tracks = true;
    scan_tracks = done;
    scan_tracks_total = total;
}

// Rendered as a single log line that overwrites itself.
std::string AppState::compose_scan_line() {
    std::ostringstream ss;
    if (scan_has_tracks) {
        ss << format_with_commas(scan_tracks);
        if (scan_tracks_total) ss << "/" << format_with_commas(scan_tracks_total);
        ss << (scan_tracks_total == 1 ? " track, " : " tracks, ");
    }
    ss << format_with_commas(scan_events) << " events, ";
    if (scan_evps > 0) ss << format_with_commas((uint64_t)scan_evps) << " ev/s, ";
    ss << std::fixed << std::setprecision(1) << scan_elapsed << "s elapsed";
    return ss.str();
}

// End-of-operation bookkeeping shared by process and render: hide the live
// scan stats (progress bar returns to percentage-only) and mark completion.
// Progress jumps to 100% so fast operations that stalled at their last scan
// ping (~90%) visibly complete instead of hanging just short of the end.
void AppState::finish_op() {
    {
        std::lock_guard<std::mutex> lock(scan_mutex);
        scan_active = false;
    }
    if (render_active) render_watch_done = true;   // popup closes on next frame
    render_active = false;
    // Cancelled operations reset the bar to empty; completed ones show 100%.
    int r = op_result.load();
    bool cancelled = (r == OP_PROCESS_CANCELLED || r == OP_RENDER_CANCELLED);
    progress.store(cancelled ? 0.0f : 1.0f);
    busy = false;
    done = true;
}

// Progress fraction for the bar: the ffmpeg/render side drives progress
// directly; during the scan we derive a smooth value from the stream fraction.
float AppState::display_progress() {
    float p = progress.load();
    std::lock_guard<std::mutex> lock(scan_mutex);
    if (scan_active && scan_frac >= 0.0)
        p = (float)(scan_frac * 0.85);   // scan = 85% of the process, then ASS/render
    return p;
}

fmcg_text::TextStyle app_text_style() {
    fmcg_text::TextStyle st;
    st.font_file = find_font_file_for_variant(g_app.font_family, g_app.font_variant, &st.face_index);
    st.size = g_app.font_size;
    st.colour = fmcg_text::from_ass_colour(g_app.text_colour_ass);
    st.outline = g_app.outline_enabled && g_app.outline_width > 0.0;
    st.outline_width = g_app.outline_width;
    st.outline_colour = fmcg_text::from_ass_colour(g_app.outline_colour_ass);
    g_app.typo.apply(st);
    return st;
}

fmcg_text::Placement app_placement() {
    fmcg_text::Placement pl;
    pl.frame_w = std::max(16, g_app.vid_width);
    pl.frame_h = std::max(16, g_app.vid_height);
    pl.pos_mode = g_app.pos_mode;
    pl.pos_x = g_app.pos_x;
    pl.pos_y = g_app.pos_y;
    pl.frame_anchor = gui_anchor_to_numpad(g_app.frame_anchor);
    pl.block_anchor = gui_anchor_to_numpad(g_app.block_anchor);
    pl.cells = g_app.cells;
    pl.cell_align = g_app.cell_align;
    pl.alignment = g_app.ass_alignment;
    return pl;
}

FrameTextSource app_text_source() {
    FrameTextSource src;
    src.frames = &g_app.frames_data;
    src.template_lines = g_app.template_lines;
    src.total_notes = g_app.total_notes;
    src.total_cc = g_app.frames_data.empty() ? 0 : g_app.frames_data.back().cumulative_cc;
    src.total_ticks = g_app.total_ticks;
    src.ppqn = g_app.ppqn;
    src.fps = g_app.fps > 0 ? g_app.fps : 60.0;
    src.start_delay = g_app.start_delay;
    src.end_delay = g_app.end_delay;
    src.commas = g_app.preview_commas;
    src.pad = g_app.preview_pad;
    src.bpm = g_app.preview_bpm;
    return src;
}
