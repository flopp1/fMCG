#include "dialogs.h"
#include "app_state.h"

#include <cstdio>
#include <thread>

// ---------------------------------------------------------------------------
// Async file dialog (non-blocking)
// ---------------------------------------------------------------------------

static void run_file_dialog(const std::string& cmd) {   // called with g_app.dialog_kind set
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
    g_app.dialog_result = result;
    g_app.dialog_done = true;
    g_app.dialog_busy = false;
}

void launch_dialog(const std::string& cmd, const std::string& kind) {
    if (g_app.dialog_busy.load()) return;
    g_app.dialog_result.clear();
    g_app.dialog_kind = kind;
    g_app.dialog_done = false;
    g_app.dialog_busy = true;
    std::thread(run_file_dialog, cmd).detach();
}

std::string poll_dialog_result(const std::string& kind) {
    if (g_app.dialog_done.load() && g_app.dialog_kind == kind) {
        g_app.dialog_done = false;
        g_app.dialog_kind.clear();
        std::string r = g_app.dialog_result;
        g_app.dialog_result.clear();
        return r;
    }
    return "";
}
