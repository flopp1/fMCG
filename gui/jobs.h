// jobs.h -- worker-thread wrappers for the two long-running operations.
#pragma once
#include "app_state.h"

void run_process(GuiSettings s);       // parse MIDI into per-frame stats (worker thread)
void run_render(RenderSettings s);     // in-built render + encode to MP4/MOV (worker thread)
