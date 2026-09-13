#include "preview.h"
#include "app_state.h"
#include "jobs.h"

#include "imgui.h"
#include <GLFW/glfw3.h>

#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cmath>


namespace fmcg_preview {

std::string format_frame_text(const FrameStats& fs) {
    std::string result;
    double max_time = g_app.total_duration;
    uint64_t total_cc = g_app.frames_data.empty() ? 0 : g_app.frames_data.back().cumulative_cc;
    for (size_t i = 0; i < g_app.template_lines.size(); ++i) {
        result += ProcessTemplateLine(g_app.template_lines[i], fs, g_app.total_notes, total_cc,
                                      max_time, g_app.ppqn, g_app.preview_commas, g_app.preview_pad,
                                      g_app.preview_bpm, g_app.total_ticks);
        if (i + 1 < g_app.template_lines.size()) result += '\n';
    }
    return result;
}

void process_preview_font_reload() {
    if (!g_app.preview_font_reload.exchange(false)) return;
    if (g_app.font_family.empty()) return;
    std::string key = g_app.font_family + "|" + g_app.font_variant;
    std::string file = find_font_file_for_variant(g_app.font_family, g_app.font_variant);
    if (file.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    g_app.preview_font = io.Fonts->AddFontFromFileTTF(file.c_str(), 36.0f);
    if (g_app.preview_font)
        g_app.preview_baked_family = key;
}



// ---------------------------------------------------------------------------
// Preview helpers
// ---------------------------------------------------------------------------

ImVec4 ass_colour_to_imgui(const std::string& ass_col) {
    // ASS colour format: &HAABBGGRR
    if (ass_col.size() >= 8 && ass_col[0] == '&' && (ass_col[1] == 'H' || ass_col[1] == 'h')) {
        std::string hex = ass_col.substr(2);
        if (hex.size() == 8) hex = hex.substr(2);   // drop alpha byte -> BBGGRR
        if (hex.size() == 6) {
            unsigned int b = std::stoul(hex.substr(0, 2), nullptr, 16);
            unsigned int g = std::stoul(hex.substr(2, 2), nullptr, 16);
            unsigned int r = std::stoul(hex.substr(4, 2), nullptr, 16);
            return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
        }
    }
    return ImVec4(1, 1, 1, 1);
}

ImU32 ass_colour_to_imcol32(const std::string& ass_col) {
    ImVec4 c = ass_colour_to_imgui(ass_col);
    return IM_COL32((int)(c.x * 255.0f), (int)(c.y * 255.0f), (int)(c.z * 255.0f), 255);
}

const FrameStats& find_frame(double time_sec) {
    const auto& data = g_app.frames_data;
    if (data.empty()) {
        static FrameStats empty;
        return empty;
    }
    size_t lo = 0, hi = data.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (data[mid].timestamp_sec <= time_sec)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0) return data[0];
    return data[lo - 1];
}

// ---------------------------------------------------------------------------
// Pattern-only preview: shown when no MIDI is processed. Renders the current
// pattern with all stats at zero over the background colour; a short fixed
// timeline lets the user see the start-delay countdown behaviour too.
// ---------------------------------------------------------------------------

void render_pattern_only_popup() {
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float popup_w = display.x * 0.6f;
    float popup_h = display.y * 0.6f;
    if (popup_w < 480) popup_w = 480;
    if (popup_h < 320) popup_h = 320;
    double total_len = 10.0;
    double total_len_song = total_len - g_app.start_delay;   // MIDI-time span
    if (total_len_song < 0.0) total_len_song = 0.0;

    ImGui::SetNextWindowSize(ImVec2(popup_w, popup_h), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2((display.x - popup_w) / 2, (display.y - popup_h) / 2), ImGuiCond_FirstUseEver);

    if (ImGui::BeginPopupModal("Preview", &g_app.show_preview,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar)) {

        if (g_app.preview_playing) {
            double elapsed = glfwGetTime() - g_app.preview_start_time;
            g_app.preview_time = g_app.preview_start_pos + elapsed;
            if (g_app.preview_time >= total_len) {
                g_app.preview_time = total_len;
                g_app.preview_playing = false;
            }
        }

        // Slider shows MIDI (song) time: negative during the start-delay
        // countdown, crossing 0 when the song begins -- the same time base
        // the stats themselves display, so bar and text never disagree.
        float song_t = (float)(g_app.preview_time - g_app.start_delay);

        ImGui::BeginChild("pattern_only", ImVec2(0, ImGui::GetContentRegionAvail().y - 34),
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 wp0 = ImGui::GetCursorScreenPos();
            ImVec2 area0 = ImGui::GetContentRegionAvail();
            dl->AddRectFilled(wp0, ImVec2(wp0.x + area0.x, wp0.y + area0.y),
                              ass_colour_to_imcol32(g_app.bg_colour_ass));
        }

        float preview_scale = (g_app.vid_width > 0) ? ImGui::GetContentRegionAvail().x / (float)g_app.vid_width : 0.0f;
        float desired_px = (float)g_app.font_size * preview_scale;
        if (desired_px < 4.0f) desired_px = 4.0f;
        if (desired_px > 256.0f) desired_px = 256.0f;
        if (!g_app.preview_font && !g_app.preview_font_reload.load() && !g_app.font_family.empty())
            g_app.preview_font_reload = true;

        // Layout fallback: the mirror is normally snapped on Preview click, but
        // guard anyway -- an empty template list would render nothing and leave
        // the cursor moved with no item submitted (ImGui assert).
        if (g_app.template_lines.empty()) {
            g_app.template_lines.push_back("Time: {time-milli}/{time-milli-max}");
            g_app.template_lines.push_back("Notes: {nc}/{nc-total}/{nc-rem}");
            g_app.template_lines.push_back("NPS: {nps}/{nps-max}");
            g_app.template_lines.push_back("Polyphony: {plph}/{plph-max}");
            g_app.template_lines.push_back("BPM: {bpm}");
        }

        FrameStats zero_fs;
        zero_fs.timestamp_sec = g_app.preview_time - g_app.start_delay;
        std::string text = format_frame_text(zero_fs);
        std::vector<std::string> lines;
        {
            std::istringstream stream(text);
            std::string ln;
            while (std::getline(stream, ln)) lines.push_back(ln);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ass_colour_to_imgui(g_app.text_colour_ass));
        if (g_app.preview_font) ImGui::PushFont(g_app.preview_font, desired_px);
        float line_h = ImGui::GetTextLineHeightWithSpacing();
        float total_text_h = line_h * (float)lines.size();
        float area_w = ImGui::GetContentRegionAvail().x;
        float area_h = ImGui::GetContentRegionAvail().y;
        float margin = 30.0f * preview_scale;
        if (margin < 4.0f) margin = 4.0f;

        float max_tw = 0;
        for (auto& ln : lines) {
            float w = ImGui::CalcTextSize(ln.c_str()).x;
            if (w > max_tw) max_tw = w;
        }

        int a = g_app.ass_alignment;
        bool left  = (a == 7 || a == 1 || a == 8 || a == 2);   // L, and centers
        bool top   = (a == 7 || a == 9 || a == 8);
        float x_off, y_off;
        if (g_app.pos_mode == 1) {
            // Mirror the generator's bottom-right clamp: shift the anchor so
            // the whole block stays visible (same 0.62em/char estimate).
            float est_w = 0.0f;
            for (auto& ln : lines) {
                float w = (float)ln.size() * 0.62f * (float)g_app.font_size * preview_scale;
                if (w > est_w) est_w = w;
            }
            float est_h = (float)lines.size() * 1.50f * (float)g_app.font_size * preview_scale;
            x_off = (float)g_app.pos_x * preview_scale;
            y_off = (float)g_app.pos_y * preview_scale;
            if (x_off > area_w - est_w) x_off = area_w - est_w;
            if (y_off > area_h - est_h) y_off = area_h - est_h;
            if (x_off < 0) x_off = 0;
            if (y_off < 0) y_off = 0;
        } else {
            x_off = left ? ((a == 8 || a == 2) ? area_w / 2 - max_tw / 2 : margin)
                         : area_w - max_tw - margin;
            y_off = top  ? margin : area_h - total_text_h - margin;
            if (y_off < margin) y_off = margin;
            if (x_off < margin) x_off = margin;
        }
        // Hard clamp: never place the cursor at/after the child's edge, or
        // ImGui asserts on cursor-beyond-boundaries when text is empty.
        if (x_off > area_w - 1.0f) x_off = area_w - 1.0f;
        if (y_off > area_h - 1.0f) y_off = area_h - 1.0f;
        if (x_off < 0.0f) x_off = 0.0f;
        if (y_off < 0.0f) y_off = 0.0f;

        ImGui::SetCursorPos(ImVec2(x_off, y_off));
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) ImGui::SetCursorPosX(x_off);
            ImGui::TextUnformatted(lines[i].c_str());
        }
        if (g_app.preview_font) ImGui::PopFont();
        ImGui::PopStyleColor();
        // Submit an item so the window grows over any cursor movement (the
        // SetCursorPos + empty-text combination would otherwise assert).
        ImGui::Dummy(ImVec2(4.0f, 4.0f));

