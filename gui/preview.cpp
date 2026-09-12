#include "preview.h"
#include "app_state.h"
#include "jobs.h"

#include "imgui.h"
#include <GLFW/glfw3.h>

#include <sstream>
#include <algorithm>
#include <cstdio>


namespace fmcg_preview {

std::string format_frame_text(const FrameStats& fs) {
    std::string result;
    double max_time = g_app.total_duration;
    uint64_t total_cc = g_app.frames_data.empty() ? 0 : g_app.frames_data.back().cumulative_cc;
    for (size_t i = 0; i < g_app.template_lines.size(); ++i) {
        result += ProcessTemplateLine(g_app.template_lines[i], fs, g_app.total_notes, total_cc,
                                      max_time, g_app.ppqn, g_app.preview_commas, g_app.preview_pad);
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

        ImGui::SetCursorPos(ImVec2(x_off, y_off));
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) ImGui::SetCursorPosX(x_off);
            ImGui::TextUnformatted(lines[i].c_str());
        }
        if (g_app.preview_font) ImGui::PopFont();
        ImGui::PopStyleColor();

        ImGui::EndChild();

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
        float tp = (float)g_app.preview_time;
        if (ImGui::SliderFloat("##ptime", &tp, 0.0f, (float)total_len, "%.2fs")) {
            g_app.preview_time = tp;
            if (g_app.preview_playing) {
                g_app.preview_start_pos = tp;
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
    double total_len = g_app.total_duration + g_app.start_delay;   // includes black lead-in
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

        // Update preview time (timeline spans start_delay + song duration)
        if (g_app.render_active && g_app.busy.load() && total_len > 0) {
            g_app.preview_time = g_app.progress.load() * total_len;
            if (g_app.preview_time > total_len) g_app.preview_time = total_len;
        } else if (g_app.preview_playing && !g_app.frames_data.empty() && total_len > 0) {
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
            // Start-delay lead-in: stats all at zero, current-time fields run
            // negative and count up to 0 — same text the video shows there.
            if (g_app.preview_time < g_app.start_delay) {
                ImGui::EndChild();
                ImGui::PushStyleColor(ImGuiCol_Text, ass_colour_to_imgui(g_app.text_colour_ass));
                if (g_app.preview_font) ImGui::PushFont(g_app.preview_font, std::max(4.0f, std::min(256.0f, (float)g_app.font_size * (g_app.vid_width > 0 ? ImGui::GetContentRegionAvail().x / (float)g_app.vid_width : 0.0f))));
                FrameStats zero_fs;
                zero_fs.bpm = g_app.frames_data.front().bpm;
                zero_fs.timestamp_sec = g_app.preview_time - g_app.start_delay;
                std::string text = format_frame_text(zero_fs);
                std::vector<std::string> lines;
                {
                    std::istringstream stream(text);
                    std::string ln;
                    while (std::getline(stream, ln)) lines.push_back(ln);
                }
                // Match the video's placement (same geometry as the song branch).
                float preview_scale2 = (g_app.vid_width > 0) ? ImGui::GetContentRegionAvail().x / (float)g_app.vid_width : 0.0f;
                float margin2 = std::max(4.0f, 30.0f * preview_scale2);
                float max_tw2 = 0;
                for (auto& ln : lines) {
                    float w = ImGui::CalcTextSize(ln.c_str()).x;
                    if (w > max_tw2) max_tw2 = w;
                }
                float area_w2 = ImGui::GetContentRegionAvail().x;
                float area_h2 = ImGui::GetContentRegionAvail().y;
                float line_h2 = ImGui::GetTextLineHeightWithSpacing();
                float total_text_h2 = line_h2 * (float)lines.size();
                int a2 = g_app.ass_alignment;
                bool left2 = (a2 == 7 || a2 == 1 || a2 == 8 || a2 == 2);
                bool top2  = (a2 == 7 || a2 == 9 || a2 == 8);
                float x2, y2;
                if (g_app.pos_mode == 1) {
                    // Same bottom-right clamp parity as the generator.
                    float est_w2 = 0.0f;
                    for (auto& ln : lines) {
                        float w = (float)ln.size() * 0.62f * (float)g_app.font_size * preview_scale2;
                        if (w > est_w2) est_w2 = w;
                    }
                    float est_h2 = (float)lines.size() * 1.50f * (float)g_app.font_size * preview_scale2;
                    x2 = (float)g_app.pos_x * preview_scale2;
                    y2 = (float)g_app.pos_y * preview_scale2;
                    if (x2 > area_w2 - est_w2) x2 = area_w2 - est_w2;
                    if (y2 > area_h2 - est_h2) y2 = area_h2 - est_h2;
                    if (x2 < 0) x2 = 0;
                    if (y2 < 0) y2 = 0;
                } else {
                    x2 = left2 ? ((a2 == 8 || a2 == 2) ? area_w2 / 2 - max_tw2 / 2 : margin2)
                               : area_w2 - max_tw2 - margin2;
                    y2 = top2  ? margin2 : area_h2 - total_text_h2 - margin2;
                    if (y2 < margin2) y2 = margin2;
                    if (x2 < margin2) x2 = margin2;
                }
                ImGui::SetCursorPos(ImVec2(x2, y2));
                for (size_t i = 0; i < lines.size(); ++i) {
                    if (i > 0) ImGui::SetCursorPosX(x2);
                    ImGui::TextUnformatted(lines[i].c_str());
                }
                if (g_app.preview_font) ImGui::PopFont();
                ImGui::PopStyleColor();

                if (rendering) {
                    float p = g_app.display_progress();
                    char overlay[32];
                    snprintf(overlay, sizeof(overlay), "Rendering %d%%", (int)(p * 100));
                    ImGui::ProgressBar(p, ImVec2(-1, 0), overlay);
                } else {
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
                    float tp = (float)g_app.preview_time;
                    if (ImGui::SliderFloat("##time", &tp, 0.0f, (float)total_len, "%.2fs")) {
                        g_app.preview_time = tp;
                        if (g_app.preview_playing) {
                            g_app.preview_start_pos = tp;
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
                }
                ImGui::EndPopup();
                return;
            }
            // Scale video-resolution values (ASS Fontsize / 30px margins, PlayRes
            // = video width x height) into the preview area, which shows the
            // full video frame scaled by preview_width / video_width.
            float preview_scale = (g_app.vid_width > 0) ? ImGui::GetContentRegionAvail().x / (float)g_app.vid_width : 0.0f;
            float desired_px = (float)g_app.font_size * preview_scale;
            if (desired_px < 4.0f) desired_px = 4.0f;
            if (desired_px > 256.0f) desired_px = 256.0f;
            if (!g_app.preview_font && !g_app.preview_font_reload.load() && !g_app.font_family.empty())
                g_app.preview_font_reload = true;   // picked up before the next frame

            const FrameStats& fs = find_frame(g_app.preview_time - g_app.start_delay);
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
            bool left = (a == 7 || a == 1);
            bool top  = (a == 7 || a == 9);

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
                if (left)  x_off = margin;
                else       x_off = area_w - max_tw - margin;
                if (top)   y_off = margin;
                else       y_off = area_h - total_text_h - margin;
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
            // Render finished in the background: clear the flag once. The popup
            // stays open (unless it was opened for the render itself) so Preview
            // can always bring it back.
            if (g_app.render_active && !g_app.busy.load()) {
                g_app.render_active = false;
                g_app.preview_playing = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
            float tp = (float)g_app.preview_time;
            if (ImGui::SliderFloat("##time", &tp, 0.0f, (float)total_len, "%.2fs")) {
                g_app.preview_time = tp;
                if (g_app.preview_playing) {
                    g_app.preview_start_pos = tp;
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
