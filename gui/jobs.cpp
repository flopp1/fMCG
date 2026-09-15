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
// Processing worker: parse MIDI + generate the ASS subtitle file
// ---------------------------------------------------------------------------

static std::string get_ass_path(const std::string& midi_file) {
    // Generated in the OS temp area, not beside the user's MIDI: keeps the
    // visible folder clean and the file safe from accidental deletion.
    std::string stem = extract_stem(midi_file);
    return app_temp_dir() + "/" + stem + "_fMCG.ass";
}

void run_process(GuiSettings s) {
    AppState& app = g_app;
    app.busy = true;
    app.done = false;
    app.progress.store(0.0f);
    app.log_lines.clear();
    app.result_path.clear();
    app.processed = false;
    app.cancel = false;   // clearing here too: a previous cancelled op must not abort this scan

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
    uint64_t total_ticks = 0;

    ProgressCallbacks cb;
    cb.on_progress = [](const char* pass, int percent) { g_app.gui_progress(pass, percent); };
    cb.on_log = [](const char* msg, bool err) { g_app.gui_log(msg, err); };
    cb.on_scan_progress = [](uint64_t ev, double el, double evps, double frac) {
        g_app.gui_scan_progress(ev, el, evps, frac);
    };
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
    }

    app.gui_log(("Processing MIDI: " + s.midi_file + "...").c_str(), false);

    auto frames = ScaleMidiProcessor::process_midi(s.midi_file, s.fps, ppqn, total_notes,
                                                    s.vel0_note_off, total_ticks, cb, s.end_delay,
                                                    s.parse_threads);
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
    acfg.bpm = s.bpm;
    acfg.bg_color_ass = s.bg_color_aabbggrr;
    acfg.start_delay = s.start_delay;
    acfg.end_delay = s.end_delay;
    acfg.total_ticks = total_ticks;

    generate_ass(ass_filename, frames, template_lines, total_notes, total_cc, ppqn, acfg, total_ticks);

    app.ass_path = ass_filename;
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

