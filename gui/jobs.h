// jobs.h -- worker-thread wrappers for the two long-running operations.
#pragma once
#include "app_state.h"

#include <functional>
#include <string>
#include <vector>

void run_process(GuiSettings s);       // parse MIDI + generate ASS (worker thread)
void run_render(RenderSettings s);     // ffmpeg render (worker thread)

// Spawn ffmpeg directly (no shell, no batch file) with the given working
// directory and arguments; stdout lines (the -progress feed) are delivered to
// on_line as they arrive. stderr_file optionally captures ffmpeg's stderr
// (debug builds); empty means discard. Returns the exit code, -2 on cancel.
// Exposed for the test suite.
int spawn_ffmpeg_cancellable(const std::string& cwd,
                             const std::vector<std::string>& args,
                             const std::string& stderr_file,
                             const std::function<void(const std::string&)>& on_line);
