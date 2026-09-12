#include "app_state.h"

#include <sstream>
#include <iomanip>

AppState g_app;
UiState  g_ui;

// ---------------------------------------------------------------------------
// Alignment + font-variant helpers
// ---------------------------------------------------------------------------

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

// Rendered as a single log line that overwrites itself.
std::string AppState::compose_scan_line() {
    std::ostringstream ss;
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
    progress.store(1.0f);
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