// Spawn ffmpeg directly (no shell, no batch file) with its working directory
// set to `cwd`, and run it to completion. The child's stdout carries ffmpeg's
// machine-readable `-progress pipe:1` feed; every complete line is passed to
// `on_line` (called on the calling thread). The child's stderr is appended to
// `stderr_file` when non-empty (debug builds keep a log there for diagnosis);
// otherwise it is discarded. Cancellation via g_app.cancel kills the whole
// process tree. Returns the exit code, or -2 when cancelled, -1 on spawn
// failure.
int spawn_ffmpeg_cancellable(const std::string& cwd,
                                    const std::vector<std::string>& args,
                                    const std::string& stderr_file,
                                    const std::function<void(const std::string&)>& on_line) {
#if defined(_WIN32)
    // Build the quoted command line for the child (CreateProcess takes one
    // string; the child's CRT re-parses it with the standard rules).
    std::wstring wcmd = L"ffmpeg ";
    {
        std::string rest;
        for (size_t i = 0; i < args.size(); ++i) {
            rest += "\"" + args[i] + "\"";
            if (i + 1 < args.size()) rest += " ";
        }
        int wlen = MultiByteToWideChar(CP_UTF8, 0, rest.c_str(), -1, nullptr, 0);
        std::wstring warg(static_cast<size_t>(wlen) - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, rest.c_str(), -1, &warg[0], wlen);
        wcmd += warg;
    }
    std::wstring wcwd = L".";
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, cwd.c_str(), -1, nullptr, 0);
        wcwd.resize(static_cast<size_t>(wlen) - 1);
        MultiByteToWideChar(CP_UTF8, 0, cwd.c_str(), -1, &wcwd[0], wlen);
        // lpCurrentDirectory must be fully qualified; resolve relative paths.
        wchar_t abs[MAX_PATH];
        if (GetFullPathNameW(wcwd.c_str(), MAX_PATH, abs, nullptr) > 0) wcwd = abs;
    }

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };   // inheritable
    HANDLE out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) return -1;
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);   // parent side only

    HANDLE err_w = nullptr;
    if (!stderr_file.empty()) {
        std::wstring werr;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, stderr_file.c_str(), -1, nullptr, 0);
        werr.resize(static_cast<size_t>(wlen) - 1);
        MultiByteToWideChar(CP_UTF8, 0, stderr_file.c_str(), -1, &werr[0], wlen);
        err_w = CreateFileW(werr.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (!err_w) {
        err_w = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_w;
    si.hStdError  = err_w;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, &wcmd[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
                             wcwd.empty() ? nullptr : &wcwd[0], &si, &pi);
    CloseHandle(out_w); CloseHandle(err_w);
    if (!ok) { CloseHandle(out_r); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return -1; }

    int ret = -1;
    std::string pend;                       // partial line from the pipe
    char buf[4096];
    bool pipe_open = true;
    while (true) {
        // Drain whatever the child has produced so far.
        while (pipe_open) {
            DWORD got = 0;
            if (!PeekNamedPipe(out_r, nullptr, 0, nullptr, &got, nullptr) || got == 0) break;
            DWORD n = (DWORD)std::min<DWORD>(got, sizeof(buf));
            if (!ReadFile(out_r, buf, n, &got, nullptr) || got == 0) { pipe_open = false; break; }
            pend.append(buf, buf + got);
            size_t pos;
            while ((pos = pend.find('\n')) != std::string::npos) {
                std::string line = pend.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                pend.erase(0, pos + 1);
                if (on_line) on_line(line);
            }
        }
        DWORD w = WaitForSingleObject(pi.hProcess, 200);
        if (w == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            ret = (int)code;
            break;
        }
        if (g_app.cancel.load()) {
            // Kill ffmpeg directly -- no shell. (taskkill via std::system
            // would flash a cmd.exe console: the app is GUI-subsystem, so
            // Windows allocates one for the spawned shell.) ffmpeg has no
            // children of its own, so terminating the process is enough.
            TerminateProcess(pi.hProcess, (UINT)-3);
            WaitForSingleObject(pi.hProcess, 10000);
            ret = -2;   // cancelled
            break;
        }
    }
    // Flush whatever remains after exit.
    DWORD got = 0;
    while (pipe_open && PeekNamedPipe(out_r, nullptr, 0, nullptr, &got, nullptr) && got > 0) {
        if (!ReadFile(out_r, buf, (DWORD)std::min<DWORD>(got, sizeof(buf)), &got, nullptr) || got == 0) break;
        pend.append(buf, buf + got);
        size_t pos;
        while ((pos = pend.find('\n')) != std::string::npos) {
            std::string line = pend.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pend.erase(0, pos + 1);
            if (on_line) on_line(line);
        }
    }
    if (!pend.empty() && on_line) on_line(pend);
    CloseHandle(out_r);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return ret;
#else
    int pfd[2];
    if (pipe(pfd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        // Child: wire stdout to the progress pipe, stderr to the log (or
        // devnull), run in the ASS directory, then become ffmpeg.
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]); close(pfd[1]);
        int err_fd = open(stderr_file.empty() ? "/dev/null" : stderr_file.c_str(),
                          O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (err_fd >= 0) { dup2(err_fd, STDERR_FILENO); close(err_fd); }
        if (chdir(cwd.c_str()) != 0) _exit(126);
        setpgid(0, 0);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("ffmpeg"));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp("ffmpeg", argv.data());
        _exit(127);
    }
    close(pfd[1]);
    setpgid(pid, pid);

    int ret = -1;
    std::string pend;
    char buf[4096];
    bool pipe_open = true;
    while (true) {
        ssize_t n = pipe_open ? read(pfd[0], buf, sizeof(buf)) : (ssize_t)-1;
        if (n > 0) {
            pend.append(buf, buf + n);
            size_t pos;
            while ((pos = pend.find('\n')) != std::string::npos) {
                std::string line = pend.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                pend.erase(0, pos + 1);
                if (on_line) on_line(line);
            }
            continue;   // keep draining before checking the child
        }
        if (n == 0) pipe_open = false;   // EOF: child closed stdout
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
        if (!pipe_open) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!pend.empty() && on_line) on_line(pend);
    close(pfd[0]);
    return ret;
#endif
}

