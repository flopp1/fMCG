// colour_edit.h -- compact RGB colour-picker widgets.
//
// One swatch button per colour; clicking opens a small always-on-top popup
// with R/G/B sliders (0..255), a hex field, and a live swatch. Vertical cost
// in the settings panel is a single 24px row per colour.
#pragma once
#include <imgui.h>
#include <string>

namespace fmcg_ui {

// 'col' is an in/out RRGGBB hex string ("FFFFFF", no '#'). Returns true when
// the value changed this frame (so callers can persist immediately).
bool EditColour(const char* label, std::string& rrggbb);

// ASS (&HAABBGGRR) <-> RRGGBB conversions shared by the widget and callers.
bool   ass_to_rrggbb(const std::string& ass, std::string& out_rrggbb);
std::string rrggbb_to_ass(const std::string& rrggbb);

} // namespace fmcg_ui
