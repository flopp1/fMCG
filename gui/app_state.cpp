#include "app_state.h"

#include <sstream>
#include <iomanip>
#include <algorithm>

AppState g_app;
UiState  g_ui;

// ---------------------------------------------------------------------------
// Alignment + font-variant helpers
// ---------------------------------------------------------------------------

void set_app_pattern(const PatternData& p) {
    g_app.pattern = p;
    pattern_template(p, g_app.template_lines, g_app.template_boxes);
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

fmcg_text::TextStyle text_style_for(const PatternData& p) {
    fmcg_text::TextStyle st;
    st.font_file = find_font_file_for_variant(p.font_family, p.font_variant, &st.face_index);
    st.size = p.font_size;
    st.colour = fmcg_text::from_ass_colour(p.text_color_aabbggrr);
    st.outline = p.outline_enabled && p.outline_width > 0.0;
    st.outline_width = p.outline_width;
    st.outline_colour = fmcg_text::from_ass_colour(p.outline_color_aabbggrr);
    p.typo.apply(st);
    return st;
}

fmcg_text::Placement placement_for(const PatternData& p, int frame_w, int frame_h) {
    fmcg_text::Placement pl;
    pl.frame_w = std::max(16, frame_w);
    pl.frame_h = std::max(16, frame_h);
    pl.boxes = fmcg_boxes::place_list(p.boxes);
    pl.margin = p.margin;
    pl.cells = p.cells;
    pl.cell_align = p.cell_align;
    return pl;
}

fmcg_text::TextStyle app_text_style() { return text_style_for(g_app.pattern); }
fmcg_text::Placement app_placement() { return placement_for(g_app.pattern, g_app.vid_width, g_app.vid_height); }

FrameTextSource text_source_for(const PatternData& p) {
    FrameTextSource src = app_text_source();
    pattern_template(p, src.template_lines, src.template_boxes);
    src.fmt = p.format_opts();
    return src;
}

FrameTextSource app_text_source() {
    FrameTextSource src;
    src.frames = &g_app.frames_data;
    src.template_lines = g_app.template_lines;
    src.template_boxes = g_app.template_boxes;
    src.song.notes = g_app.total_notes;
    src.song.cc = g_app.frames_data.empty() ? 0 : g_app.frames_data.back().cumulative_cc;
    src.song.ticks = g_app.total_ticks;
    src.song.meter = g_app.meter;
    src.song.ppqn = g_app.ppqn;
    src.fps = g_app.fps > 0 ? g_app.fps : 60.0;
    src.start_delay = g_app.start_delay;
    src.end_delay = g_app.end_delay;
    src.fmt = g_app.pattern.format_opts();
    return src;
}
