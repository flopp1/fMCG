// preview.h -- in-app preview popup: plays the processed stats overlay live,
// or the frames being encoded during a render.
#pragma once
#include "imgui.h"

struct PatternData;

namespace fmcg_preview {
void render_preview_popup();          // call inside the main window's Begin/End
// The frame the pattern `p` draws (at the video resolution: the processed
// song's last frame, or zero stats without one) as a GL texture for the box
// editor; redrawn only when the pattern, resolution or song change. 0 if the
// font cannot be loaded.
ImTextureID pattern_thumbnail(const PatternData& p);
}
using fmcg_preview::render_preview_popup;