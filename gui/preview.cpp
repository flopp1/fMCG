#include "preview.h"
#include "app_state.h"
#include "jobs.h"

#include "imgui.h"
#include "fmcg_text.h"
#include <GLFW/glfw3.h>

#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cmath>


#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F   // GL 1.2; Windows' gl.h stops at 1.1
#endif

namespace fmcg_preview {

// ---- WYSIWYG canvas ------------------------------------------------------------
// The preview shows the in-built renderer's own RGBA frame (src/fmcg_text), so
// font, metrics, placement and clamping are exactly what gets encoded. Only the
// changed rectangle is uploaded to the GL texture each frame.
namespace {
struct CanvasPreview {
    fmcg_text::FrameRenderer fr;
    GLuint tex = 0;
    int tex_w = 0, tex_h = 0;
    std::string key;                       // settings the canvas was built with
    std::vector<std::string> last_lines;
    bool ok = false;
    std::string err;
};
CanvasPreview g_cp;

std::string canvas_key() {
    std::ostringstream k;
    k << g_app.font_family << '|' << g_app.font_variant << '|' << g_app.font_size << '|'
      << g_app.text_colour_ass << '|' << g_app.bg_colour_ass << '|' << g_app.vid_width << 'x'
      << g_app.vid_height << '|' << g_app.pos_mode << '|' << g_app.pos_x << ',' << g_app.pos_y << '|'
      << g_app.ass_alignment << '|' << g_app.outline_enabled << ',' << g_app.outline_width << ','
      << g_app.outline_colour_ass << '|' << g_app.typo.letter_spacing << ',' << g_app.typo.line_spacing;
    for (const std::string& f : g_app.typo.features()) k << ',' << f;
    return k.str();
}

// Renders `lines` and draws the frame scaled to fit the current content region.
void draw_canvas(const std::vector<std::string>& lines) {
    const std::string key = canvas_key();
    const int W = std::max(16, g_app.vid_width), H = std::max(16, g_app.vid_height);
    if (key != g_cp.key) {
        g_cp.key = key;
        g_cp.last_lines.clear();
        const fmcg_text::TextStyle st = app_text_style();
        fmcg_text::Placement pl = app_placement();
        pl.frame_w = W; pl.frame_h = H;
        const fmcg_text::RGBA bg = fmcg_text::from_ass_colour(g_app.bg_colour_ass) | 0xFFu;   // opaque video bg
        g_cp.err.clear();
        g_cp.ok = g_cp.fr.init(st, pl, bg, &g_cp.err);
        if (!g_cp.tex) glGenTextures(1, &g_cp.tex);
        glBindTexture(GL_TEXTURE_2D, g_cp.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     g_cp.fr.canvas().px.data());
        g_cp.tex_w = W; g_cp.tex_h = H;
    }
    if (g_cp.ok && lines != g_cp.last_lines) {
        const fmcg_text::Canvas& c = g_cp.fr.render(lines);
        g_cp.last_lines = lines;
        const fmcg_text::Rect d = g_cp.fr.dirty().clipped(c.w, c.h);
        if (!d.empty()) {
            glBindTexture(GL_TEXTURE_2D, g_cp.tex);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, c.w);
            glTexSubImage2D(GL_TEXTURE_2D, 0, d.x, d.y, d.w, d.h, GL_RGBA, GL_UNSIGNED_BYTE,
                            c.px.data() + ((size_t)d.y * c.w + d.x) * 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
    }
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const float s = std::min(area.x / (float)g_cp.tex_w, area.y / (float)g_cp.tex_h);
    const ImVec2 size((float)g_cp.tex_w * s, (float)g_cp.tex_h * s);
    const ImVec2 cur = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(cur.x + (area.x - size.x) / 2, cur.y + (area.y - size.y) / 2));
    ImGui::Image((ImTextureID)(intptr_t)g_cp.tex, size);
    if (!g_cp.ok) {
        ImGui::SetCursorPos(ImVec2(cur.x + 8, cur.y + 8));
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Font error: %s", g_cp.err.c_str());
    }
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string ln;
    while (std::getline(stream, ln)) lines.push_back(ln);
    return lines;
}
} // namespace

std::string format_frame_text(const FrameStats& fs) {
    std::string result;
    // {time-max} must show the MIDI song horizon, not the end-delay-extended
    // video length (total_duration already spans the tail the engine appended).
    double max_time = g_app.total_duration - g_app.end_delay;
    if (max_time < 0.0) max_time = 0.0;
    uint64_t total_cc = g_app.frames_data.empty() ? 0 : g_app.frames_data.back().cumulative_cc;
    for (size_t i = 0; i < g_app.template_lines.size(); ++i) {
        result += ProcessTemplateLine(g_app.template_lines[i], fs, g_app.total_notes, total_cc,
                                      max_time, g_app.ppqn, g_app.preview_commas, g_app.preview_pad,
                                      g_app.preview_bpm, g_app.total_ticks);
        if (i + 1 < g_app.template_lines.size()) result += '\n';
    }
    return result;
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
                              IM_COL32(24, 24, 24, 255));   // letterbox: the frame itself carries the video background
        }

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
        draw_canvas(split_lines(text));

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
    // total_duration is frames.back().timestamp_sec: it ALREADY spans the
    // end-delay tail (engine extends frames through it), so only the start
    // delay lead-in is added here. (Adding end_delay again would desync the
    // scrub bar and the render-progress mirror with the actual video.)
    double total_len = g_app.total_duration + g_app.start_delay;   // lead-in + song + tail
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
                              IM_COL32(24, 24, 24, 255));   // letterbox: the frame itself carries the video background
        }

        // Render text: the in-built renderer's frame for this video frame
        // (same FrameTextSource as the encoder: lead-in, song and tail).
        if (!g_app.frames_data.empty() && total_len > 0) {
            const FrameTextSource src = app_text_source();
            const size_t nf = src.frame_count();
            size_t k = (size_t)std::max(0.0, std::floor(g_app.preview_time * src.fps + 1e-9));
            if (nf && k >= nf) k = nf - 1;
            draw_canvas(src.lines(k));
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
