// jobs.h -- worker-thread wrappers for the two long-running operations.
#pragma once
#include "app_state.h"

void run_process(GuiSettings s);       // parse MIDI + generate ASS (worker thread)
void run_render(RenderSettings s);     // ffmpeg render (worker thread)
