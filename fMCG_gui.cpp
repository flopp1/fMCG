#include "fMCG_core.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include <thread>
#include <mutex>
#include <atomic>
#include <sstream>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static std::mutex                g_log_mutex;
static std::vector<std::string> g_log_lines;
static std::atomic<float>       g_progress{0.0f};
static std::atomic<bool>        g_busy{false};
static std::atomic<bool>        g_done{false};
static std::atomic<bool>        g_cancel{false};   // user-requested abort of process/render
static std::string              g_result_path;
static std::atomic<int>         g_result_ret{-1};

// Processed state
static std::atomic<bool>        g_processed{false};
static std::string              g_ass_path;
static std::string              g_midi_dir;
static std::string              g_midi_stem;
static double                   g_total_duration{0.0};
static double                   g_fps{60.0};
static int                      g_vid_width{1920};
static int                      g_vid_height{1080};
static uint16_t                 g_ppqn{480};
static uint64_t                 g_total_notes{0};

// Preview data
static std::vector<FrameStats>    g_frames_data;
static std::string                g_layout_text;         // template editor content (persistent)
static std::vector<std::string>   g_template_lines;
static std::string                g_text_colour_ass{"&H00FFFFFF"};
static int                        g_ass_alignment{7};
static int                        g_font_size{36};       // video-resolution font size (ASS Fontsize)
static std::string                g_font_family;         // family used for the last processed render
static bool                       g_show_preview{false};
static bool                       g_render_active{false};  // true while FFmpeg render is in progress
static bool                       g_preview_playing{false};
static double                     g_preview_time{0.0};
static double                     g_start_delay{0.0};   // black lead-in before the song starts
static int                        g_pos_mode{0};        // 0 = corners, 1 = explicit x/y
static int                        g_pos_x{30};
static int                        g_pos_y{30};
static int                        g_alignment{1};       // corner dropdown (GUI index)
static double                     g_preview_start_time{0.0};
static double                     g_preview_start_pos{0.0};

// Preview font (same TTF family as the rendered video, drawn at a size scaled to
// match the video: preview_px = g_font_size * preview_width / video_width)
static ImFont*                    g_preview_font{nullptr};
static std::string                g_preview_baked_family; // family g_preview_font was loaded from
static std::atomic<bool>          g_preview_font_reload{false};

// Async file dialog state
static std::atomic<bool>        g_dialog_busy{false};
static std::atomic<bool>        g_dialog_done{false};
static std::string              g_dialog_result;
static std::string              g_dialog_kind;   // which UI element opened the dialog

static void gui_log(const char* msg, bool is_error) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::string line = msg;
    while (!line.empty() && line.back() == '\n') line.pop_back();
    if (!line.empty()) g_log_lines.push_back(line);
}

static void gui_progress(const char* /*pass_name*/, int percent) {
    g_progress.store(percent / 100.0f);
}

// Continuous scan-progress state (written by the processing thread, read by the
// UI thread). Rendered as a single log line that overwrites itself.
static std::mutex  g_scan_mutex;
static uint64_t    g_scan_events = 0;
static double      g_scan_elapsed = 0.0;
static double      g_scan_evps = 0.0;
static double      g_scan_frac = -1.0;   // stream fraction 0..1, <0 when unknown
static bool        g_scan_active = false;

static std::string compose_scan_line() {
    std::ostringstream ss;
    ss << format_with_commas(g_scan_events) << " events, ";
    if (g_scan_evps > 0) ss << format_with_commas((uint64_t)g_scan_evps) << " ev/s, ";
    ss << std::fixed << std::setprecision(1) << g_scan_elapsed << "s elapsed";
    return ss.str();
}

static void gui_scan_progress(uint64_t events, double elapsed_sec, double ev_per_s, double frac) {
    std::lock_guard<std::mutex> lock(g_scan_mutex);
    g_scan_events = events;
    g_scan_elapsed = elapsed_sec;
    g_scan_evps = ev_per_s;
    g_scan_frac = frac;
    g_scan_active = true;
}

