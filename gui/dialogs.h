// dialogs.h -- non-blocking native file-open dialogs.
//
// Runs the platform dialog in a detached worker thread (the commands shell out
// to PowerShell on Windows, osascript on macOS, zenity/kdialog elsewhere) and
// reports the picked path back through AppState.
#pragma once
#include <string>

void launch_dialog(const std::string& cmd, const std::string& kind);
std::string poll_dialog_result(const std::string& kind);
