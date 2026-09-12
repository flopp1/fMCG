// End-to-end test of the direct-ffmpeg spawn path (no batch file):
// renders a 1-second video through spawn_ffmpeg_cancellable + ffmpeg_args,
// checks that progress lines arrive, that the output exists, and that no
// .bat/_progress.txt artifacts are left behind. Requires ffmpeg on PATH.
#include "../gui/jobs.h"
#include "../src/fmcg_render.h"
#include <cstdio>
#include <fstream>
#include <sys/stat.h>
#include <algorithm>
#include <thread>
#include <chrono>
#if !defined(_WIN32)
#include <unistd.h>
#endif

static int fails = 0;
static void check(const char* name, bool ok) {
    printf("%-46s %s\n", name, ok ? "[OK]" : "[FAIL]");
    if (!ok) fails++;
}

static bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

int main() {
    // The worker touches g_app; jobs.cpp defines it via app_state.cpp, which
    // this test links too, so no extra setup is required.
    g_app.cancel = false;
    const std::string dir = "./_fftest/";
    system("mkdir -p _fftest 2>NUL");   // works via sh on POSIX; harmless no-op fallback on cmd
#if defined(_WIN32)
    system("if not exist _fftest mkdir _fftest");
#endif

    // Fake ASS (ffmpeg still needs a real one for the filter to init).
    {
        std::ofstream ass(dir + "temp_stats.ass");
        ass << "[Script Info]\nScriptType: v4.00+\nPlayResX: 320\nPlayResY: 180\n\n"
            << "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
               "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, "
               "Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, "
               "MarginV, Encoding\n"
            << "Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,"
               "100,100,0,0,1,2,0,2,10,10,10,1\n\n"
            << "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
               "Effect, Text\n"
            << "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,hello\n";
    }

    // Like real usage: output path is absolute (the child's CWD is the ASS
    // directory, so relative outputs would land inside it).
    char cwd_abs[1024];
    std::string abs_out;
    if (getcwd(cwd_abs, sizeof(cwd_abs)))
        abs_out = std::string(cwd_abs) + "/_fftest/out.mp4";
    else
        abs_out = "_fftest/out.mp4";
#if defined(_WIN32)
    std::replace(abs_out.begin(), abs_out.end(), '/', '\\');
#endif
    auto args = ffmpeg_args(abs_out, 320, 180, 30.0, 1.0, "&H00000000");
    int progress_lines = 0, out_time_lines = 0;
    int ret = spawn_ffmpeg_cancellable(dir, args, "",
        [&](const std::string& line) {
            if (line.rfind("out_time_ms=", 0) == 0) { ++out_time_lines; ++progress_lines; }
            else if (line.rfind("progress=", 0) == 0) ++progress_lines;
        });
    check("render exit code == 0", ret == 0);
    check("progress lines received", progress_lines > 0 && out_time_lines > 0);
    check("output video exists", exists(dir + "out.mp4"));

    check("no .bat artifact left", !exists("_fftest_fMCG_ffmpeg.bat") && !exists(dir + "out_fMCG_ffmpeg.bat"));
    check("no progress.txt artifact left", !exists("_fftest_fMCG_progress.txt") && !exists(dir + "out_fMCG_progress.txt"));

    // Cancellation: start a long render and cancel it quickly.
    g_app.cancel = false;
    auto long_args = ffmpeg_args(dir + "long.mp4", 320, 180, 30.0, 600.0, "&H00000000");
    std::thread canceler([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        g_app.cancel = true;
    });
    int ret2 = spawn_ffmpeg_cancellable(dir, long_args, "",
        [](const std::string&) {});
    canceler.join();
    check("cancelled render returns -2", ret2 == -2);
    check("cancelled render left no output", !exists(dir + "long.mp4"));

    system("rm -rf _fftest 2>/dev/null; rmdir /s /q _fftest 2>NUL");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