        ImGui::EndChild();

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
        float tp = song_t;
        if (ImGui::SliderFloat("##ptime", &tp, (float)-g_app.start_delay, (float)total_len_song, "%.2fs")) {
            g_app.preview_time = (double)tp + g_app.start_delay;
            if (g_app.preview_playing) {
                g_app.preview_start_pos = g_app.preview_time;
                g_app.preview_start_time = glfwGetTime();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(g_app.preview_playing ? "Pause" : "Play", ImVec2(50, 0))) {
            if (g_app.preview_playing) g_app.preview_playing = false;
            else {
                g_app.preview_playing = true;
                g_app.preview_start_time = glfwGetTime();
                g_app.preview_start_pos = g_app.preview_time;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(50, 0))) {
            g_app.preview_playing = false;
            g_app.preview_time = 0.0;
        }

        ImGui::EndPopup();
    }
}

void render_preview_popup() {
    if (!g_app.show_preview) return;

    ImVec2 display = ImGui::GetIO().DisplaySize;

    // No MIDI processed: pattern-only preview (stats at zero). The timeline
    // is a fixed 10s so the user can still inspect placement/colours.
    if (g_app.frames_data.empty()) {
        render_pattern_only_popup();
        return;
    }

    // Compute popup size matching output aspect ratio
    double total_len = g_app.total_duration + g_app.start_delay + g_app.end_delay;   // lead-in + song + tail
    double total_len_song = total_len - g_app.start_delay;         // song time span (incl. tail)
    if (total_len_song < 0.0) total_len_song = 0.0;
    float max_w = display.x * 0.8f;
    float max_h = display.y * 0.85f;
    float popup_w, popup_h;
    if (g_app.vid_width > 0 && g_app.vid_height > 0) {
        float aspect = (float)g_app.vid_width / (float)g_app.vid_height;
        popup_w = max_w;
        popup_h = popup_w / aspect;
        if (popup_h > max_h) {
            popup_h = max_h;
            popup_w = popup_h * aspect;
        }
    } else {
        popup_w = max_w;
        popup_h = max_h;
    }
    if (popup_w < 320) popup_w = 320;
    if (popup_h < 240) popup_h = 240;

    ImGui::SetNextWindowSize(ImVec2(popup_w, popup_h), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2((display.x - popup_w) / 2, (display.y - popup_h) / 2), ImGuiCond_FirstUseEver);

    if (ImGui::BeginPopupModal("Preview", &g_app.show_preview,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar)) {

        // Space toggles play/pause when a preview is playing (not during a
        // render, where space has no meaning and the user may be typing).
        bool rendering_early = g_app.render_active && g_app.busy.load();
        if (!rendering_early && ImGui::IsKeyPressed(ImGuiKey_Space) &&
            !g_app.frames_data.empty()) {
            if (g_app.preview_playing) {
                g_app.preview_playing = false;
            } else {
                g_app.preview_playing = true;
                g_app.preview_start_time = glfwGetTime();
                g_app.preview_start_pos = g_app.preview_time;
            }
        }

        // Update preview time (timeline spans start_delay + song duration)
        if (g_app.render_active && g_app.busy.load() && total_len > 0) {
            g_app.preview_time = g_app.progress.load() * total_len;
            if (g_app.preview_time > total_len) g_app.preview_time = total_len;
        } else if (!g_app.render_watch_done.load()) {
            // Normal (timeline) view: remember where the user is so the render
            // view -- which borrows preview_time as its progress -- can put it
            // back when it finishes. Skipped on the completion frame itself:
            // preview_time still holds the render's final position there.
            g_app.preview_saved_pos = g_app.preview_time;
        }
        if (g_app.preview_playing && !g_app.frames_data.empty() && total_len > 0) {
            double elapsed = glfwGetTime() - g_app.preview_start_time;
            g_app.preview_time = g_app.preview_start_pos + elapsed;
            if (g_app.preview_time >= total_len) {
                g_app.preview_time = total_len;
                g_app.preview_playing = false;
            }
        }

        bool rendering = g_app.render_active && g_app.busy.load();
        float controls_h = rendering ? 28.0f : 50.0f;
        float preview_h = ImGui::GetContentRegionAvail().y - controls_h;

        // Preview child window
        ImGui::BeginChild("preview_render", ImVec2(0, preview_h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        // Video background colour via draw list
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 wp0 = ImGui::GetCursorScreenPos();
            ImVec2 area = ImGui::GetContentRegionAvail();
            dl->AddRectFilled(wp0, ImVec2(wp0.x + area.x, wp0.y + area.y),
                              ass_colour_to_imcol32(g_app.bg_colour_ass));
        }

        // Render text
        if (!g_app.frames_data.empty() && total_len > 0) {
            // Scale video-resolution values (ASS Fontsize / 30px margins, PlayRes
            // = video width x height) into the preview area, which shows the
            // full video frame scaled by preview_width / video_width.
            float preview_scale = (g_app.vid_width > 0) ? ImGui::GetContentRegionAvail().x / (float)g_app.vid_width : 0.0f;
            float desired_px = (float)g_app.font_size * preview_scale;
            if (desired_px < 4.0f) desired_px = 4.0f;
            if (desired_px > 256.0f) desired_px = 256.0f;
            if (!g_app.preview_font && !g_app.preview_font_reload.load() && !g_app.font_family.empty())
                g_app.preview_font_reload = true;   // picked up before the next frame

            // Start-delay lead-in: stats all at zero, current-time fields run
            // negative and count up to 0, same text the video shows there.
            // Same child + controls as the song branch (unified layout).
            bool in_delay = g_app.preview_time < g_app.start_delay;
            FrameStats delay_fs;
            const FrameStats* fs_ptr;
            if (in_delay) {
                delay_fs.bpm = g_app.frames_data.front().bpm;
                delay_fs.timestamp_sec = g_app.preview_time - g_app.start_delay;
                // Negative song ticks counting up to 0, matching the ASS
                // delay block: linear back-extrapolation at the initial
                // tempo from the song's first frame (safe even when the
                // first tempo change sits after tick 0).
                const FrameStats& front = g_app.frames_data.front();
                const double us0 = 60000000.0 / (front.bpm > 0.0 ? front.bpm : 120.0);
                delay_fs.tick = front.tick
                    + (int64_t)std::llround(delay_fs.timestamp_sec
                        * (double)g_app.ppqn * 1e6 / us0);
                fs_ptr = &delay_fs;
            } else {
                // Song and end-delay tail alike: the engine's frames vector
                // already carries tail frames (resting stats, counting time,
                // last-tempo ticks); find_frame returns the latest one at or
                // before the song time.
                fs_ptr = &find_frame(g_app.preview_time - g_app.start_delay);
            }
            const FrameStats& fs = *fs_ptr;
            std::string text = format_frame_text(fs);

            std::vector<std::string> lines;
            {
                std::istringstream stream(text);
                std::string ln;
                while (std::getline(stream, ln)) lines.push_back(ln);
            }

            ImGui::PushStyleColor(ImGuiCol_Text, ass_colour_to_imgui(g_app.text_colour_ass));
            if (g_app.preview_font) ImGui::PushFont(g_app.preview_font, desired_px);
            float line_h = ImGui::GetTextLineHeightWithSpacing();
            float total_text_h = line_h * (float)lines.size();
            float area_w = ImGui::GetContentRegionAvail().x;
            float area_h = ImGui::GetContentRegionAvail().y;
            float margin = 30.0f * preview_scale;
            if (margin < 4.0f) margin = 4.0f;

            float max_tw = 0;
            for (auto& ln : lines) {
                float w = ImGui::CalcTextSize(ln.c_str()).x;
                if (w > max_tw) max_tw = w;
            }

            int a = g_app.ass_alignment;
            bool left = (a == 7 || a == 1 || a == 8 || a == 2);   // L, and centers
            bool top  = (a == 7 || a == 9 || a == 8);

            float x_off, y_off;
            if (g_app.pos_mode == 1) {
                // Explicit placement: (pos_x,pos_y) in video pixels anchors the
                // text block's TOP-LEFT (matches the \pos(...,7) in the ASS).
                // Bottom-right clamp parity with the generator (0.62em/char).
                float est_w = 0.0f;
                for (auto& ln : lines) {
                    float w = (float)ln.size() * 0.62f * (float)g_app.font_size * preview_scale;
                    if (w > est_w) est_w = w;
                }
                float est_h = (float)lines.size() * 1.50f * (float)g_app.font_size * preview_scale;
                x_off = (float)g_app.pos_x * preview_scale;
                y_off = (float)g_app.pos_y * preview_scale;
                if (x_off > area_w - est_w) x_off = area_w - est_w;
                if (y_off > area_h - est_h) y_off = area_h - est_h;
                if (x_off < 0) x_off = 0;
                if (y_off < 0) y_off = 0;
            } else {
                x_off = left ? ((a == 8 || a == 2) ? area_w / 2 - max_tw / 2 : margin)
                             : area_w - max_tw - margin;
                y_off = top  ? margin : area_h - total_text_h - margin;
                if (y_off < margin) y_off = margin;
                if (x_off < margin) x_off = margin;
            }

            ImGui::SetCursorPos(ImVec2(x_off, y_off));
            for (size_t i = 0; i < lines.size(); ++i) {
                if (i > 0) ImGui::SetCursorPosX(x_off);
                ImGui::TextUnformatted(lines[i].c_str());
            }
            if (g_app.preview_font) ImGui::PopFont();
            ImGui::PopStyleColor();
        } else {
            const char* msg = "No preview data";
            ImVec2 ts = ImGui::CalcTextSize(msg);
            ImVec2 area = ImGui::GetContentRegionAvail();
            ImGui::SetCursorPos(ImVec2((area.x - ts.x) / 2, (area.y - ts.y) / 2));
            ImGui::TextDisabled("%s", msg);
        }

        ImGui::EndChild();

        // Controls
        if (rendering) {
            float p = g_app.display_progress();
            char overlay[32];
            snprintf(overlay, sizeof(overlay), "Rendering %d%%", (int)(p * 100));
            ImGui::ProgressBar(p, ImVec2(-1, 0), overlay);
        } else {
            // Render finished: a popup opened to watch the render closes
            // itself; one opened via Preview stays as a regular timeline view.
            // finish_op() latches render_watch_done BEFORE clearing
            // render_active/busy, so this never races: those atomics may
            // already be false by the frame after completion, but the latch
            // survives until consumed here.
            if (g_app.render_watch_done.exchange(false)) {
                g_app.preview_playing = false;
                // The render view borrowed preview_time as its progress; hand
                // it back at where the normal timeline last was (0 if the
                // timeline was never opened this session).
                g_app.preview_time = g_app.preview_saved_pos;
                if (g_app.preview_watching_render) {
                    g_app.preview_watching_render = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            // Scrub bar in MIDI (song) time: negative during the start-delay
            // countdown, 0 at the song's first frame -- matching the stats'
            // own time fields.
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
            float song_t = (float)(g_app.preview_time - g_app.start_delay);
            float tp = song_t;
            if (ImGui::SliderFloat("##time", &tp, (float)-g_app.start_delay, (float)total_len_song, "%.2fs")) {
                g_app.preview_time = (double)tp + g_app.start_delay;
                if (g_app.preview_playing) {
                    g_app.preview_start_pos = g_app.preview_time;
                    g_app.preview_start_time = glfwGetTime();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(g_app.preview_playing ? "Pause" : "Play", ImVec2(50, 0))) {
                if (g_app.preview_playing) {
                    g_app.preview_playing = false;
                } else {
                    g_app.preview_playing = true;
                    g_app.preview_start_time = glfwGetTime();
                    g_app.preview_start_pos = g_app.preview_time;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop", ImVec2(50, 0))) {
                g_app.preview_playing = false;
                g_app.preview_time = 0.0;
            }
        }

        ImGui::EndPopup();
    }
}


} // namespace fmcg_preview
