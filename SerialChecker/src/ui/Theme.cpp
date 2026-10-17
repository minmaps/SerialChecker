#include "ui/Theme.h"

#include <windows.h>
#include <string>

namespace ui {

Fonts g_fonts;

namespace {

ImFont* LoadSystemFont(const wchar_t* file, ImFont* fallback)
{
    wchar_t dir[MAX_PATH];
    GetWindowsDirectoryW(dir, MAX_PATH);
    std::wstring path = std::wstring(dir) + L"\\Fonts\\" + file;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return fallback;

    char utf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
    ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(utf8, 15.0f);
    return f ? f : fallback;
}

} // namespace

void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    // Fonts are rasterized on demand at any size (ImGui 1.92 dynamic fonts), so one entry per face is enough.
    g_fonts.regular = LoadSystemFont(L"segoeui.ttf", nullptr);
    if (!g_fonts.regular) g_fonts.regular = io.Fonts->AddFontDefault();
    g_fonts.semibold = LoadSystemFont(L"seguisb.ttf", g_fonts.regular);
    g_fonts.italic = LoadSystemFont(L"segoeuii.ttf", g_fonts.regular);
    g_fonts.mono = LoadSystemFont(L"CascadiaMono.ttf", LoadSystemFont(L"consola.ttf", g_fonts.regular));
    io.FontDefault = g_fonts.regular;
}

void ApplyTheme(float dpiScale)
{
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);

    style.WindowPadding = ImVec2(0, 0);
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.FrameRounding = 6;
    style.ScrollbarSize = 10;
    style.ScrollbarRounding = 8;
    style.ItemSpacing = ImVec2(0, 0);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(col::Text);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.20f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.32f);
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);

    style.ScaleAllSizes(dpiScale);
    style.FontSizeBase = 15.0f;
    style.FontScaleDpi = dpiScale;
}

} // namespace ui
