// Popup reproduction harness: replicates fMCG_gui.cpp's preview popup exactly,
// renders a few frames, then dumps diagnostics + pixel counts of the child area.
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"
#include <GLFW/glfw3.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <string>
#include <vector>

// ---- replica of GUI preview state ----
struct FrameStats { size_t frame_index = 0; double timestamp_sec = 0.0; uint64_t cumulative_notes = 0;
                    double notes_per_second = 0.0; double peak_nps = 0.0; int64_t polyphony = 0;
                    int64_t peak_polyphony = 0; double bpm = 120.0; };
static std::vector<FrameStats> g_frames_data;
static std::vector<std::string> g_template_lines;
static std::string g_text_colour_ass = "&H00FFFFFF";
static int g_ass_alignment = 7;
static bool g_show_preview = false;
static bool g_render_active = false;
static bool g_preview_playing = false;
static double g_preview_time = 0.0;
static double g_preview_start_time = 0.0;
static double g_preview_start_pos = 0.0;
static double g_total_duration = 10.0;
static int g_vid_width = 1920, g_vid_height = 1080;
static std::atomic<bool> g_busy{false};
static std::atomic<float> g_progress{0.0f};

// ---- preview font baking (mirrors planned GUI logic) ----
static ImFont*      g_preview_font = nullptr;
static float        g_preview_font_px = 0.0f;
static std::string  g_preview_font_file;
static float        g_preview_pending_px = 0.0f;
static int          g_font_size = 36;   // video-resolution font size (ASS Fontsize)

static void request_preview_font(float px, const char* file) {
    g_preview_pending_px = px;
    g_preview_font_file = file;
}

static void process_preview_font_rebake() {
    if (g_preview_pending_px <= 0.0f) return;
    g_preview_pending_px = 0.0f;
    if (g_preview_font) return;          // TTF already loaded; size is applied per-frame
    if (g_preview_font_file.empty()) return;
    printf("[rebake] loading preview font from %s\n", g_preview_font_file.c_str());
    ImGuiIO& io = ImGui::GetIO();
    g_preview_font = io.Fonts->AddFontFromFileTTF(g_preview_font_file.c_str(), 36.0f);
}

static std::string format_frame_text(const FrameStats& fs) {
    char buf[256];
    snprintf(buf, sizeof(buf), "Time: %.2fs\nNotes: %llu\nNPS: %.0f\nPoly: %lld",
             fs.timestamp_sec, (unsigned long long)fs.cumulative_notes,
             fs.notes_per_second, (long long)fs.polyphony);
    return buf;
}

static const FrameStats& find_frame(double time_sec) {
    if (g_frames_data.empty()) { static FrameStats e; return e; }
    size_t lo = 0, hi = g_frames_data.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_frames_data[mid].timestamp_sec <= time_sec) lo = mid + 1; else hi = mid;
    }
    return g_frames_data[lo == 0 ? 0 : lo - 1];
}

static ImVec4 ass_colour_to_imgui(const std::string& ass_col) {
    // ASS colour format: &HAABBGGRR
    if (ass_col.size() >= 8 && ass_col[0] == '&' && (ass_col[1] == 'H' || ass_col[1] == 'h')) {
        std::string hex = ass_col.substr(2);
        if (hex.size() == 8) hex = hex.substr(2);   // drop alpha byte
        if (hex.size() == 6) {
            unsigned int b = std::stoul(hex.substr(0, 2), nullptr, 16);
            unsigned int g = std::stoul(hex.substr(2, 2), nullptr, 16);
            unsigned int r = std::stoul(hex.substr(4, 2), nullptr, 16);
            return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
        }
    }
    return ImVec4(1, 1, 1, 1);
}

static ImVec2 g_child_min, g_child_max;
static bool g_text_branch = false;
static int g_lines_drawn = 0;
static int g_variant = 0;

