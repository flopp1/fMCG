// main.cpp -- fMCG GUI entry point: GLFW/OpenGL3 bootstrap and the per-frame
// window (MIDI row, the two settings panels, action buttons, progress, log).
// The panels live in pattern_panel.cpp / global_panel.cpp (panels.h).
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
#include "panels.h"

// --- per-frame drawing -------------------------------------------------------
// Extracted from the main loop so it can also run from the window-refresh
// callback: on Windows, border-drag/resize enters a modal loop that never
// returns to glfwPollEvents, and GLFW repaints through this callback there.
// Snap the live mirror (g_app) to the settings currently in the UI: everything
// that does not need a re-scan (pattern, text options, resolution, start
// delay). Preview and Render both call it, so they always use the newest
// settings. FPS, end delay and the scan options are baked into the processed
// frames; render_stale_note() reports when those changed since Process.
static void snap_live_settings() {
    set_app_pattern(current_pattern_from_ui());
    g_app.vid_width = g_ui.s.global.width;
    g_app.vid_height = g_ui.s.global.height;
    g_app.start_delay = g_ui.s.global.start_delay;
    // end_delay stays as processed: the tail frames were built with it.
}

// Settings changed since Process that only a re-process can apply ("" = none).
static std::string render_stale_note() {
    const GuiSettings& p = g_app.processed_with;
    const GuiSettings& s = g_ui.s;
    std::string what;
    auto add = [&](bool changed, const char* name) {
        if (changed) what += (what.empty() ? "" : ", ") + std::string(name);
    };
    add(std::string(g_ui.midi_buf) != p.midi_file, "MIDI file");
    add(s.global.fps != p.global.fps, "FPS");
    add(s.global.end_delay != p.global.end_delay, "end delay");
    add(s.global.vel0_note_off != p.global.vel0_note_off, "velocity-0 handling");
    add(s.global.close_held_notes != p.global.close_held_notes, "close held notes");
    add(s.global.dedup_mode != p.global.dedup_mode ||
            (s.global.dedup_mode == 2 && s.risky_dedup != p.risky_dedup), "dedup");
    if (what.empty()) return what;
    return "Note: " + what + " changed since Process; the video uses the processed values. "
           "Press Process again to apply them.";
}

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
                  "[Console]::OutputEncoding = [Text.Encoding]::UTF8; "   // UTF-8 path on stdout
                  "Add-Type -AssemblyName System.Windows.Forms; "
                  "$f = New-Object System.Windows.Forms.OpenFileDialog; "
                  "$f.Filter = 'MIDI, CJCMCG & archives (*.mid;*.midi;*.cjcmcg;*.7z;*.xz;*.rar;*.gz;*.bz2;*.zst;*.lz4)|*.mid;*.midi;*.cjcmcg;*.7z;*.xz;*.rar;*.gz;*.bz2;*.zst;*.lz4|All files (*.*)|*.*'; "
                  "$f.Title = 'Select MIDI file'; "
                  "if ($f.ShowDialog() -eq 'OK') { $f.FileName }\"";
