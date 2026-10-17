#pragma once
#include "hw/Types.h"
#include "io/Exporter.h"

#include <future>
#include <optional>
#include <string>
#include <vector>

struct ImDrawList;
struct ImFont;
struct ImVec2;

namespace ui {

class App {
public:
    explicit App(void* hwnd);

    // Draws the whole UI. Call between ImGui::NewFrame() and ImGui::Render().
    void Frame(float dpi);
    // Work that must run outside the ImGui frame (modal Open / Save dialogs).
    void AfterFrame();

    // Re-reads the hardware and compares it with an export. Empty path = newest serials_*.json on the Desktop.
    void RequestCheck(std::wstring baselinePath = {});

private:
    void DrawHeader(ImDrawList* dl, const ImVec2& origin, float width);
    void DrawCompareBar(ImDrawList* dl, const ImVec2& origin, float width);
    void DrawSidebar(const ImVec2& origin, const ImVec2& size);
    void DrawContent(const ImVec2& origin, const ImVec2& size);
    void DrawLoading(ImDrawList* dl, const ImVec2& center);
    void DrawToast(ImDrawList* dl, const ImVec2& viewportMax);

    float MeasureCard(const hw::Card& card, float width) const;
    void DrawCard(ImDrawList* dl, const hw::Card& card, const ImVec2& pos, float width, float height);
    bool Button(const char* id, const char* label, bool primary, bool enabled = true);

    void StartCheck();
    void ShowToast(std::string text);
    float S(float v) const { return v * dpi_; }

    void* hwnd_;
    float dpi_ = 1.0f;

    std::future<hw::Report> pending_;
    std::optional<hw::Report> report_;

    std::vector<float> sectionY_;   // content-space Y of each section header (previous frame)
    int activeSection_ = 0;         // highlighted in the sidebar
    int selectedSection_ = -1;      // last section clicked in the sidebar
    float scrollTarget_ = -1.0f;    // animated scroll destination, -1 when idle
    float pinnedScroll_ = -1.0f;    // scroll position reached after a sidebar click

    std::optional<io::Format> exportRequest_;

    // Compare ("Check") state
    bool checkRequest_ = false;
    bool checkPick_ = false;         // Shift+Check: choose the export file instead of the newest one
    std::wstring checkPath_;         // explicit baseline (from --compare)
    std::future<hw::Report> checking_;
    std::optional<hw::Report> baseline_;
    std::string baselineName_;
    bool compareActive_ = false;
    int changeCount_ = 0;

    std::string toast_;
    double toastTime_ = -100.0;
    bool toastPending_ = false;     // start the toast clock on the next frame (after a modal dialog)
};

} // namespace ui