// End-of-operation bookkeeping shared by process and render: hide the live
// scan stats (progress bar returns to percentage-only) and mark completion.
static void finish_op() {
    {
        std::lock_guard<std::mutex> lock(g_scan_mutex);
        g_scan_active = false;
    }
    g_busy = false;
    g_done = true;
}

// Progress fraction for the bar: the ffmpeg/render side drives g_progress
// directly; during the scan we derive a smooth value from the stream fraction.
static float display_progress() {
    float p = g_progress.load();
    std::lock_guard<std::mutex> lock(g_scan_mutex);
    if (g_scan_active && g_scan_frac >= 0.0)
        p = (float)(g_scan_frac * 0.85);   // scan = 85% of the process, then ASS/render
    return p;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string get_ass_path(const std::string& midi_file) {
    std::string dir = extract_dir(midi_file);
    std::string stem = extract_stem(midi_file);
    return dir + stem + "_fMCG.ass";
}

static ImVec4 ass_colour_to_imgui(const std::string& ass_col) {
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

static ImU32 ass_colour_to_imcol32(const std::string& ass_col) {
    ImVec4 c = ass_colour_to_imgui(ass_col);
    return IM_COL32((int)(c.x * 255.0f), (int)(c.y * 255.0f), (int)(c.z * 255.0f), 255);
}

static const FrameStats& find_frame(double time_sec) {
    if (g_frames_data.empty()) {
        static FrameStats empty;
        return empty;
    }
    size_t lo = 0, hi = g_frames_data.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_frames_data[mid].timestamp_sec <= time_sec)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0) return g_frames_data[0];
    return g_frames_data[lo - 1];
}

// Preview mirror of the options the last Process ran with (updated in
// run_process, read by the preview popup).
static CommaOpts g_preview_commas;
static PadOpts   g_preview_pad;
static std::string g_bg_colour_ass{"&H00000000"};   // video background colour

static std::string format_frame_text(const FrameStats& fs) {
    std::string result;
    double max_time = g_total_duration;
    uint64_t total_cc = g_frames_data.empty() ? 0 : g_frames_data.back().cumulative_cc;
    for (size_t i = 0; i < g_template_lines.size(); ++i) {
        result += ProcessTemplateLine(g_template_lines[i], fs, g_total_notes, total_cc,
                                      max_time, g_ppqn, g_preview_commas, g_preview_pad);
        if (i + 1 < g_template_lines.size()) result += '\n';
    }
    return result;
}

