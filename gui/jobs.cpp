#include "jobs.h"
#include "app_state.h"
#include "dialogs.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <chrono>
#include <atomic>
#include <cstring>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#endif

// ---------------------------------------------------------------------------
// Processing worker: parse MIDI into per-frame stats (rendered later in-process)
// ---------------------------------------------------------------------------

void run_process(GuiSettings s) {
    AppState& app = g_app;
    app.busy = true;
    app.done = false;
    app.progress.store(0.0f);
    {   // the UI thread iterates the log under this mutex every frame
        std::lock_guard<std::mutex> lk(app.log_mutex);
        app.log_lines.clear();
    }
    app.result_path.clear();
    app.processed = false;
    app.cancel = false;   // clearing here too: a previous cancelled op must not abort this scan

    int ass_alignment = gui_alignment_to_ass(s.alignment);

    std::string text_color_ass = s.text_color_aabbggrr;

    std::string midi_dir = extract_dir(s.midi_file);
    std::string midi_stem = extract_stem(s.midi_file);

    if (s.output_video.empty())
        s.output_video = midi_dir + midi_stem + "_fMCG" + fmcg_video::extension((fmcg_video::Format)s.output_format);

    std::vector<std::string> template_lines;
    {
        std::istringstream tstream(s.layout_text);
        std::string line;
        while (std::getline(tstream, line)) {
            while (!line.empty() && (line.back() == '\r')) line.pop_back();
            if (!line.empty()) template_lines.push_back(line);
        }
    }
    if (template_lines.empty()) {
        template_lines = {
            "Time: {time-milli}/{time-milli-max}",
            "Notes: {nc}/{nc-total}/{nc-rem}",
            "NPS: {nps}/{nps-max}",
            "Polyphony: {plph}/{plph-max}",
            "BPM: {bpm}"
        };
    }

    uint16_t ppqn = 480;
    uint64_t total_notes = 0;
    uint64_t total_ticks = 0;

    ProgressCallbacks cb;
    cb.on_progress = [](const char* pass, int percent) { g_app.gui_progress(pass, percent); };
    cb.on_log = [](const char* msg, bool err) { g_app.gui_log(msg, err); };
    cb.on_scan_progress = [](uint64_t ev, double el, double evps, double frac) {
        g_app.gui_scan_progress(ev, el, evps, frac);
    };
    cb.on_tracks = [](uint64_t done, uint64_t total) { g_app.gui_scan_tracks(done, total); };
    cb.cancel_flag = &app.cancel;
    app.cancel = false;

    // MIDI spec-violation handling: when the scanner crosses the 28-bit tick
    // limit it parks on this wait; the UI thread opens a modal and stores the
    // answer in app.spec_choice. Waiting on the atomic (not a future) keeps
    // Cancel live while parked.
    bool spec_prompt_used = false;
    cb.on_spec_violation = [&app, &spec_prompt_used](uint64_t tick, size_t bytes) -> int {
        spec_prompt_used = true;
        app.spec_choice.store(-1);
        app.spec_popup_started = false;
        app.gui_log(("Warning: this MIDI exceeds the spec's 28-bit delta-time limit at tick "
                     + std::to_string(tick) + ".").c_str(), true);
        app.spec_prompt_open.store(true);
        int choice;
        while ((choice = app.spec_choice.load()) < 0) {
            if (app.cancel.load()) { choice = 2; break; }   // Cancel button works while parked
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        app.spec_prompt_open.store(false);
        return choice;
    };

    {
        std::lock_guard<std::mutex> lock(app.scan_mutex);
        app.scan_events = 0; app.scan_elapsed = 0.0; app.scan_evps = 0.0;
        app.scan_frac = -1.0; app.scan_active = false;
        app.scan_has_tracks = false; app.scan_tracks = 0; app.scan_tracks_total = 0;
    }

    app.gui_log(("Processing MIDI: " + s.midi_file + "...").c_str(), false);

    fmcg_stream::set_close_held_notes(s.close_held_notes);
    auto frames = ScaleMidiProcessor::process_midi(s.midi_file, s.fps, ppqn, total_notes,
                                                    s.vel0_note_off, total_ticks, cb, s.end_delay,
                                                    s.parse_threads, s.track_dedup,
                                                    s.track_dedup && s.risky_dedup);
    if (spec_prompt_used) {
        app.spec_prompt_open.store(false);
        app.spec_choice.store(-1);
        app.spec_popup_started = false;
    }
    if (frames.empty()) {
        app.gui_log(app.cancel.load() ? "Processing cancelled." : "Error: Could not parse MIDI file.",
                    app.cancel.load() ? false : true);
        app.result_ret = 1;
        app.op_result = app.cancel.load() ? AppState::OP_PROCESS_CANCELLED : AppState::OP_PROCESS_FAIL;
        app.finish_op();
        return;
    }

    app.progress.store(0.90f);

    double total_duration = frames.back().timestamp_sec;
    uint64_t total_cc = frames.back().cumulative_cc;
    app.midi_dir = midi_dir;
    app.midi_stem = midi_stem;
    app.total_duration = total_duration;
    app.fps = s.fps;
    app.vid_width = s.width;
    app.vid_height = s.height;
    app.ppqn = ppqn;
    app.total_notes = total_notes;
    app.total_ticks = total_ticks;
    app.start_delay = s.start_delay;
    app.end_delay = s.end_delay;
    app.preview_commas = s.commas;
    app.preview_pad = s.pad;
    app.preview_bpm = s.bpm;
    app.bg_colour_ass = s.bg_color_aabbggrr;
    app.pos_mode = s.pos_mode;
    app.pos_x = s.pos_x;
    app.pos_y = s.pos_y;
    app.frame_anchor = s.frame_anchor;
    app.block_anchor = s.block_anchor;
    app.cells = s.cells;
    app.cell_align = s.cell_align;
    app.alignment = s.alignment;

    app.frames_data = std::move(frames);
    app.template_lines = template_lines;
    app.text_colour_ass = text_color_ass;
    app.ass_alignment = ass_alignment;
    app.font_size = s.font_size;
    app.font_family = s.font_family;
    app.font_variant = s.font_variant;
    app.font_bold = s.font_bold;
    app.font_italic = s.font_italic;
    app.outline_enabled = s.outline_enabled;
    app.outline_width = s.outline_width;
    app.outline_colour_ass = s.outline_color_aabbggrr;
    app.typo = s.typo;

    app.gui_log("Processing complete. Ready for preview/render.", false);
    app.processed = true;
    app.result_ret = 0;
    app.op_result = AppState::OP_PROCESS_OK;
    app.finish_op();
}

// ---------------------------------------------------------------------------
// Rendering worker: in-built renderer + encoder (fmcg_video::render_video)
// ---------------------------------------------------------------------------

void run_render(RenderSettings s) {
    AppState& app = g_app;
    app.busy = true;
    app.done = false;
    app.cancel = false;   // a cancelled previous render must not kill this one
    app.progress.store(0.0f);
    app.render_speed.store(0.0f);
    {   // the UI thread iterates the log under this mutex every frame
        std::lock_guard<std::mutex> lk(app.log_mutex);
        app.log_lines.clear();
    }
    app.result_path.clear();
    app.render_active = true;

    // In-built renderer: the same FrameTextSource -> FrameRenderer path the
    // preview draws, encoded in-process (no FFmpeg, no subtitle file).
    const fmcg_video::Format fmt = (fmcg_video::Format)s.output_format;
    const bool transparent = fmt == fmcg_video::Format::MOV_QTRLE && s.transparent_bg;
    app.gui_log(("Rendering video: " + s.output_video
                 + (fmt == fmcg_video::Format::MOV_QTRLE
                        ? (transparent ? "  [MOV, QuickTime Animation, transparent]" : "  [MOV, QuickTime Animation]")
                        : "  [MP4, H.264, CRF " + std::to_string(s.x264_crf) + ", " + s.x264_preset
                              + (s.x264_tune.empty() ? "" : ", tune " + s.x264_tune) + "]")).c_str(), false);

    std::string err;
    fmcg_text::RGBA bg = fmcg_text::from_ass_colour(s.bg_color_aabbggrr) | 0xFFu;
    if (transparent) bg &= 0xFFFFFF00u;                       // keep the colour, alpha 0
    fmcg_video::Options vo;
    vo.format = fmt;
    vo.width = s.width;
    vo.height = s.height;
    vo.fps = s.fps;
    vo.transparent = transparent;
    vo.threads = s.ffmpeg_threads;
    vo.crf = s.x264_crf;
    vo.preset = s.x264_preset;
    vo.tune = s.x264_tune;
    vo.keyframe_seconds = s.keyframe_seconds;
    const auto t0 = std::chrono::steady_clock::now();
    double text_scale = 1.0;
    const int rc = fmcg_video::render_video(s.output_video, vo, app_text_source(), app_text_style(),
        app_placement(), bg, &app.cancel,
        [&](size_t done, size_t total) {
            app.progress.store((float)((double)done / (double)total));
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (el > 0) app.render_speed.store((float)((double)done / s.fps / el));   // x realtime
        }, &err, &text_scale);
    if (text_scale < 1.0) {
        char msg[200];
        std::snprintf(msg, sizeof msg, "Note: the largest counter text did not fit the %dx%d frame; "
                      "font scaled to %.0f%% (%.1f px) for the whole video.",
                      s.width, s.height, text_scale * 100.0, app.font_size * text_scale);
        app.gui_log(msg, false);
    }
    app.render_wallclock.store(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    if (rc == -2) {
        app.gui_log("Rendering cancelled.", false);
        app.result_ret = -2;
        app.op_result = AppState::OP_RENDER_CANCELLED;
    } else if (rc != 0) {
        app.gui_log(("Rendering failed: " + err).c_str(), true);
        app.result_ret = -1;
        app.op_result = AppState::OP_RENDER_FAIL;
    } else {
        app.result_ret = 0;
        app.op_result = AppState::OP_RENDER_OK;
        app.result_path = s.output_video;
        app.gui_log(("Video rendered to: " + s.output_video).c_str(), false);
    }
    app.progress.store(1.0f);
    app.finish_op();
}