static void render_preview_popup() {
    if (!g_show_preview) return;

    ImVec2 display = ImGui::GetIO().DisplaySize;
    float max_w = display.x * 0.8f;
    float max_h = display.y * 0.85f;
    float popup_w, popup_h;
    if (g_vid_width > 0 && g_vid_height > 0) {
        float aspect = (float)g_vid_width / (float)g_vid_height;
        popup_w = max_w; popup_h = popup_w / aspect;
        if (popup_h > max_h) { popup_h = max_h; popup_w = popup_h * aspect; }
    } else { popup_w = max_w; popup_h = max_h; }
    if (popup_w < 320) popup_w = 320;
    if (popup_h < 240) popup_h = 240;

    ImGui::SetNextWindowSize(ImVec2(popup_w, popup_h), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2((display.x - popup_w) / 2, (display.y - popup_h) / 2), ImGuiCond_FirstUseEver);

    bool open;
    if (g_variant == 3)
        open = ImGui::BeginPopup("Preview", ImGuiWindowFlags_NoScrollbar);
    else
        open = ImGui::BeginPopupModal("Preview", &g_show_preview,
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);
    if (open) {
        if (g_render_active && g_busy.load() && g_total_duration > 0) {
            g_preview_time = g_progress.load() * g_total_duration;
            if (g_preview_time > g_total_duration) g_preview_time = g_total_duration;
        } else if (g_preview_playing && !g_frames_data.empty() && g_total_duration > 0) {
            double elapsed = glfwGetTime() - g_preview_start_time;
            g_preview_time = g_preview_start_pos + elapsed;
            if (g_preview_time >= g_total_duration) {
                g_preview_time = g_total_duration;
                g_preview_playing = false;
            }
        }

        bool rendering = g_render_active && g_busy.load();
        float controls_h = rendering ? 28.0f : 50.0f;
        float preview_h = ImGui::GetContentRegionAvail().y - controls_h;

        ImGui::BeginChild("preview_render", ImVec2(0, preview_h), 0,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        { // black background
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 wp0 = ImGui::GetCursorScreenPos();
            ImVec2 area = ImGui::GetContentRegionAvail();
            dl->AddRectFilled(wp0, ImVec2(wp0.x + area.x, wp0.y + area.y), IM_COL32(0, 0, 0, 255));
        }

        g_text_branch = false; g_lines_drawn = 0;
        if (!g_frames_data.empty() && g_total_duration > 0) {
            g_text_branch = true;
            // Scale ASS video-resolution values into the preview area
            float area_w0 = ImGui::GetContentRegionAvail().x;
            float scale = (g_vid_width > 0) ? area_w0 / (float)g_vid_width : 0.0f;
            float desired_px = g_font_size * scale;
            if (desired_px < 4.0f) desired_px = 4.0f;
            if (desired_px > 256.0f) desired_px = 256.0f;
            if (!g_preview_font || desired_px - g_preview_font_px > 0.75f || g_preview_font_px - desired_px > 0.75f) {
                static const char* candidates[] = {
                    "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeui.ttf",
                    "C:/Windows/Fonts/times.ttf", "C:/Windows/Fonts/calibri.ttf" };
                if (g_preview_font_file.empty())
                    for (const char* c : candidates) {
                        struct stat st;
                        if (stat(c, &st) == 0) { request_preview_font(desired_px, c); break; }
                    }
                else
                    request_preview_font(desired_px, g_preview_font_file.c_str());
            }
            const FrameStats& fs = find_frame(g_preview_time);
            std::string text = format_frame_text(fs);
            std::vector<std::string> lines;
            std::string cur;
            for (char c : text) { if (c == '\n') { lines.push_back(cur); cur.clear(); } else cur += c; }
            lines.push_back(cur);

            float line_h = ImGui::GetTextLineHeightWithSpacing();
            float total_text_h = line_h * (float)lines.size();
            float area_w = ImGui::GetContentRegionAvail().x;
            float area_h = ImGui::GetContentRegionAvail().y;
            float margin = 30.0f * scale;          // ASS MarginL/R/V = 30 video px
            if (margin < 4.0f) margin = 4.0f;
            float max_tw = 0;
            for (auto& ln : lines) { float w = ImGui::CalcTextSize(ln.c_str()).x; if (w > max_tw) max_tw = w; }
            int a = g_ass_alignment;
            bool left = (a == 7 || a == 1); bool top = (a == 7 || a == 9);
            float x_off = left ? margin : area_w - max_tw - margin;
            float y_off = top ? margin : area_h - total_text_h - margin;
            if (y_off < margin) y_off = margin;
            if (x_off < margin) x_off = margin;
            ImVec4 text_col = ass_colour_to_imgui(g_text_colour_ass);
            ImGui::PushStyleColor(ImGuiCol_Text, text_col);
            if (g_preview_font) ImGui::PushFont(g_preview_font, desired_px);
            ImGui::SetCursorPos(ImVec2(x_off, y_off));
            for (size_t i = 0; i < lines.size(); ++i) {
                if (i > 0) ImGui::SetCursorPosX(x_off);
                ImGui::TextUnformatted(lines[i].c_str());
            }
            if (g_preview_font) ImGui::PopFont();
            ImGui::PopStyleColor();
            g_lines_drawn = (int)lines.size();
        }
        g_child_min = ImGui::GetWindowPos();
        g_child_max = ImVec2(g_child_min.x + ImGui::GetWindowWidth(), g_child_min.y + ImGui::GetWindowHeight());

        ImGui::EndChild();

        if (!rendering) {
            if (g_render_active && !g_busy.load()) { // GUI's close-on-race branch
                printf("[popup] RACE-CLOSE triggered (render_active && !busy) -> closing popup\n");
                g_render_active = false;
                g_preview_playing = false;
                g_show_preview = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
            float tp = (float)g_preview_time;
            ImGui::SliderFloat("##time", &tp, 0.0f, (float)g_total_duration, "%.2fs");
            ImGui::SameLine();
            ImGui::Button(g_preview_playing ? "Pause" : "Play", ImVec2(50, 0));
            ImGui::SameLine();
            ImGui::Button("Stop", ImVec2(50, 0));
        }
        ImGui::EndPopup();
    }
}

static void count_pixels(GLFWwindow* window, int frame) {
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    std::vector<unsigned char> px((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    int x0 = (int)g_child_min.x, x1 = (int)g_child_max.x;
    int y0 = h - (int)g_child_max.y, y1 = h - (int)g_child_min.y;
    long bright = 0, total = 0;
    for (int y = y0; y < y1 && y >= 0 && y < h; ++y)
        for (int x = x0; x < x1 && x >= 0 && x < w; ++x) {
            const unsigned char* p = &px[((size_t)y * w + x) * 4];
            total++;
            if (p[0] > 40 || p[1] > 40 || p[2] > 40) bright++;
        }
    printf("[frame %d] child (%d,%d)-(%d,%d) %dx%d bright=%ld/%ld open=%d text=%d lines=%d\n",
           frame, x0, y0, x1, y1, x1 - x0, y1 - y0, bright, total,
           (int)g_show_preview, (int)g_text_branch, g_lines_drawn);
    if (ImGuiWindow* w = ImGui::FindWindowByName("Preview")) {
        printf("    Preview window: Active=%d WasActive=%d Hidden=%d SkipItems=%d Appearing=%d pos=(%.0f,%.0f) size=(%.0f,%.0f) flags=%08X\n",
               w->Active, w->WasActive, w->Hidden, w->SkipItems, w->Appearing,
               w->Pos.x, w->Pos.y, w->Size.x, w->Size.y, w->Flags);
    } else {
        printf("    Preview window: NOT FOUND in window pool\n");
    }
    char name[64]; snprintf(name, sizeof(name), "popup_frame%d.png", frame);
    stbi_flip_vertically_on_write(1);
    stbi_write_png(name, w, h, 4, px.data(), w * 4);
}

int main(int argc, char** argv) {
    int scenario = (argc > 1 && argv[1][0] == 'r') ? 1 : 0;
    int variant  = (argc > 2) ? atoi(argv[2]) : 0;
    g_variant = variant;
    printf("variant %d: %s\n", variant,
        variant == 0 ? "modal inside main window (verbatim GUI)" :
        variant == 1 ? "modal inside main window, no NoBringToFrontOnFocus" :
        variant == 2 ? "modal begun OUTSIDE main window Begin/End" :
                       "non-modal BeginPopup inside main window");
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1100, 800, "popup_test", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    for (int i = 0; i < 600; ++i) {
        FrameStats fs;
        fs.frame_index = i; fs.timestamp_sec = i / 60.0;
        fs.cumulative_notes = (uint64_t)i * 12345;
        fs.notes_per_second = 500000 + i * 100;
        fs.polyphony = 1000 + i * 3; fs.peak_polyphony = 3000;
        fs.bpm = 125;
        g_frames_data.push_back(fs);
    }

    for (int frame = 0; frame < 14; ++frame) {
        glfwPollEvents();
        process_preview_font_rebake();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize((g_variant == 4) ? ImVec2(500, 300) : ImGui::GetIO().DisplaySize);
        int mw_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
        if (g_variant != 1 && g_variant != 4) mw_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("fMCG", nullptr, mw_flags);
        ImGui::Text("main window content");

        if (frame == 2) {
            if (scenario == 0) {
                g_show_preview = true;
                g_preview_playing = true;
                g_preview_start_time = glfwGetTime();
                g_preview_start_pos = g_preview_time;
                ImGui::OpenPopup("Preview");
                printf("[frame 2] PREVIEW click: OpenPopup\n");
            } else {
                g_show_preview = true;
                g_render_active = true;
                g_preview_playing = false;
                g_preview_time = 0.0;
                ImGui::OpenPopup("Preview");
                std::thread([]{ std::this_thread::sleep_for(std::chrono::milliseconds(300)); g_busy = true; }).detach();
                printf("[frame 2] RENDER click: OpenPopup, busy in 300ms\n");
            }
        }

        if (g_variant == 2) {
            ImGui::End();
            render_preview_popup();
        } else {
            render_preview_popup();
            ImGui::End();
        }

        ImGui::Render();
        if (frame == 8) {
            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            std::vector<unsigned char> px((size_t)w * h * 4);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            auto samp = [&](int sx, int sy) {
                const unsigned char* p = &px[((size_t)(h - sy) * w + sx) * 4];
                printf("    pixel(%4d,%4d) = (%3d,%3d,%3d,%3d)\n", sx, sy, p[0], p[1], p[2], p[3]);
            };
            printf("pixel samples:\n");
            samp(10, 10);       // main window bg
            samp(600, 400);     // child center
            samp(135, 225);     // first text char area
            samp(600, 760);     // below popup
        }
        if (frame == 8) {
            ImDrawData* dd = ImGui::GetDrawData();
            printf("draw lists: %d\n", dd->CmdListsCount);
            for (int li = 0; li < dd->CmdListsCount; ++li) {
                ImDrawList* l = dd->CmdLists[li];
                printf("  list %d: vtx=%d idx=%d cmds=%d\n",
                       li, l->VtxBuffer.Size, l->IdxBuffer.Size, l->CmdBuffer.Size);
                for (int vi = 0; vi < l->VtxBuffer.Size && vi < 8; ++vi)
                    printf("    vtx %d: pos=(%.0f,%.0f) col=%08X\n", vi,
                           l->VtxBuffer[vi].pos.x, l->VtxBuffer[vi].pos.y,
                           l->VtxBuffer[vi].col);
                for (int ci = 0; ci < l->CmdBuffer.Size && ci < 8; ++ci) {
                    const ImDrawCmd& c = l->CmdBuffer[ci];
                    printf("    cmd %d: elems=%d clip=(%.0f,%.0f)-(%.0f,%.0f)\n", ci,
                           (int)c.ElemCount, c.ClipRect.x, c.ClipRect.y, c.ClipRect.z, c.ClipRect.w);
                }
            }
        }
        int dw, dh;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (frame == 4 || frame == 8) count_pixels(window, frame);
        glfwSwapBuffers(window);
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
