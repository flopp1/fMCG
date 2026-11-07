// global_panel.cpp -- the left column: global settings (video size / FPS /
// delays, scan options, encoder settings, output), autosaved to settings.ini.
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

void draw_global_section() {
    // --- Global section: session-wide settings, autosaved, NOT in patterns --
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f),
        "Global settings - autosaved, separate from patterns");
    ImGui::Spacing();

    ImGui::Text("Start delay (seconds): ");
    ImGui::SameLine(170);
    ImGui::SetNextItemWidth(100);
    double delay_input = g_ui.s.global.start_delay;
    if (ImGui::InputDouble("##delay", &delay_input, 0, 0, "%.2f")) {
        if (delay_input >= 0) { g_ui.s.global.start_delay = std::min(delay_input, k_max_delay); mark_globals_dirty(); }
    }

    ImGui::Text("End delay (seconds): ");
    ImGui::SameLine(170);
    ImGui::SetNextItemWidth(100);
    double edelay_input = g_ui.s.global.end_delay;
    if (ImGui::InputDouble("##edelay", &edelay_input, 0, 0, "%.2f")) {
        if (edelay_input >= 0) { g_ui.s.global.end_delay = std::min(edelay_input, k_max_delay); mark_globals_dirty(); }
    }

    if (ImGui::Checkbox("Vel-0 as Note-Off", &g_ui.s.global.vel0_note_off)) mark_globals_dirty();
    ImGui::SameLine();
    if (ImGui::Checkbox("Close held notes", &g_ui.s.global.close_held_notes)) mark_globals_dirty();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Release notes that are still held when their track ends.\n"
                          "Off (default): polyphony is note-ons minus note-offs, as the MIDI spec defines it.\n"
                          "Affects polyphony only.");

    ImGui::Text("Resolution");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(100);
    if (ImGui::InputInt("##w", &g_ui.s.global.width, 0, 0)) { mark_globals_dirty(); }
    ImGui::SameLine();
    ImGui::Text("x");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (ImGui::InputInt("##h", &g_ui.s.global.height, 0, 0)) { mark_globals_dirty(); }
    // Encoder limits: 16..16384 px; H.264 (4:2:0) also needs even sizes.
    g_ui.s.global.width = std::max(16, std::min(16384, g_ui.s.global.width));
    g_ui.s.global.height = std::max(16, std::min(16384, g_ui.s.global.height));
    if (g_ui.s.global.output_format == 0 && ((g_ui.s.global.width | g_ui.s.global.height) & 1)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "MP4 needs an even width and height");
    }

    ImGui::Text("FPS");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(120);
    double fps_input = g_ui.s.global.fps;
    if (ImGui::InputDouble("##fps", &fps_input, 0, 0, "%.1f")) {
        if (fps_input > 0) { g_ui.s.global.fps = std::min(fps_input, k_max_fps); mark_globals_dirty(); }
    }

    ImGui::Text("Parsing threads");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(120);
    int par_input = g_ui.s.global.parse_threads;
    if (ImGui::InputInt("##parthreads", &par_input, 0, 0)) {
        // 0 = all cores; 1 = sequential (historic behaviour); negative makes no sense.
        if (par_input < 0) par_input = 0;
        if (par_input > 64) par_input = 64;
        if (par_input != g_ui.s.global.parse_threads) {
            g_ui.s.global.parse_threads = par_input;
            mark_globals_dirty();
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Worker threads for parsing plain (uncompressed) MIDI files.\n0 = all cores (default). 1 = sequential.\nCompressed inputs and the two-pass fallback always parse sequentially.");

    // Track and layered dedup are exclusive: ticking one greys the other out.
    {
        bool track = g_ui.s.global.dedup_mode == 1, layered = g_ui.s.global.dedup_mode == 2;
        if (layered) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Track dedup", &track)) {
            g_ui.s.global.dedup_mode = track ? 1 : 0;
            mark_globals_dirty();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Traditional dedup: byte-identical MIDI tracks are parsed once and replayed.\n"
                              "Works on every input: plain, compressed and nested archives (the\n"
                              "archive is still fully decompressed; parsing is what is saved).\n"
                              "Off by default. Cannot be combined with Layered dedup.");
        if (layered) ImGui::EndDisabled();
        ImGui::SameLine();
        if (track) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Layered dedup", &layered)) {
            g_ui.s.global.dedup_mode = layered ? 2 : 0;
            mark_globals_dirty();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Nested archives (xz-in-xz, repeated members): only unique members are\n"
                              "decoded and parsed, duplicates are counted by multiplicity, and uniform\n"
                              "spam grids fold analytically. Other inputs get the normal scan.\n"
                              "Off by default. Cannot be combined with Track dedup.");
        if (track) ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (g_ui.s.global.dedup_mode != 2) ImGui::BeginDisabled();
    ImGui::Checkbox("Risky fast dedup", &g_ui.s.risky_dedup);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Layered (xz-in-xz) files only, needs Layered dedup.\n"
                          "In each layer, once the first tile repeats, assume the REST of the layer is\n"
                          "that tile repeated whenever the remaining size is an exact multiple of the\n"
                          "tile's size, and skip decoding it. Much faster on huge single-tile layers.\n"
                          "WRONG (silently) if that part of the layer is made of different tiles whose\n"
                          "sizes happen to add up -- only use it on files where that cannot happen.\n"
                          "The log lists every assumption applied. Not saved: off at every start.");
    if (g_ui.s.global.dedup_mode != 2) ImGui::EndDisabled();
    if (g_ui.s.global.dedup_mode == 2 && g_ui.s.risky_dedup)
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                           "Risky: counts are wrong if a layer's rest is other tiles of matching size.");

    ImGui::Text("Encoder threads");
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(120);
    int thr_input = g_ui.s.global.ffmpeg_threads;
    if (ImGui::InputInt("##threads", &thr_input, 0, 0)) {
        // 0 = auto (x264 picks); negative makes no sense.
        if (thr_input < 0) thr_input = 0;
        if (thr_input > 64) thr_input = 64;
        if (thr_input != g_ui.s.global.ffmpeg_threads) {
            g_ui.s.global.ffmpeg_threads = thr_input;
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
        int fmt = g_ui.s.global.output_format;
        if (ImGui::Combo("##format", &fmt, k_formats, 2) && fmt != g_ui.s.global.output_format) {
            g_ui.s.global.output_format = fmt;
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
        const bool mov = g_ui.s.global.output_format == 1;
        if (!mov) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Transparent background", &g_ui.s.global.transparent_bg)) mark_globals_dirty();
        if (!mov) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("MOV only: the background is fully transparent (alpha 0),\n"
                              "so the counter can be overlaid in an editor without keying.");
    }

    // Encoder settings (global). CRF / preset / tune are H.264 only; the
    // keyframe interval applies to both formats.
    if (ImGui::TreeNode("Encoder settings")) {
        const bool mp4 = g_ui.s.global.output_format == 0;
        if (!mp4) ImGui::BeginDisabled();
        ImGui::Text("Quality (CRF)");
        ImGui::SameLine(170);
        ImGui::SetNextItemWidth(160);
        int crf = g_ui.s.global.x264_crf;
        if (ImGui::SliderInt("##crf", &crf, 0, 51, "%d", ImGuiSliderFlags_AlwaysClamp) && crf != g_ui.s.global.x264_crf) {
            g_ui.s.global.x264_crf = crf;
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
                if (g_ui.s.global.x264_preset == fmcg_video::k_presets[i]) idx = i;
            if (ImGui::Combo("##preset", &idx, fmcg_video::k_presets, np)
                && g_ui.s.global.x264_preset != fmcg_video::k_presets[idx]) {
                g_ui.s.global.x264_preset = fmcg_video::k_presets[idx];
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
                if (g_ui.s.global.x264_tune == fmcg_video::k_tunes[i]) idx = i + 1;
            if (ImGui::Combo("##tune", &idx, k_tune_labels, nt + 1)) {
                const std::string t = idx == 0 ? "" : fmcg_video::k_tunes[idx - 1];
                if (t != g_ui.s.global.x264_tune) { g_ui.s.global.x264_tune = t; mark_globals_dirty(); }
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("x264 tuning for the content. None (default) suits text overlays;\n"
                              "Still image / Animation favour flat areas and sharp edges.");
        if (!mp4) ImGui::EndDisabled();
        ImGui::Text("Keyframe every");
        ImGui::SameLine(170);
        ImGui::SetNextItemWidth(100);
        double kf = g_ui.s.global.keyframe_seconds;
        if (ImGui::InputDouble("##keyint", &kf, 0, 0, "%.2f")) {
            if (kf < 0.1) kf = 0.1;
            if (kf > 60.0) kf = 60.0;
            if (kf != g_ui.s.global.keyframe_seconds) { g_ui.s.global.keyframe_seconds = kf; mark_globals_dirty(); }
        }
        ImGui::SameLine();
        ImGui::Text("seconds");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Maximum distance between keyframes (0.1..60 s), both formats.\n"
                              "Shorter = faster seeking in editors, slightly bigger files.");
        if (ImGui::Button("Reset encoder defaults")) {
            g_ui.s.global.x264_crf = 18;
            g_ui.s.global.x264_preset = "veryfast";
            g_ui.s.global.x264_tune.clear();
            g_ui.s.global.keyframe_seconds = 2.0;
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
            // The filter follows the chosen format: a fixed MP4 filter made the
            // dialog append ".mp4" to QuickTime (MOV) output.
            const bool mov_out = g_ui.s.global.output_format == 1;
#if defined(_WIN32)
            cmd = std::string("powershell -NoProfile -Command \""
                  "[Console]::OutputEncoding = [Text.Encoding]::UTF8; "   // UTF-8 path on stdout
                  "Add-Type -AssemblyName System.Windows.Forms; "
                  "$f = New-Object System.Windows.Forms.SaveFileDialog; ")
                  + (mov_out ? "$f.Filter = 'QuickTime movie (*.mov)|*.mov|All files (*.*)|*.*'; "
                             : "$f.Filter = 'MP4 video (*.mp4)|*.mp4|All files (*.*)|*.*'; ")
                  + "$f.Title = 'Select output video'; "
                  "if ($f.ShowDialog() -eq 'OK') { $f.FileName }\"";
#else
            cmd = std::string("zenity --file-selection --save --title='Select output video' --file-filter='")
                  + (mov_out ? "QuickTime movie | *.mov" : "MP4 video | *.mp4") + "' 2>/dev/null || "
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

