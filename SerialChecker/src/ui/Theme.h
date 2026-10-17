#pragma once
#include "imgui.h"

namespace ui {

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* semibold = nullptr;
    ImFont* italic = nullptr;
    ImFont* mono = nullptr;  // serial values
};
extern Fonts g_fonts;

// Monochrome palette: near-black surfaces, white text, greys for hierarchy. No hue anywhere.
namespace col {
constexpr ImU32 Bg = IM_COL32(9, 9, 9, 255);
constexpr ImU32 Surface = IM_COL32(15, 15, 15, 255);
constexpr ImU32 SurfaceHover = IM_COL32(22, 22, 22, 255);
constexpr ImU32 Border = IM_COL32(30, 30, 30, 255);
constexpr ImU32 BorderStrong = IM_COL32(46, 46, 46, 255);
constexpr ImU32 Hairline = IM_COL32(24, 24, 24, 255);
constexpr ImU32 Text = IM_COL32(237, 237, 237, 255);
constexpr ImU32 TextDim = IM_COL32(160, 160, 160, 255);
constexpr ImU32 Label = IM_COL32(128, 128, 128, 255);
constexpr ImU32 Muted = IM_COL32(82, 82, 82, 255);
constexpr ImU32 White = IM_COL32(250, 250, 250, 255);
constexpr ImU32 Black = IM_COL32(10, 10, 10, 255);
// The only hue in the app: differences found by "Check".
constexpr ImU32 Green = IM_COL32(74, 222, 128, 255);
constexpr ImU32 GreenSoft = IM_COL32(74, 222, 128, 18);
constexpr ImU32 GreenLine = IM_COL32(74, 222, 128, 70);
} // namespace col

void LoadFonts();
void ApplyTheme(float dpiScale);

} // namespace ui
