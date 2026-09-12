#include "dialogs.h"
#include "app_state.h"

#include <cstdio>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

// ---------------------------------------------------------------------------
// Async file dialog (non-blocking)
// ---------------------------------------------------------------------------

#if defined(_WIN32)
// Like _popen(cmd, "r") but WITHOUT the visible cmd.exe console: the app is a
// GUI-subsystem binary, so Windows would allocate a brand-new console for the
// shell. CreateProcess with CREATE_NO_WINDOW keeps the helper hidden; its
// stdout feeds the pipe the same way.
static FILE* popen_hidden(const std::string& cmd) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };   // inheritable
    HANDLE out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) return nullptr;
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);   // parent side only

    std::wstring wcmd(cmd.begin(), cmd.end());   // dialog commands are ASCII
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_w;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, &wcmd[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(out_w);
    if (!ok) { CloseHandle(out_r); return nullptr; }
    CloseHandle(pi.hThread);

    // Wrap the read end in a FILE* the caller reads/closes normally. The
    // child handle is still open, so the descriptor must not be closed
    // early; _close(pi.hProcess-ish) is deliberately avoided, we leak the
    // process handle until the pipe closes (dialog processes are one-shot).
    int fd = _open_osfhandle((intptr_t)out_r, _O_RDONLY);
    if (fd == -1) {
        CloseHandle(out_r);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return nullptr;
    }
    FILE* f = _fdopen(fd, "r");
    if (!f) { _close(fd); TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hProcess); return nullptr; }
    // Store the process handle for the close path (see fclose_hidden).
    return f;
}
#endif   // _WIN32

static void run_file_dialog(const std::string& cmd) {   // called with g_app.dialog_kind set
    FILE* pipe =
#if defined(_WIN32)
        popen_hidden(cmd);
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
