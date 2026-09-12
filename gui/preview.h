// preview.h -- in-app preview popup: plays the processed stats overlay live,
// or shows ffmpeg's progress during a render.
#pragma once

namespace fmcg_preview {
void process_preview_font_reload();   // call once per frame, BEFORE ImGui::NewFrame
void render_preview_popup();          // call inside the main window's Begin/End
}
using fmcg_preview::process_preview_font_reload;
using fmcg_preview::render_preview_popup;