// Load the preview font for the current family. Deferred out of the frame: the
// family->file lookup walks the font directories, and atlas additions must not
// happen mid-frame. Size is applied per-frame via PushFont (dynamic atlas).
static void process_preview_font_reload() {
    if (!g_preview_font_reload.exchange(false)) return;
    if (g_font_family.empty() || g_font_family == g_preview_baked_family) return;
    std::string file = find_font_file_for_family(g_font_family);
    if (file.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    g_preview_font = io.Fonts->AddFontFromFileTTF(file.c_str(), 36.0f);
    if (g_preview_font)
        g_preview_baked_family = g_font_family;
}

// ---------------------------------------------------------------------------
// Async file dialog (non-blocking)
// ---------------------------------------------------------------------------

static void run_file_dialog(const std::string& cmd) {   // called with g_dialog_kind set
    FILE* pipe =
#if defined(_WIN32)
        _popen(cmd.c_str(), "r");
#else
        popen(cmd.c_str(), "r");
#endif
    std::string result;
    if (pipe) {
        char buffer[1024];
        while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
#if defined(_WIN32)
        _pclose(pipe);
#else
        pclose(pipe);
#endif
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
        result.pop_back();
    g_dialog_result = result;
    g_dialog_done = true;
    g_dialog_busy = false;
}

static void launch_dialog(const std::string& cmd, const std::string& kind) {
    if (g_dialog_busy.load()) return;
    g_dialog_result.clear();
    g_dialog_kind = kind;
    g_dialog_done = false;
    g_dialog_busy = true;
    std::thread(run_file_dialog, cmd).detach();
}

static std::string poll_dialog_result(const std::string& kind) {
    if (g_dialog_done.load() && g_dialog_kind == kind) {
        g_dialog_done = false;
        g_dialog_kind.clear();
        std::string r = g_dialog_result;
        g_dialog_result.clear();
        return r;
    }
    return "";
}

// ---------------------------------------------------------------------------
// Processing wrapper
// ---------------------------------------------------------------------------

struct GuiSettings {
    std::string midi_file;
    std::string output_video;
    std::string layout_text;
    std::string font_family;
    int         font_size = 36;
    int         alignment = 1;
    int         width = 1920;
    int         height = 1080;
    double      fps = 60.0;
    CommaOpts   commas;
    PadOpts     pad;
    bool        cc_stats = false;
    double      start_delay = 3.0;
    // 0 = corner alignment dropdown, 1 = explicit x/y (top-left of text block)
    int         pos_mode = 0;
    int         pos_x = 30;
    int         pos_y = 30;
    bool        vel0_note_off = true;
    std::string text_color_aabbggrr = "&H00FFFFFF";
    std::string bg_color_aabbggrr = "&H00000000";
};

static void run_process(GuiSettings s) {
    g_busy = true;
    g_done = false;
    g_progress.store(0.0f);
    g_log_lines.clear();
    g_result_path.clear();
    g_processed = false;

    int ass_alignment;
    if (s.alignment == 0)      ass_alignment = 7;
    else if (s.alignment == 1) ass_alignment = 9;
    else if (s.alignment == 2) ass_alignment = 1;
    else                       ass_alignment = 3;

    std::string text_color_ass = s.text_color_aabbggrr;

    std::string midi_dir = extract_dir(s.midi_file);
    std::string midi_stem = extract_stem(s.midi_file);

    if (s.output_video.empty())
        s.output_video = midi_dir + midi_stem + "_fMCG.mp4";

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

    ProgressCallbacks cb;
    cb.on_progress = gui_progress;
    cb.on_log = gui_log;
    cb.on_scan_progress = gui_scan_progress;
    cb.cc_stats = s.cc_stats;
    cb.cancel_flag = &g_cancel;
    g_cancel = false;

    {
        std::lock_guard<std::mutex> lock(g_scan_mutex);
        g_scan_events = 0; g_scan_elapsed = 0.0; g_scan_evps = 0.0;
        g_scan_frac = -1.0; g_scan_active = false;
    }

    gui_log(("Processing MIDI: " + s.midi_file + "...").c_str(), false);

    auto frames = ScaleMidiProcessor::process_midi(s.midi_file, s.fps, ppqn, total_notes,
                                                    s.vel0_note_off, cb);
    if (frames.empty()) {
        gui_log(g_cancel.load() ? "Processing cancelled." : "Error: Could not parse MIDI file.",
                g_cancel.load() ? false : true);
        g_result_ret = 1;
        finish_op();
        return;
    }

    g_progress.store(0.90f);

    double total_duration = frames.back().timestamp_sec;
    uint64_t total_cc = frames.back().cumulative_cc;
    std::string ass_filename = get_ass_path(s.midi_file);

    AssConfig acfg;
    acfg.width = s.width;
    acfg.height = s.height;
    acfg.fps = s.fps;
    acfg.font_size = s.font_size;
    acfg.font_family = s.font_family;
    acfg.text_color_ass = text_color_ass;
    acfg.ass_alignment = ass_alignment;
    acfg.pos_mode = s.pos_mode;
    acfg.pos_x = s.pos_x;
    acfg.pos_y = s.pos_y;
    acfg.commas = s.commas;
    acfg.pad = s.pad;
    acfg.bg_color_ass = s.bg_color_aabbggrr;

    generate_ass(ass_filename, frames, template_lines, total_notes, total_cc, ppqn, acfg);

    g_ass_path = ass_filename;
    g_midi_dir = midi_dir;
    g_midi_stem = midi_stem;
    g_total_duration = total_duration;
    g_fps = s.fps;
    g_vid_width = s.width;
    g_vid_height = s.height;
    g_ppqn = ppqn;
    g_total_notes = total_notes;
    g_start_delay = s.start_delay;
    g_preview_commas = s.commas;
    g_preview_pad = s.pad;
    g_bg_colour_ass = s.bg_color_aabbggrr;
    g_pos_mode = s.pos_mode;
    g_pos_x = s.pos_x;
    g_pos_y = s.pos_y;
    g_alignment = s.alignment;

    g_frames_data = std::move(frames);
    g_template_lines = template_lines;
    g_text_colour_ass = text_color_ass;
    g_ass_alignment = ass_alignment;
    g_font_size = s.font_size;
    g_font_family = s.font_family;
    g_preview_font_reload = true;   // preview font must match the (possibly new) family

    gui_log("Processing complete. Ready for preview/render.", false);
    g_processed = true;
    g_result_ret = 0;
    finish_op();
}

// ---------------------------------------------------------------------------
// Rendering wrapper (FFmpeg full video render)
// ---------------------------------------------------------------------------

// Run a command, letting the user cancel it mid-flight via g_cancel.
// Windows: the command runs in its own process tree, killed with taskkill /T.
// POSIX: the child gets its own process group, killed with SIGKILL.
static int run_command_cancellable(const std::string& cmd) {
#if defined(_WIN32)
    std::wstring wcmd = L"cmd.exe /c \"";
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
        std::wstring arg(static_cast<size_t>(wlen) - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, &arg[0], wlen);
        wcmd += arg;
        wcmd += L"\"";
    }
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
        return -1;
    int ret = -1;
    while (true) {
        DWORD w = WaitForSingleObject(pi.hProcess, 500);
        if (w == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            ret = (int)code;
            break;
        }
        if (g_cancel.load()) {
            std::string kill = "taskkill /PID " + std::to_string(pi.dwProcessId) + " /T /F >nul 2>&1";
            std::system(kill.c_str());
            WaitForSingleObject(pi.hProcess, 10000);
            ret = -2;   // cancelled
            break;
        }
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return ret;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    setpgid(pid, pid);
    int ret = -1;
    while (true) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1; break; }
        if (r < 0) break;
        if (g_cancel.load()) {
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            ret = -2;   // cancelled
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return ret;
#endif
}

struct RenderSettings {
    std::string output_video;
    std::string midi_dir;
    std::string midi_stem;
    int width, height;
    double fps, total_duration;
    std::string bg_color_aabbggrr = "&H00000000";
};

static void run_render(RenderSettings s) {
    g_busy = true;
    g_done = false;
    g_progress.store(0.0f);
    g_log_lines.clear();
    g_result_path.clear();

    gui_log(("Rendering video: " + s.output_video + "...").c_str(), false);

    // Move the ASS to a fixed bare name beside itself (see prepare_ass_for_filter
    // in fMCG_core.h): ffmpeg's filter parser mangles backslashes, apostrophes
    // and colons, so the subtitles filter must never receive a real user path.
    std::string safe_ass = prepare_ass_for_filter(g_ass_path);
    size_t ass_sep = safe_ass.find_last_of("/\\");
    std::string dir_cd = (ass_sep != std::string::npos) ? safe_ass.substr(0, ass_sep) : std::string(".");

    std::string bat_file = s.midi_dir + s.midi_stem + "_fMCG_ffmpeg.bat";
    std::string progress_file = s.midi_dir + s.midi_stem + "_fMCG_progress.txt";
    {
        std::ofstream bat(bat_file);
        std::string out_fwd = s.output_video;
        std::replace(out_fwd.begin(), out_fwd.end(), '\\', '/');
        bat << "@echo off\ncd /d \"" << dir_cd << "\" || exit /b 1\n";
        bat << "ffmpeg -y -f lavfi -i \"color=c=" << ffmpeg_color_spec(s.bg_color_aabbggrr)
            << ":s=" << s.width << "x" << s.height
            << ":r=" << s.fps << ":d=" << s.total_duration
            << "\" -vf \"subtitles=temp_stats.ass\""
            << " -c:v libx264 -pix_fmt yuv420p"
            << " -progress pipe:2"
            << " \"" << out_fwd << "\" 2>\"" << progress_file << "\"\n";
    }

    std::string cmd = "\"" + bat_file + "\"";
    std::atomic<bool> render_done{false};
    std::thread render_thread([cmd, bat_file, progress_file, s, &render_done]() {
        int ret = run_command_cancellable(cmd);
        if (g_cancel.load()) {
            gui_log("Rendering cancelled.", false);
            std::remove(bat_file.c_str());
            std::remove(progress_file.c_str());
            ret = -2;
        } else if (ret == 0) std::remove(bat_file.c_str());   // keep bat + log on failure for diagnosis
        g_result_ret = ret;
        g_result_path = s.output_video;
        if (ret == 0)
            gui_log(("Video rendered to: " + s.output_video).c_str(), false);
        else if (ret != -2)
            gui_log(("FFmpeg rendering failed (exit " + std::to_string(ret)
                     + "). Full log kept in: " + progress_file).c_str(), true);
        render_done = true;
    });

    while (!render_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::ifstream pf(progress_file);
        if (pf.is_open()) {
            std::string line;
            double last_out_time = 0.0;
            while (std::getline(pf, line)) {
                if (line.size() > 12 && line.substr(0, 12) == "out_time_ms=")
                    last_out_time = std::stod(line.substr(12)) / 1000000.0;
                else if (line == "progress=end")
                    last_out_time = s.total_duration;
            }
            pf.close();
            if (s.total_duration > 0) {
                float p = (float)(last_out_time / s.total_duration);
                if (p > 1.0f) p = 1.0f;
                g_progress.store(p);
            }
        }
    }
    render_thread.join();
    if (g_result_ret == 0) {
        std::remove(progress_file.c_str());
        std::remove(safe_ass.c_str());   // renamed copy; original was consumed by the rename
    }
    // On failure the .ass, the generated .bat and the ffmpeg log are kept so
    // the exact cause remains inspectable.
    g_progress.store(1.0f);
    finish_op();
}

// ---------------------------------------------------------------------------
// Preview popup (rendered inside main ImGui window as a modal)
// ---------------------------------------------------------------------------

static void render_preview_popup() {
    if (!g_show_preview) return;

    ImVec2 display = ImGui::GetIO().DisplaySize;

    // Compute popup size matching output aspect ratio
    double total_len = g_total_duration + g_start_delay;   // includes black lead-in
    float max_w = display.x * 0.8f;
    float max_h = display.y * 0.85f;
    float popup_w, popup_h;
    if (g_vid_width > 0 && g_vid_height > 0) {
        float aspect = (float)g_vid_width / (float)g_vid_height;
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

    if (ImGui::BeginPopupModal("Preview", &g_show_preview,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar)) {

        // Update preview time (timeline spans start_delay + song duration)
        if (g_render_active && g_busy.load() && total_len > 0) {
            g_preview_time = g_progress.load() * total_len;
            if (g_preview_time > total_len) g_preview_time = total_len;
        } else if (g_preview_playing && !g_frames_data.empty() && total_len > 0) {
            double elapsed = glfwGetTime() - g_preview_start_time;
            g_preview_time = g_preview_start_pos + elapsed;
            if (g_preview_time >= total_len) {
                g_preview_time = total_len;
                g_preview_playing = false;
            }
        }

        bool rendering = g_render_active && g_busy.load();
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
                              ass_colour_to_imcol32(g_bg_colour_ass));
        }

        // Render text
        if (!g_frames_data.empty() && total_len > 0) {
            // Start-delay lead-in: stats all at zero, current-time fields run
            // negative and count up to 0 — same text the video shows there.
            if (g_preview_time < g_start_delay) {
                ImGui::EndChild();
                ImGui::PushStyleColor(ImGuiCol_Text, ass_colour_to_imgui(g_text_colour_ass));
                if (g_preview_font) ImGui::PushFont(g_preview_font, std::max(4.0f, std::min(256.0f, (float)g_font_size * (g_vid_width > 0 ? ImGui::GetContentRegionAvail().x / (float)g_vid_width : 0.0f))));
                FrameStats zero_fs;
                zero_fs.bpm = g_frames_data.front().bpm;
                zero_fs.timestamp_sec = g_preview_time - g_start_delay;
                std::string text = format_frame_text(zero_fs);
                std::vector<std::string> lines;
                {
                    std::istringstream stream(text);
                    std::string ln;
                    while (std::getline(stream, ln)) lines.push_back(ln);
                }
                // Match the video's placement (same geometry as the song branch).
                float preview_scale2 = (g_vid_width > 0) ? ImGui::GetContentRegionAvail().x / (float)g_vid_width : 0.0f;
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
                int a2 = g_ass_alignment;
                bool left2 = (a2 == 7 || a2 == 1);
                bool top2  = (a2 == 7 || a2 == 9);
                float x2, y2;
                if (g_pos_mode == 1) {
                    x2 = (float)g_pos_x * preview_scale2;
                    y2 = (float)g_pos_y * preview_scale2;
                } else {
                    x2 = left2 ? margin2 : area_w2 - max_tw2 - margin2;
                    y2 = top2  ? margin2 : area_h2 - total_text_h2 - margin2;
                    if (y2 < margin2) y2 = margin2;
                    if (x2 < margin2) x2 = margin2;
                }
                ImGui::SetCursorPos(ImVec2(x2, y2));
                for (size_t i = 0; i < lines.size(); ++i) {
                    if (i > 0) ImGui::SetCursorPosX(x2);
                    ImGui::TextUnformatted(lines[i].c_str());
                }
                if (g_preview_font) ImGui::PopFont();
                ImGui::PopStyleColor();

                if (rendering) {
                    float p = display_progress();
                    char overlay[32];
                    snprintf(overlay, sizeof(overlay), "Rendering %d%%", (int)(p * 100));
                    ImGui::ProgressBar(p, ImVec2(-1, 0), overlay);
                } else {
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
                    float tp = (float)g_preview_time;
                    if (ImGui::SliderFloat("##time", &tp, 0.0f, (float)total_len, "%.2fs")) {
                        g_preview_time = tp;
                        if (g_preview_playing) {
                            g_preview_start_pos = tp;
                            g_preview_start_time = glfwGetTime();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(g_preview_playing ? "Pause" : "Play", ImVec2(50, 0))) {
                        if (g_preview_playing) g_preview_playing = false;
                        else {
                            g_preview_playing = true;
                            g_preview_start_time = glfwGetTime();
                            g_preview_start_pos = g_preview_time;
                        }
                    }
                }
                ImGui::EndPopup();
                return;
            }
            // Scale video-resolution values (ASS Fontsize / 30px margins, PlayRes
            // = video width x height) into the preview area, which shows the
            // full video frame scaled by preview_width / video_width.
            float preview_scale = (g_vid_width > 0) ? ImGui::GetContentRegionAvail().x / (float)g_vid_width : 0.0f;
            float desired_px = (float)g_font_size * preview_scale;
            if (desired_px < 4.0f) desired_px = 4.0f;
            if (desired_px > 256.0f) desired_px = 256.0f;
            if (!g_preview_font && !g_preview_font_reload.load() && !g_font_family.empty())
                g_preview_font_reload = true;   // picked up before the next frame

            const FrameStats& fs = find_frame(g_preview_time - g_start_delay);
            std::string text = format_frame_text(fs);

            std::vector<std::string> lines;
            {
                std::istringstream stream(text);
                std::string ln;
                while (std::getline(stream, ln)) lines.push_back(ln);
            }

            ImGui::PushStyleColor(ImGuiCol_Text, ass_colour_to_imgui(g_text_colour_ass));
            if (g_preview_font) ImGui::PushFont(g_preview_font, desired_px);
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

            int a = g_ass_alignment;
            bool left = (a == 7 || a == 1);
            bool top  = (a == 7 || a == 9);

            float x_off, y_off;
            if (g_pos_mode == 1) {
                // Explicit placement: (pos_x,pos_y) in video pixels anchors the
                // text block's TOP-LEFT (matches the \pos(...,7) in the ASS).
                x_off = (float)g_pos_x * preview_scale;
                y_off = (float)g_pos_y * preview_scale;
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
            if (g_preview_font) ImGui::PopFont();
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
            float p = display_progress();
            char overlay[32];
            snprintf(overlay, sizeof(overlay), "Rendering %d%%", (int)(p * 100));
            ImGui::ProgressBar(p, ImVec2(-1, 0), overlay);
        } else {
            if (g_render_active && !g_busy.load()) {
                g_render_active = false;
                g_preview_playing = false;
                g_show_preview = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
            float tp = (float)g_preview_time;
            if (ImGui::SliderFloat("##time", &tp, 0.0f, (float)total_len, "%.2fs")) {
                g_preview_time = tp;
                if (g_preview_playing) {
                    g_preview_start_pos = tp;
                    g_preview_start_time = glfwGetTime();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(g_preview_playing ? "Pause" : "Play", ImVec2(50, 0))) {
                if (g_preview_playing) {
                    g_preview_playing = false;
                } else {
                    g_preview_playing = true;
                    g_preview_start_time = glfwGetTime();
                    g_preview_start_pos = g_preview_time;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop", ImVec2(50, 0))) {
                g_preview_playing = false;
                g_preview_time = 0.0;
            }
        }

        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

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
            bool dialog_active = g_dialog_busy.load();
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
        ImGui::Text("Font Family");
        ImGui::SetNextItemWidth(-1);
        if (!font_list_loaded && !font_cstrs.empty()) font_list_loaded = true;
        if (!font_cstrs.empty()) {
            ImGui::Combo("##font", &selected_font_idx, font_cstrs.data(), (int)font_cstrs.size());
            settings.font_family = system_fonts[selected_font_idx];
        } else {
            char font_buf[256];
            strncpy(font_buf, settings.font_family.c_str(), sizeof(font_buf) - 1);
            font_buf[sizeof(font_buf) - 1] = '\0';
            ImGui::InputText("##font_custom", font_buf, sizeof(font_buf));
            settings.font_family = font_buf;
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
            bool cfg_dialog_active = g_dialog_busy.load();
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
            bool can_process = (midi_buf[0] != '\0') && !g_busy.load();
            bool can_preview = g_processed.load() && !g_busy.load();
            bool can_render  = g_processed.load() && !g_busy.load();

            if (!can_process) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
            if (ImGui::Button("Process", ImVec2(140, 30)) && can_process) {
                settings.midi_file = midi_buf;
                settings.output_video = output_buf;
                settings.layout_text = layout_buf;
                g_processed = false;
                g_done = false;
                g_log_lines.clear();
                g_preview_playing = false;
                g_preview_time = 0.0;
                g_busy = true;  // set before detach so the button disables immediately
                std::thread(run_process, settings).detach();
            }
            if (!can_process) ImGui::PopStyleVar();

            ImGui::SameLine();
            // Cancel while processing or rendering (checked at ~1M-event pings
            // in the scan; within 500ms in the ffmpeg wait loop).
            if (g_busy.load()) {
                ImGui::Button("Cancel", ImVec2(140, 30));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Abort the current operation");
                if (ImGui::IsItemClicked()) {
                    g_cancel = true;
                    if (g_render_active) gui_log("Cancelling ffmpeg...", false);
                }
            }

            ImGui::SameLine();
            if (!can_preview) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
            if (ImGui::Button("Preview", ImVec2(140, 30)) && can_preview) {
                g_show_preview = true;
                g_preview_playing = true;
                g_preview_start_time = glfwGetTime();
                g_preview_start_pos = g_preview_time;
                ImGui::OpenPopup("Preview");
            }
            if (!can_preview) ImGui::PopStyleVar();

            ImGui::SameLine();
            if (!can_render) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
            if (ImGui::Button("Render Video", ImVec2(140, 30)) && can_render) {
                g_show_preview = true;
                g_render_active = true;
                g_preview_playing = false;
                g_preview_time = 0.0;
                ImGui::OpenPopup("Preview");

                RenderSettings rs;
                rs.output_video = output_buf;
                rs.midi_dir = g_midi_dir;
                rs.midi_stem = g_midi_stem;
                rs.width = g_vid_width;
                rs.height = g_vid_height;
                rs.fps = g_fps;
                rs.total_duration = g_total_duration + g_start_delay;
                rs.bg_color_aabbggrr = g_bg_colour_ass;
                g_done = false;
                g_log_lines.clear();
                // Set busy on the GUI thread *before* detaching: the preview popup
                // closes itself when it sees render-active && !busy, and the detached
                // thread may not have run yet on the first frame (race closed the
                // popup instantly, so it never appeared).
                g_busy = true;
                std::thread(run_render, rs).detach();
            }
            if (!can_render) ImGui::PopStyleVar();

            ImGui::SameLine();
            if (g_processed.load()) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "Processed");
            } else if (g_busy.load()) {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "Working...");
            } else if (g_done.load() && g_result_ret.load() == 0 && !g_processed.load()) {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Done!");
            }
        }

        // --- Progress Bar ---
        if (g_busy.load() || g_progress.load() > 0.0f) {
            float p = display_progress();
            std::string overlay;
            {
                std::lock_guard<std::mutex> lock(g_scan_mutex);
                // Scan stats belong on the bar only while processing, never
                // during an ffmpeg render.
                if (g_scan_active && !g_render_active)
                    overlay = compose_scan_line();
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
            std::lock_guard<std::mutex> lock(g_log_mutex);
            // While a scan is live, the last log entry is a self-overwriting
            // status line (per the user's request) instead of a growing list.
            std::string scan_line;
            bool scan_live = false;
            {
                std::lock_guard<std::mutex> slock(g_scan_mutex);
                scan_live = g_scan_active && g_busy.load() && g_processed.load() == false;
                if (scan_live) scan_line = compose_scan_line();
            }
            size_t n = g_log_lines.size();
            if (scan_live && n > 0) n -= 1;   // the live line replaces the last one
            for (size_t i = 0; i < n; ++i)
                ImGui::TextWrapped("%s", g_log_lines[i].c_str());
            if (scan_live) ImGui::TextWrapped("%s", scan_line.c_str());
            else if (!g_log_lines.empty() && n != g_log_lines.size())
                ImGui::TextWrapped("%s", g_log_lines.back().c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        if (g_done.load() && !g_processed.load() && !g_busy.load()) {
            if (g_result_ret.load() == 0) {
                uint64_t fsize = 0;
                {
                    std::ifstream rf(g_result_path, std::ios::binary | std::ios::ate);
                    if (rf.is_open()) fsize = (uint64_t)rf.tellg();
                }
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Rendered: %s (%s)",
                    g_result_path.c_str(), format_file_size(fsize).c_str());
            } else if (g_result_ret.load() != -1)
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
