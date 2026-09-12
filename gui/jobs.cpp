#include "jobs.h"
#include "app_state.h"
#include "dialogs.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <atomic>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Processing worker: parse MIDI + generate the ASS subtitle file
// ---------------------------------------------------------------------------

static std::string get_ass_path(const std::string& midi_file) {
    std::string dir = extract_dir(midi_file);
    std::string stem = extract_stem(midi_file);
    return dir + stem + "_fMCG.ass";
}

void run_process(GuiSettings s) {
    AppState& app = g_app;
    app.busy = true;
    app.done = false;
    app.progress.store(0.0f);
    app.log_lines.clear();
    app.result_path.clear();
    app.processed = false;

    int ass_alignment = gui_alignment_to_ass(s.alignment);

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
    cb.on_progress = [](const char* pass, int percent) { g_app.gui_progress(pass, percent); };
    cb.on_log = [](const char* msg, bool err) { g_app.gui_log(msg, err); };
    cb.on_scan_progress = [](uint64_t ev, double el, double evps, double frac) {
        g_app.gui_scan_progress(ev, el, evps, frac);
    };
    cb.cc_stats = s.cc_stats;
    cb.cancel_flag = &app.cancel;
    app.cancel = false;

    {
        std::lock_guard<std::mutex> lock(app.scan_mutex);
        app.scan_events = 0; app.scan_elapsed = 0.0; app.scan_evps = 0.0;
        app.scan_frac = -1.0; app.scan_active = false;
    }

    app.gui_log(("Processing MIDI: " + s.midi_file + "...").c_str(), false);

    auto frames = ScaleMidiProcessor::process_midi(s.midi_file, s.fps, ppqn, total_notes,
                                                    s.vel0_note_off, cb);
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
    std::string ass_filename = get_ass_path(s.midi_file);

    AssConfig acfg;
    acfg.width = s.width;
    acfg.height = s.height;
    acfg.fps = s.fps;
    acfg.font_size = s.font_size;
    acfg.font_family = s.font_family;
    acfg.bold = s.font_bold;
    acfg.italic_flag = s.font_italic;
    acfg.text_color_ass = text_color_ass;
    acfg.ass_alignment = ass_alignment;
    acfg.pos_mode = s.pos_mode;
    acfg.pos_x = s.pos_x;
    acfg.pos_y = s.pos_y;
    acfg.commas = s.commas;
    acfg.pad = s.pad;
    acfg.bg_color_ass = s.bg_color_aabbggrr;
    acfg.start_delay = s.start_delay;

    generate_ass(ass_filename, frames, template_lines, total_notes, total_cc, ppqn, acfg);

    app.ass_path = ass_filename;
    app.midi_dir = midi_dir;
    app.midi_stem = midi_stem;
    app.total_duration = total_duration;
    app.fps = s.fps;
    app.vid_width = s.width;
    app.vid_height = s.height;
    app.ppqn = ppqn;
    app.total_notes = total_notes;
    app.start_delay = s.start_delay;
    app.preview_commas = s.commas;
    app.preview_pad = s.pad;
    app.bg_colour_ass = s.bg_color_aabbggrr;
    app.pos_mode = s.pos_mode;
    app.pos_x = s.pos_x;
    app.pos_y = s.pos_y;
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
    app.preview_font_reload = true;   // preview font must match the (possibly new) family

    app.gui_log("Processing complete. Ready for preview/render.", false);
    app.processed = true;
    app.result_ret = 0;
    app.op_result = AppState::OP_PROCESS_OK;
    app.finish_op();
}

// ---------------------------------------------------------------------------
// Rendering worker: FFmpeg full video render
// ---------------------------------------------------------------------------

// Run a command, letting the user cancel it mid-flight via g_app.cancel.
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
        if (g_app.cancel.load()) {
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
        if (g_app.cancel.load()) {
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

void run_render(RenderSettings s) {
    AppState& app = g_app;
    app.busy = true;
    app.done = false;
    app.progress.store(0.0f);
    app.log_lines.clear();
    app.result_path.clear();

    app.render_active = true;

    app.gui_log(("Rendering video: " + s.output_video + "...").c_str(), false);

    // Move the ASS to a fixed bare name beside itself (see fmcg_render.h):
    // ffmpeg's filter parser mangles backslashes, apostrophes and colons, so
    // the subtitles filter must never receive a real user path.
    std::string safe_ass = prepare_ass_for_filter(app.ass_path);
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
        if (g_app.cancel.load()) {
            g_app.gui_log("Rendering cancelled.", false);
            std::remove(bat_file.c_str());
            std::remove(progress_file.c_str());
            ret = -2;
        } else if (ret == 0) std::remove(bat_file.c_str());   // keep bat + log on failure for diagnosis
        g_app.result_ret = ret;
        g_app.op_result = (ret == 0)  ? AppState::OP_RENDER_OK
                        : (ret == -2) ? AppState::OP_RENDER_CANCELLED
                                      : AppState::OP_RENDER_FAIL;
        g_app.result_path = s.output_video;
        if (ret == 0)
            g_app.gui_log(("Video rendered to: " + s.output_video).c_str(), false);
        else if (ret != -2)
            g_app.gui_log(("FFmpeg rendering failed (exit " + std::to_string(ret)
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
                app.progress.store(p);
            }
        }
    }
    render_thread.join();
    if (app.result_ret == 0) {
        std::remove(progress_file.c_str());
        std::remove(safe_ass.c_str());   // renamed copy; original was consumed by the rename
    }
    // On failure the .ass, the generated .bat and the ffmpeg log are kept so
    // the exact cause remains inspectable.
    app.progress.store(1.0f);
    app.finish_op();
}
