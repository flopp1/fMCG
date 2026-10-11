// preview.h -- in-app preview popup: plays the processed stats overlay live,
// or the frames being encoded during a render.
#pragma once

namespace fmcg_preview {
void render_preview_popup();          // call inside the main window's Begin/End
}
using fmcg_preview::render_preview_popup;