void run_render(RenderSettings s) {
    AppState& app = g_app;
    app.busy = true;
    app.done = false;
    app.cancel = false;   // a cancelled previous render must not kill this one
    app.progress.store(0.0f);
    app.log_lines.clear();
    app.result_path.clear();

    app.render_active = true;

    app.gui_log(("Rendering video: " + s.output_video + "...").c_str(), false);

    // Move the ASS to a fixed bare name beside itself (see fmcg_render.h):
    // ffmpeg's filter parser mangles backslashes, apostrophes and colons, so
    // the subtitles filter must never receive a real user path.
    std::string safe_ass = prepare_ass_for_filter(app.ass_path);
    if (safe_ass.empty()) {
        app.gui_log("Error: cannot prepare the subtitle file for rendering (missing or unreadable).\n", true);
        app.result_ret = -1;
        app.op_result = AppState::OP_RENDER_FAIL;
        app.finish_op();
        return;
    }
    size_t ass_sep = safe_ass.find_last_of("/\\");
    std::string dir_cd = (ass_sep != std::string::npos) ? safe_ass.substr(0, ass_sep) : std::string(".");

    // ffmpeg runs with its working directory set to the ASS directory so the
    // subtitles filter receives a bare, un-manglable name. Progress arrives
    // on the child's stdout (-progress pipe:1). ffmpeg's own stderr log is
    // only kept on disk in debug builds; end users never see it.
    std::vector<std::string> args = ffmpeg_args(s.output_video, s.width, s.height,
                                                s.fps, s.total_duration,
                                                s.bg_color_aabbggrr,
                                                /*progress_to_stdout=*/true,
                                                s.ffmpeg_threads);
#if defined(FMCG_DEBUG)
    std::string progress_file = s.midi_dir + s.midi_stem + "_fMCG_progress.txt";
#else
    std::string progress_file;   // discarded (NUL/devnull) in release builds
#endif

    std::atomic<bool> render_done{false};
    std::thread render_thread([&args, &dir_cd, &progress_file, &safe_ass, s, &render_done]() {
        auto on_line = [&s](const std::string& line) {
            if (line.size() > 12 && line.substr(0, 12) == "out_time_ms=") {
                try {
                    double t = std::stod(line.substr(12)) / 1000000.0;
                    if (s.total_duration > 0) {
                        float p = (float)(t / s.total_duration);
                        if (p > 1.0f) p = 1.0f;
                        if (p < 0.0f) p = 0.0f;
                        g_app.progress.store(p);
                    }
                } catch (...) {}
            } else if (line.rfind("speed=", 0) == 0) {
                try { g_app.render_speed.store((float)std::stod(line.substr(6))); } catch (...) {}
            } else if (line == "progress=end" && s.total_duration > 0) {
                g_app.progress.store(1.0f);
            }
        };
        const auto t_ff_start = std::chrono::steady_clock::now();
        int ret = spawn_ffmpeg_cancellable(dir_cd, args, progress_file, on_line);
        g_app.render_wallclock.store(std::chrono::duration<double>(std::chrono::steady_clock::now() - t_ff_start).count());
        if (g_app.cancel.load()) {
            g_app.gui_log("Rendering cancelled.", false);
            if (!progress_file.empty()) std::remove(progress_file.c_str());
            ret = -2;
        } else if (ret == 0 && !progress_file.empty()) {
            std::remove(progress_file.c_str());   // clean run: no log left behind
        }
        g_app.result_ret = ret;
        g_app.op_result = (ret == 0)  ? AppState::OP_RENDER_OK
                        : (ret == -2) ? AppState::OP_RENDER_CANCELLED
                                      : AppState::OP_RENDER_FAIL;
        g_app.result_path = s.output_video;
        if (ret == 0)
            g_app.gui_log(("Video rendered to: " + s.output_video).c_str(), false);
        else if (ret != -2)
            g_app.gui_log(("FFmpeg rendering failed (exit " + std::to_string(ret)
                     + ")." + (progress_file.empty()
                                ? std::string(" Rebuild with FMCG_DEBUG=1 for a detailed log.")
                                : " Full log kept in: " + progress_file)).c_str(), true);
        render_done = true;
    });

    render_thread.join();
    // temp_stats.ass is a working copy; sweep it either way. The original
    // <name>_fMCG.ass is intentionally kept so the render can be repeated.
    std::remove(safe_ass.c_str());
    app.progress.store(1.0f);
    app.finish_op();
}