#else
            cmd = "zenity --file-selection --title='Select MIDI file' --file-filter='MIDI, CJCMCG & archives | *.mid *.midi *.cjcmcg *.7z *.xz *.rar *.gz *.bz2 *.zst *.lz4' 2>/dev/null || "
                  "kdialog --getopenfilename . 'MIDI, CJCMCG & archives (*.mid *.midi *.cjcmcg *.7z *.xz *.rar *.gz *.bz2 *.zst *.lz4)' 'Select MIDI file' 2>/dev/null";
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
                g_ui.s.output_video = dir + stem + "_fMCG" + fmcg_video::extension((fmcg_video::Format)g_ui.s.global.output_format);
                strncpy(g_ui.output_buf, g_ui.s.output_video.c_str(), sizeof(g_ui.output_buf) - 1);
                g_ui.s.global.midi_dir = dir;
                mark_globals_dirty();
            }
        }
    }

    // The pattern picker sits in the top bar with the MIDI file.
    draw_pattern_bar();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Settings: Global | Look | Layout side by side (Look and Layout stacked
    // in one column when the window is narrow). The area takes the window's
    // height minus what the buttons, progress bar and log used last frame,
    // and every column scrolls on its own.
    static float bottom_h = 260.0f;
    const float settings_h = std::max(300.0f, ImGui::GetContentRegionAvail().y - bottom_h);
    const bool three = ImGui::GetContentRegionAvail().x >= 1200.0f;
    if (ImGui::BeginTable(three ? "##settings_cols3" : "##settings_cols2", three ? 3 : 2,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Global", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Look", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        if (three) ImGui::TableSetupColumn("Layout", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##col_global", ImVec2(0, settings_h));
        draw_global_section();
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##col_look", ImVec2(0, settings_h));
        draw_look_section();
        if (!three) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            draw_layout_section(false);
        }
        ImGui::EndChild();
        if (three) {
            ImGui::TableSetColumnIndex(2);
            ImGui::BeginChild("##col_layout", ImVec2(0, settings_h));
            draw_layout_section(true);
            ImGui::EndChild();
        }
        ImGui::EndTable();
    }
    const float bottom_start = ImGui::GetCursorPosY();
    // Draggable border between the settings and the buttons / progress / log:
    // it sets the log's height (the settings area takes the rest).
    // Height and open state are global settings (saved; collapsed by default).
    float log_h = (float)g_ui.s.global.log_height;
    static bool log_open_last = false;
    {
        ImGui::InvisibleButton("##settings_split", ImVec2(-1, 6));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemHovered() && !log_open_last) ImGui::SetTooltip("Open the log to resize it");
        if (ImGui::IsItemActive() && log_open_last) {
            const float max_h = std::max(60.0f, ImGui::GetWindowHeight() - 480.0f);
            log_h = std::max(60.0f, std::min(max_h, log_h - ImGui::GetIO().MouseDelta.y));
            if (log_h != (float)g_ui.s.global.log_height) {
                g_ui.s.global.log_height = log_h;
                mark_globals_dirty();
            }
        }
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float y = (a.y + b.y) / 2;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, y), ImVec2(b.x, y),
            ImGui::GetColorU32(hot ? ImGuiCol_SeparatorActive : ImGuiCol_Separator), hot ? 3.0f : 1.0f);
    }

    maybe_autosave_globals();

    // --- Action Buttons ---
    {
        bool can_process = (g_ui.midi_buf[0] != '\0') && !g_app.busy.load();
        bool can_render  = g_app.processed.load() && !g_app.busy.load();

        // CC is always counted, so {cc*} tokens always render real values.

        if (!can_process) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
        if (ImGui::Button("Process", ImVec2(140, 30)) && can_process) {
            g_ui.s.midi_file = g_ui.midi_buf;
            g_ui.s.output_video = g_ui.output_buf;
            commit_layout_buf();
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
            snap_live_settings();
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
            snap_live_settings();   // the newest pattern / resolution / delay

            if (g_ui.output_buf[0] == '\0') {   // same default as Process
                const std::string def = g_app.midi_dir + g_app.midi_stem + "_fMCG"
                    + fmcg_video::extension((fmcg_video::Format)g_ui.s.global.output_format);
                strncpy(g_ui.output_buf, def.c_str(), sizeof(g_ui.output_buf) - 1);
                g_ui.output_buf[sizeof(g_ui.output_buf) - 1] = '\0';
                g_ui.s.output_video = def;
            }
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
            rs.ffmpeg_threads = g_ui.s.global.ffmpeg_threads;
            rs.bg_color_aabbggrr = g_app.pattern.bg_color_aabbggrr;
            rs.output_format = g_ui.s.global.output_format;
            rs.transparent_bg = g_ui.s.global.transparent_bg;
            rs.x264_crf = g_ui.s.global.x264_crf;
            rs.x264_preset = g_ui.s.global.x264_preset;
            rs.x264_tune = g_ui.s.global.x264_tune;
            rs.keyframe_seconds = g_ui.s.global.keyframe_seconds;
            // Built here, on the UI thread: a Preview click mid-render rewrites
            // the g_app mirror the worker would otherwise read.
            rs.text_source = app_text_source();
            rs.text_style = app_text_style();
            rs.placement = app_placement();
            rs.note = render_stale_note();
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
            g_app.template_boxes.clear();
            g_app.total_notes = 0;
            g_app.total_ticks = 0;
            g_app.meter = SongMeter{};
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

    // --- Log (collapsible; opens by itself when a process or render starts) ---
    static bool was_busy = false;
    const bool busy_now = g_app.busy.load();
    if (busy_now && !was_busy) ImGui::SetNextItemOpen(true);
    else ImGui::SetNextItemOpen(g_ui.s.global.log_open, ImGuiCond_Once);   // saved state at startup
    was_busy = busy_now;
    const bool log_open = ImGui::CollapsingHeader("Log");
    if (log_open != g_ui.s.global.log_open) {
        g_ui.s.global.log_open = log_open;
        mark_globals_dirty();
    }
    log_open_last = log_open;
    if (log_open) ImGui::BeginChild("log_area", ImVec2(0, log_h), ImGuiChildFlags_None,
                                    ImGuiWindowFlags_HorizontalScrollbar);
    if (log_open) {
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
    if (log_open) ImGui::EndChild();

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

    // What the buttons, progress bar, log and status used: the settings area
    // leaves that much room next frame. A status line is always reserved so
    // the columns do not jump when one appears.
    bottom_h = std::max(ImGui::GetCursorPosY() - bottom_start, 0.0f) + ImGui::GetStyle().WindowPadding.y;
    if (!(g_app.done.load() && !g_app.busy.load())) bottom_h += ImGui::GetTextLineHeightWithSpacing();

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
    load_global_settings(g_ui.s.global);
    ensure_default_pattern();

    g_ui.system_fonts = enumerate_system_fonts();
    for (auto& f : g_ui.system_fonts) g_ui.font_cstrs.push_back(f.c_str());   // safe: system_fonts is never mutated after this

    // Bootstrap the UI from the Default pattern (or first available).
    {
        auto names = list_patterns();
        if (!names.empty()) set_active_pattern(names[0]);
        else {
            PatternData p;                 // no store available: in-memory defaults
            p.boxes.box.text = k_default_layout;
            apply_pattern_to_ui(p);
            g_ui.pattern_baseline = p;
        }
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
