// colour_edit.cpp -- see colour_edit.h.
//
// Design note: ImGui's built-in ColorEdit3 already gives RGB sliders + live
// preview in a compact popup, but its default picker includes alpha/hex modes
// that let users produce non-RRGGBB values. We wrap ColorEdit3 with
// ImGuiColorEditFlags_NoAlpha | NoInputs? -- No, inputs are useful. We keep
// the hex text input (HSV-free, matching the rest of the UI) and drive it
// from the same float[3] the sliders edit, so both stay in sync.
#include "colour_edit.h"

#include <cstdio>
#include <cstring>

namespace fmcg_ui {

bool ass_to_rrggbb(const std::string& ass, std::string& out) {
    // &HAABBGGRR -> RRGGBB
    if (ass.size() >= 8 && ass[0] == '&' && (ass[1] == 'H' || ass[1] == 'h')) {
        std::string hex = ass.substr(2);
        if (hex.size() == 8) hex = hex.substr(2);   // drop alpha
        if (hex.size() == 6) {
            out = hex.substr(4, 2) + hex.substr(2, 2) + hex.substr(0, 2);
            return true;
        }
    }
    return false;
}

std::string rrggbb_to_ass(const std::string& rrggbb) {
    if (rrggbb.size() != 6) return "&H00FFFFFF";
    // RRGGBB -> &H00BBGGRR
    return "&H00" + rrggbb.substr(4, 2) + rrggbb.substr(2, 2) + rrggbb.substr(0, 2);
}

bool EditColour(const char* label, std::string& rrggbb) {
    // Normalise current value into float components.
    if (rrggbb.size() != 6) rrggbb = "FFFFFF";
    float col[3] = {
        (float)std::stoul(rrggbb.substr(0, 2), nullptr, 16) / 255.0f,
        (float)std::stoul(rrggbb.substr(2, 2), nullptr, 16) / 255.0f,
        (float)std::stoul(rrggbb.substr(4, 2), nullptr, 16) / 255.0f,
    };

    ImGui::PushID(label);
    // The swatch itself opens the compact picker popup.
    bool changed = ImGui::ColorEdit3(label, col,
        ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoDragDrop |
        ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_DisplayHex);
    ImGui::PopID();

    if (changed) {
        char hex[8];
        snprintf(hex, sizeof(hex), "%02X%02X%02X",
                 (int)(col[0] * 255.0f + 0.5f),
                 (int)(col[1] * 255.0f + 0.5f),
                 (int)(col[2] * 255.0f + 0.5f));
        rrggbb = hex;
    }
    return changed;
}

} // namespace fmcg_ui
