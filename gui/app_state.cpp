#include "app_state.h"

#include <sstream>
#include <iomanip>

AppState g_app;

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
void AppState::finish_op() {
    {
        std::lock_guard<std::mutex> lock(scan_mutex);
        scan_active = false;
    }
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
