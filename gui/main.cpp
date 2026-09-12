// main.cpp -- fMCG GUI entry point: GLFW/OpenGL3 bootstrap, the settings UI,
// action buttons, progress bar and log view. Worker operations live in jobs.cpp,
// the preview popup in preview.cpp, dialogs in dialogs.cpp, shared state in
// app_state.h/cpp.
#include "app_state.h"
#include "jobs.h"
#include "dialogs.h"
#include "preview.h"

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

int main() {
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(1100, 800, "fMCG - Fast MIDI Counter Generator", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

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

    std::vector<std::string> system_fonts = enumerate_system_fonts();
    std::vector<const char*> font_cstrs;
    for (auto& f : system_fonts) font_cstrs.push_back(f.c_str());

    GuiSettings settings;
    settings.font_family = "Arial";

    char midi_buf[1024] = "";
    char layout_buf[4096] =
        "Time: {time-milli}/{time-milli-max}\n"
        "Notes: {nc}/{nc-total}/{nc-rem}\n"
        "NPS: {nps}/{nps-max}\n"
        "Polyphony: {plph}/{plph-max}\n"
        "BPM: {bpm}";
    char output_buf[1024] = "";

    int alignment_idx = 0;
    const char* alignment_items[] = {"Top Left", "Top Right", "Bottom Left", "Bottom Right"};

    struct ColourPreset { const char* name; const char* ass; };
    ColourPreset colour_presets[] = {
        {"White",  "&H00FFFFFF"},
        {"Yellow", "&H0000FFFF"},
        {"Cyan",   "&H00FFFF00"},
        {"Green",  "&H0000FF00"},
        {"Red",    "&H000000FF"},
        {"Blue",   "&H00FF0000"},
    };
    int num_colour_presets = 6;
    char custom_colour_buf[32] = "FFFFFF";
    char custom_bg_buf[32] = "000000";   // RRGGBB for the video background

    bool font_list_loaded = false;
    int selected_font_idx = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        process_preview_font_reload();   // font/atlas changes must happen outside the frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
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
        ImGui::InputText("##midi", midi_buf, sizeof(midi_buf), ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();

        {
            bool dialog_active = g_app.dialog_busy.load();
            if (dialog_active) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
            if (ImGui::Button("Browse") && !dialog_active) {
                std::string cmd;
#if defined(_WIN32)
                cmd = "powershell -NoProfile -Command \""
                      "Add-Type -AssemblyName System.Windows.Forms; "
                      "$f = New-Object System.Windows.Forms.OpenFileDialog; "
                      "$f.Filter = 'MIDI & archives (*.mid;*.midi;*.7z;*.xz;*.rar)|*.mid;*.midi;*.7z;*.xz;*.rar|All files (*.*)|*.*'; "
                      "$f.Title = 'Select MIDI file'; "
                      "if ($f.ShowDialog() -eq 'OK') { $f.FileName }\"";
#elif defined(__APPLE__)
                cmd = "osascript -e 'tell application \"System Events\" to set f to (choose file with prompt \"Select MIDI file\") as alias' -e 'POSIX path of f'";
#else
                cmd = "zenity --file-selection --title='Select MIDI file' --file-filter='MIDI & archives | *.mid *.midi *.7z *.xz *.rar' 2>/dev/null || "
                      "kdialog --getopenfilename . 'MIDI & archives (*.mid *.midi *.7z *.xz *.rar)' 'Select MIDI file' 2>/dev/null";
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
                    settings.midi_file = result;
                    strncpy(midi_buf, result.c_str(), sizeof(midi_buf) - 1);
                    std::string dir = extract_dir(result);
                    std::string stem = extract_stem(result);
                    settings.output_video = dir + stem + "_fMCG.mp4";
                    strncpy(output_buf, settings.output_video.c_str(), sizeof(output_buf) - 1);
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Settings ---
        // Font family + weight/variant share one row to stay compact.
        ImGui::Text("Font Family");
        if (!font_list_loaded && !font_cstrs.empty()) font_list_loaded = true;
        bool fam_changed = false;
        if (!font_cstrs.empty()) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 130);
            if (ImGui::Combo("##font", &selected_font_idx, font_cstrs.data(), (int)font_cstrs.size()))
                fam_changed = true;
            settings.font_family = system_fonts[selected_font_idx];
        } else {
            char font_buf[256];
            strncpy(font_buf, settings.font_family.c_str(), sizeof(font_buf) - 1);
            font_buf[sizeof(font_buf) - 1] = '\0';
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 130);
            if (ImGui::InputText("##font_custom", font_buf, sizeof(font_buf)))
                fam_changed = true;
            settings.font_family = font_buf;
        }

        // Style/weight combo on the same row, populated from the family's files.
        static std::vector<std::string> variant_names;
        static std::vector<const char*> variant_cstrs;
        static std::string variants_for;
        static int variant_idx = 0;
        if (fam_changed || variants_for != settings.font_family) {
            variants_for = settings.font_family;
            variant_names.clear();
            variant_cstrs.clear();
            for (const auto& v : enumerate_font_variants(settings.font_family)) {
                variant_names.push_back(v.style);
                variant_cstrs.push_back(variant_names.back().c_str());
            }
            variant_idx = 0;
            for (size_t i = 0; i < variant_names.size(); ++i)
                if (variant_names[i] == settings.font_variant) variant_idx = (int)i;
        }
        ImGui::SameLine();
        float style_w = 125.0f;
        if (variant_cstrs.empty()) {
            ImGui::SetNextItemWidth(style_w);
            ImGui::TextDisabled("(no styles)");
        } else {
            if ((size_t)variant_idx >= variant_names.size()) variant_idx = 0;
            ImGui::SetNextItemWidth(style_w);
            if (ImGui::Combo("##fstyle", &variant_idx, variant_cstrs.data(), (int)variant_cstrs.size())) {
                settings.font_variant = variant_names[variant_idx];
                // Bold/italic flags for the ASS style come from the variant's
                // OS/2 weight class and fsSelection bit.
                settings.font_bold = 0;
                settings.font_italic = 0;
                for (const auto& v : enumerate_font_variants(settings.font_family)) {
                    if (v.style == settings.font_variant) {
                        settings.font_bold = (v.weight >= 600) ? 1 : 0;
                        settings.font_italic = v.italic ? 1 : 0;
                        break;
                    }
                }
            }
        }

        ImGui::Text("Font Size");
        ImGui::SameLine(120);
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##fsize", &settings.font_size, 1, 10);

        ImGui::Text("Resolution");
        ImGui::SameLine(120);
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("##w", &settings.width, 0, 0);
        ImGui::SameLine();
        ImGui::Text("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("##h", &settings.height, 0, 0);

        ImGui::Text("FPS");
        ImGui::SameLine(120);
        ImGui::SetNextItemWidth(120);
        double fps_input = settings.fps;
        if (ImGui::InputDouble("##fps", &fps_input, 0, 0, "%.1f"))
            if (fps_input > 0) settings.fps = fps_input;

        ImGui::Text("Text Colour");
        ImGui::Spacing();
        for (int i = 0; i < num_colour_presets; ++i) {
            ImGui::PushID(i);
            ImVec4 cols[] = {
                {1,1,1,1}, {1,1,0,1}, {0,1,1,1}, {0,1,0,1}, {1,0,0,1}, {0,0,1,1}
            };
            if (ImGui::ColorButton(("##cpreset" + std::to_string(i)).c_str(),
                    cols[i], 0, ImVec2(24, 24))) {
                settings.text_color_aabbggrr = colour_presets[i].ass;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", colour_presets[i].name);
            ImGui::SameLine();
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::Text("Custom (RRGGBB):");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        if (ImGui::InputText("##ccol", custom_colour_buf, sizeof(custom_colour_buf))) {
            std::string hex(custom_colour_buf);
            if (hex.size() == 6) {
                // User enters RRGGBB; ASS stores &H00BBGGRR
                std::string bbggrr = hex.substr(4, 2) + hex.substr(2, 2) + hex.substr(0, 2);
                settings.text_color_aabbggrr = "&H00" + bbggrr;
            }
        }

        ImGui::Spacing();
        ImGui::Text("Background Colour");
        ImGui::Spacing();
        {
            static const char* bg_names[] = {"Black", "White", "Dark Grey", "Navy"};
            static const char* bg_ass[]  = {"&H00000000", "&H00FFFFFF", "&H00202020", "&H00000080"};
            static const ImVec4 bg_cols[] = {{0,0,0,1},{1,1,1,1},{0.125f,0.125f,0.125f,1},{0,0,0.5f,1}};
            for (int i = 0; i < 4; ++i) {
                ImGui::PushID(100 + i);
                if (ImGui::ColorButton(("##bgpreset" + std::to_string(i)).c_str(),
                        bg_cols[i], 0, ImVec2(24, 24))) {
                    settings.bg_color_aabbggrr = bg_ass[i];
                    strncpy(custom_bg_buf, bg_ass[i] + 4, 6);   // tail = BBGGRR
                    custom_bg_buf[6] = '\0';
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", bg_names[i]);
                ImGui::SameLine();
                ImGui::PopID();
            }
            ImGui::Spacing();
            ImGui::Text("Custom (RRGGBB):");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160);
            if (ImGui::InputText("##bcol", custom_bg_buf, sizeof(custom_bg_buf))) {
                std::string hex(custom_bg_buf);
                if (hex.size() == 6) {
                    std::string bbggrr = hex.substr(4, 2) + hex.substr(2, 2) + hex.substr(0, 2);
                    settings.bg_color_aabbggrr = "&H00" + bbggrr;
                }
            }
        }

        ImGui::Spacing();
        ImGui::Text("Comma separators:");
        ImGui::SameLine();
        ImGui::Checkbox("Notes##c", &settings.commas.notes);
        ImGui::SameLine();
        ImGui::Checkbox("Poly##c", &settings.commas.polyphony);
        ImGui::SameLine();
        ImGui::Checkbox("NPS##c", &settings.commas.nps);
        ImGui::SameLine();
        ImGui::Checkbox("CC##c", &settings.commas.cc);
        ImGui::Checkbox("Vel-0 as Note-Off", &settings.vel0_note_off);
        ImGui::Checkbox("Count CC events (enables {cc} stats)", &settings.cc_stats);
        ImGui::Checkbox("Leading zeros (pad each stat to its own maximum)", &settings.pad.enabled);

        ImGui::Text("Counter position:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Corners##pm", settings.pos_mode == 0)) settings.pos_mode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Custom x,y##pm", settings.pos_mode == 1)) settings.pos_mode = 1;
        if (settings.pos_mode == 0) {
            ImGui::SetNextItemWidth(160);
            ImGui::Combo("##align", &alignment_idx, alignment_items, 4);
            settings.alignment = alignment_idx;
        } else {
            ImGui::Text("x");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::InputInt("##posx", &settings.pos_x, 0, 0);
            ImGui::SameLine();
            ImGui::Text("y");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::InputInt("##posy", &settings.pos_y, 0, 0);
            ImGui::TextDisabled("(top-left of the text block, video pixels)");
        }

        ImGui::Text("Start delay (seconds): ");
        ImGui::SameLine(170);
        ImGui::SetNextItemWidth(100);
        double delay_input = settings.start_delay;
        if (ImGui::InputDouble("##delay", &delay_input, 0, 0, "%.2f"))
            if (delay_input >= 0) settings.start_delay = delay_input;

        ImGui::Spacing();
        ImGui::Text("Output Path");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##output", output_buf, sizeof(output_buf));

        ImGui::Text("Layout (one line per stat row; {nc}, {time-milli}, {bpm}, ...)");
        ImGui::InputTextMultiline("##layout", layout_buf, sizeof(layout_buf), ImVec2(-1, 120));
        {
            bool cfg_dialog_active = g_app.dialog_busy.load();
            if (cfg_dialog_active) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
            if (ImGui::Button("Load from file...##cfg") && !cfg_dialog_active) {
                std::string cfg_cmd;
#if defined(_WIN32)
                cfg_cmd = "powershell -NoProfile -Command \""
                      "Add-Type -AssemblyName System.Windows.Forms; "
                      "$f = New-Object System.Windows.Forms.OpenFileDialog; "
                      "$f.Filter = 'Text files (*.txt)|*.txt|All files (*.*)|*.*'; "
                      "$f.Title = 'Select layout template'; "
                      "if ($f.ShowDialog() -eq 'OK') { $f.FileName }\"";
#elif defined(__APPLE__)
                cfg_cmd = "osascript -e 'tell application \"System Events\" to set f to (choose file with prompt \"Select layout template\") as alias' -e 'POSIX path of f'";
#else
                cfg_cmd = "zenity --file-selection --title='Select layout template' 2>/dev/null || "
                      "kdialog --getopenfilename . 'Text files (*.txt)' 'Select layout template' 2>/dev/null";
#endif
                launch_dialog(cfg_cmd, "layout");
            }
            if (cfg_dialog_active) ImGui::PopStyleVar();
            ImGui::SameLine();
            if (ImGui::Button("Reset to default##cfg")) {
                strncpy(layout_buf,
                        "Time: {time-milli}/{time-milli-max}\n"
                        "Notes: {nc}/{nc-total}/{nc-rem}\n"
                        "NPS: {nps}/{nps-max}\n"
                        "Polyphony: {plph}/{plph-max}\n"
                        "BPM: {bpm}",
                        sizeof(layout_buf) - 1);
            }
        }

        // Poll layout dialog result
        {
            std::string result = poll_dialog_result("layout");
            if (!result.empty()) {
                std::ifstream lf(result.c_str());
                std::string content, line;
                while (std::getline(lf, line)) {
                    content += line;
                    content += '\n';
                }
                strncpy(layout_buf, content.c_str(), sizeof(layout_buf) - 1);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Action Buttons ---
        {
            bool can_process = (midi_buf[0] != '\0') && !g_app.busy.load();
            bool can_render  = g_app.processed.load() && !g_app.busy.load();

            if (!can_process) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
            if (ImGui::Button("Process", ImVec2(140, 30)) && can_process) {
                settings.midi_file = midi_buf;
                settings.output_video = output_buf;
                settings.layout_text = layout_buf;
                g_app.processed = false;
                g_app.done = false;
                g_app.log_lines.clear();
                g_app.preview_playing = false;
                g_app.preview_time = 0.0;
                g_app.busy = true;  // set before detach so the button disables immediately
                std::thread(run_process, settings).detach();
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
                    if (g_app.render_active) g_app.gui_log("Cancelling ffmpeg...", false);
                }
            }

            // Preview opens whenever data exists — also while an ffmpeg render
            // runs (live progress view), and after the popup was closed.
            bool can_preview = g_app.processed.load() && (!g_app.busy.load() || g_app.render_active);
            bool preview_starts_render_view = g_app.render_active && g_app.busy.load();
            ImGui::SameLine();
            if (!can_preview) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
            if (ImGui::Button("Preview", ImVec2(140, 30)) && can_preview) {
                g_app.show_preview = true;
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
                g_app.preview_playing = false;
                g_app.preview_time = 0.0;
                ImGui::OpenPopup("Preview");

                RenderSettings rs;
                rs.output_video = output_buf;
                rs.midi_dir = g_app.midi_dir;
                rs.midi_stem = g_app.midi_stem;
                rs.width = g_app.vid_width;
                rs.height = g_app.vid_height;
                rs.fps = g_app.fps;
                rs.total_duration = g_app.total_duration + g_app.start_delay;
                rs.bg_color_aabbggrr = g_app.bg_colour_ass;
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
            char pct[32];
            snprintf(pct, sizeof(pct), "%d%%", (int)(p * 100));
            ImGui::ProgressBar(p, ImVec2(-1, 0), overlay.empty() ? pct : overlay.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Log ---
        ImGui::Text("Log Output:");
        ImGui::BeginChild("log_area", ImVec2(0, ImGui::GetContentRegionAvail().y - 25), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lock(g_app.log_mutex);
            // While a scan is live, the last log entry is a self-overwriting
            // status line (per the user's request) instead of a growing list.
            std::string scan_line;
            bool scan_live = false;
            {
                std::lock_guard<std::mutex> slock(g_app.scan_mutex);
                scan_live = g_app.scan_active && g_app.busy.load() && g_app.processed.load() == false;
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
        ImGui::EndChild();

        if (g_app.done.load() && !g_app.processed.load() && !g_app.busy.load()) {
            if (g_app.result_ret.load() == 0) {
                uint64_t fsize = 0;
                {
                    std::ifstream rf(g_app.result_path, std::ios::binary | std::ios::ate);
                    if (rf.is_open()) fsize = (uint64_t)rf.tellg();
                }
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Rendered: %s (%s)",
                    g_app.result_path.c_str(), format_file_size(fsize).c_str());
            } else if (g_app.result_ret.load() != -1)
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Render failed. Check log.");
        }

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

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
