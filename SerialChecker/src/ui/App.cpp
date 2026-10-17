#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/App.h"
#include "ui/Theme.h"
#include "hw/Collector.h"
#include "io/Importer.h"

#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>

namespace ui {
namespace {

using namespace std::chrono_literals;

constexpr float kHeaderH = 64.0f;
constexpr float kSidebarW = 208.0f;
constexpr float kCardHeaderH = 46.0f;
constexpr float kCompareBarH = 42.0f;
constexpr float kTextPx = 13.5f;
constexpr float kMonoPx = 13.0f;

ImVec2 Measure(ImFont* font, float px, const char* text, float wrap = 0.0f)
{
    return font->CalcTextSizeA(px, FLT_MAX, wrap, text);
}

ImU32 WithAlpha(ImU32 c, float a)
{
    ImU32 alpha = (ImU32)(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f));
    return (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

struct CardMetrics {
    float padX, labelW, rowPadY, wrap;
};

CardMetrics Metrics(float width, float dpi)
{
    CardMetrics m;
    m.padX = 18 * dpi;
    m.labelW = std::clamp(width * 0.32f, 130 * dpi, 190 * dpi);
    m.rowPadY = 11 * dpi;
    m.wrap = width - m.padX * 2 - m.labelW;
    return m;
}

ImFont* ValueFont(const hw::Field& f) { return f.placeholder ? g_fonts.italic : g_fonts.mono; }
float ValuePx(const hw::Field& f) { return f.placeholder ? kTextPx : kMonoPx; }

// Size of a field row: the value, plus the struck-through previous value when "Check" found a change.
struct RowLayout {
    ImVec2 value, prev;
    float wasW = 0, height = 0;
};

RowLayout LayoutRow(const hw::Field& f, const CardMetrics& m, float dpi)
{
    RowLayout r;
    r.value = Measure(ValueFont(f), ValuePx(f) * dpi, f.value.c_str(), m.wrap);
    float h = r.value.y;
    if (f.diff == hw::Diff::Changed) {
        r.wasW = Measure(g_fonts.regular, 11.5f * dpi, "was").x + 8 * dpi;
        r.prev = Measure(g_fonts.mono, 12 * dpi, f.previous.c_str(), m.wrap - r.wasW);
        h += 5 * dpi + r.prev.y;
    }
    r.height = std::max(kTextPx * dpi, h) + m.rowPadY * 2;
    return r;
}

std::string FileName(const std::wstring& path)
{
    std::wstring file = path.substr(path.find_last_of(L"\\/") + 1);
    int n = WideCharToMultiByte(CP_UTF8, 0, file.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, file.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    return utf8;
}

std::string Plural(int n, const char* word) { return std::to_string(n) + " " + word + (n == 1 ? "" : "s"); }

} // namespace

App::App(void* hwnd) : hwnd_(hwnd)
{
    pending_ = std::async(std::launch::async, hw::Collect);
}

void App::RequestCheck(std::wstring baselinePath)
{
    checkPath_ = std::move(baselinePath);
    checkPick_ = false;
    checkRequest_ = true;
}

// Loads the baseline export, then re-reads the hardware in the background; Frame() applies the diff.
void App::StartCheck()
{
    std::wstring path = checkPath_;
    checkPath_.clear();
    if (path.empty()) path = checkPick_ ? io::PickOpenPath(hwnd_) : io::FindLatestExport();
    if (path.empty()) {
        if (!checkPick_) ShowToast("No serials_*.json on the Desktop: export a JSON first");
        return;
    }

    hw::Report base;
    std::string error;
    if (!io::ReadReport(path, base, error)) {
        ShowToast(error + ": " + FileName(path));
        return;
    }
    baseline_ = std::move(base);
    baselineName_ = FileName(path);
    checking_ = std::async(std::launch::async, hw::Collect);
}

void App::ShowToast(std::string text)
{
    toast_ = std::move(text);
    toastPending_ = true;
}

void App::Frame(float dpi)
{
    dpi_ = dpi;
    if (!report_ && pending_.valid() && pending_.wait_for(0s) == std::future_status::ready) {
        report_ = pending_.get();
        sectionY_.assign(report_->sections.size(), 0.0f);
    }
    if (checking_.valid() && checking_.wait_for(0s) == std::future_status::ready) {
        hw::Report fresh = checking_.get();
        changeCount_ = hw::ApplyDiff(fresh, *baseline_);
        report_ = std::move(fresh);
        sectionY_.resize(report_->sections.size(), 0.0f);
        compareActive_ = true;
        ShowToast(changeCount_ ? Plural(changeCount_, "change") + " detected" : std::string("No change since the export"));
    }
    if (toastPending_) {
        toastTime_ = ImGui::GetTime();
        toastPending_ = false;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, vp->Pos + vp->Size, col::Bg);

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    const ImGuiWindowFlags rootFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;
    ImGui::Begin("##root", nullptr, rootFlags);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    DrawHeader(dl, vp->Pos, vp->Size.x);
    float headerH = S(kHeaderH);
    if (report_ && compareActive_) {
        DrawCompareBar(dl, vp->Pos + ImVec2(0, headerH), vp->Size.x);
        headerH += S(kCompareBarH);
    }
    if (!report_) {
        DrawLoading(dl, vp->Pos + ImVec2(vp->Size.x * 0.5f, headerH + (vp->Size.y - headerH) * 0.45f));
    } else {
        const float sidebarW = S(kSidebarW);
        DrawSidebar(vp->Pos + ImVec2(0, headerH), ImVec2(sidebarW, vp->Size.y - headerH));
        DrawContent(vp->Pos + ImVec2(sidebarW, headerH), ImVec2(vp->Size.x - sidebarW, vp->Size.y - headerH));
    }
    ImGui::End();

    DrawToast(ImGui::GetForegroundDrawList(), vp->Pos + vp->Size);
}

void App::AfterFrame()
{
    if (!report_) return;

    if (checkRequest_ && !checking_.valid()) {
        checkRequest_ = false;
        StartCheck();
    }

    if (!exportRequest_) return;
    io::Format format = *exportRequest_;
    exportRequest_.reset();

    std::wstring path = io::PickSavePath(hwnd_, format, *report_);
    if (path.empty()) return;
    if (io::WriteReport(*report_, format, path)) ShowToast("Saved " + FileName(path));
    else ShowToast("Export failed");
}

// ---------------------------------------------------------------------------------------------------------------------

bool App::Button(const char* id, const char* label, bool primary, bool enabled)
{
    const ImVec2 ts = Measure(g_fonts.semibold, S(13), label);
    const ImVec2 size(ts.x + S(28), S(32));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size) && enabled;
    bool hovered = ImGui::IsItemHovered() && enabled, active = ImGui::IsItemActive() && enabled;
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (primary) {
        ImU32 fill = active ? IM_COL32(200, 200, 200, 255) : hovered ? IM_COL32(225, 225, 225, 255) : col::White;
        dl->AddRectFilled(p, p + size, fill, S(7));
    } else {
        dl->AddRectFilled(p, p + size, hovered ? col::SurfaceHover : col::Bg, S(7));
        dl->AddRect(p, p + size, hovered ? col::BorderStrong : col::Border, S(7));
    }
    ImU32 tc = primary ? col::Black : enabled ? col::Text : col::Label;
    dl->AddText(g_fonts.semibold, S(13), p + (size - ts) * 0.5f, tc, label);
    return clicked;
}

void App::DrawHeader(ImDrawList* dl, const ImVec2& o, float width)
{
    const float h = S(kHeaderH);
    dl->AddLine(o + ImVec2(0, h - 1), o + ImVec2(width, h - 1), col::Border);

    // Wordmark: a small white square + name, then the machine in grey.
    const float cy = o.y + h * 0.5f;
    const ImVec2 mark(o.x + S(24), cy - S(5));
    dl->AddRectFilled(mark, mark + ImVec2(S(10), S(10)), col::White, S(2));
    float x = mark.x + S(22);
    const char* name = "Serial Checker";
    ImVec2 ns = Measure(g_fonts.semibold, S(15), name);
    dl->AddText(g_fonts.semibold, S(15), ImVec2(x, cy - ns.y * 0.5f), col::Text, name);
    x += ns.x + S(14);

    std::string sub = report_ ? report_->computerName + "  \xC2\xB7  " + report_->capturedAt
                              : std::string("Reading hardware\xE2\x80\xA6");
    dl->AddLine(ImVec2(x, cy - S(8)), ImVec2(x, cy + S(8)), col::BorderStrong);
    x += S(14);
    ImVec2 ss = Measure(g_fonts.regular, S(13), sub.c_str());
    dl->AddText(g_fonts.regular, S(13), ImVec2(x, cy - ss.y * 0.5f), col::Label, sub.c_str());

    if (!report_) return;

    // Right side, laid out from the right edge.
    float rx = o.x + width - S(24);
    const float by = cy - S(16);
    auto place = [&](const char* label) {
        rx -= Measure(g_fonts.semibold, S(13), label).x + S(28);
        ImGui::SetCursorScreenPos(ImVec2(rx, by));
    };
    place("Export JSON");
    if (Button("##json", "Export JSON", true)) exportRequest_ = io::Format::Json;
    rx -= S(8);
    place("Export TXT");
    if (Button("##txt", "Export TXT", false)) exportRequest_ = io::Format::Text;
    rx -= S(8);

    // Check: re-read the hardware and diff it against the newest export on the Desktop (Shift = pick a file).
    const bool busy = checking_.valid();
    const char* checkLabel = busy ? "Checking\xE2\x80\xA6" : "Check";
    place(checkLabel);
    if (Button("##check", checkLabel, false, !busy)) {
        checkRequest_ = true;
        checkPick_ = ImGui::GetIO().KeyShift;
    }

    // Privilege status: filled dot = elevated, hollow dot = limited.
    const bool admin = report_->elevated;
    const char* text = admin ? "Administrator" : "Limited access";
    const ImVec2 ts = Measure(g_fonts.regular, S(12.5f), text);
    rx -= S(20) + ts.x;
    dl->AddText(g_fonts.regular, S(12.5f), ImVec2(rx, cy - ts.y * 0.5f), admin ? col::TextDim : col::Label, text);
    const ImVec2 dot(rx - S(10), cy);
    if (admin) dl->AddCircleFilled(dot, S(3.5f), col::White);
    else dl->AddCircle(dot, S(3.5f), col::Label, 0, S(1.2f));
}

// Strip under the header while a comparison is shown: result, baseline file, and a Clear action.
void App::DrawCompareBar(ImDrawList* dl, const ImVec2& o, float width)
{
    const float h = S(kCompareBarH);
    dl->AddRectFilled(o, o + ImVec2(width, h), col::Surface);
    dl->AddLine(o + ImVec2(0, h - 1), o + ImVec2(width, h - 1), col::Border);
    const float cy = o.y + h * 0.5f;

    float x = o.x + S(24);
    const bool any = changeCount_ > 0;
    if (any) dl->AddCircleFilled(ImVec2(x + S(4), cy), S(4), col::Green);
    else dl->AddCircle(ImVec2(x + S(4), cy), S(4), col::Label, 0, S(1.2f));
    x += S(18);

    std::string result = any ? Plural(changeCount_, "change") : std::string("No change");
    ImVec2 rs = Measure(g_fonts.semibold, S(13), result.c_str());
    dl->AddText(g_fonts.semibold, S(13), ImVec2(x, cy - rs.y * 0.5f), any ? col::Green : col::Text, result.c_str());
    x += rs.x + S(10);

    std::string detail = "since " + baselineName_;
    if (baseline_ && !baseline_->capturedAt.empty()) detail += "  \xC2\xB7  exported " + baseline_->capturedAt;
    ImVec2 ds = Measure(g_fonts.regular, S(13), detail.c_str());
    dl->AddText(g_fonts.regular, S(13), ImVec2(x, cy - ds.y * 0.5f), col::Label, detail.c_str());

    // "Clear" text button on the right
    const char* clear = "Clear";
    const ImVec2 cs = Measure(g_fonts.semibold, S(13), clear);
    const ImVec2 cp(o.x + width - S(24) - cs.x - S(16), cy - S(13));
    ImGui::SetCursorScreenPos(cp);
    bool clicked = ImGui::InvisibleButton("##clear", ImVec2(cs.x + S(16), S(26)));
    bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddRectFilled(cp, cp + ImVec2(cs.x + S(16), S(26)), col::SurfaceHover, S(6));
    }
    dl->AddText(g_fonts.semibold, S(13), cp + ImVec2(S(8), (S(26) - cs.y) * 0.5f), hovered ? col::Text : col::TextDim, clear);
    if (clicked) {
        hw::ClearDiff(*report_);
        compareActive_ = false;
        changeCount_ = 0;
    }
}

void App::DrawSidebar(const ImVec2& o, const ImVec2& size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(o.x + size.x - 1, o.y), ImVec2(o.x + size.x - 1, o.y + size.y), col::Border);

    float y = o.y + S(20);
    for (int i = 0; i < (int)report_->sections.size(); ++i) {
        const auto& section = report_->sections[i];
        const ImVec2 p(o.x + S(12), y);
        const ImVec2 sz(size.x - S(24), S(32));
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##nav", sz);
        bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (clicked) {
            selectedSection_ = i;
            scrollTarget_ = std::max(0.0f, sectionY_[i]);
            pinnedScroll_ = -1.0f;
        }

        const bool active = i == activeSection_;
        if (active) dl->AddRectFilled(p, p + sz, col::SurfaceHover, S(6));
        const ImVec2 ts = Measure(g_fonts.regular, S(13.5f), section.name.c_str());
        ImU32 tc = active ? col::Text : hovered ? col::TextDim : col::Label;
        dl->AddText(active ? g_fonts.semibold : g_fonts.regular, S(13.5f), p + ImVec2(S(12), (sz.y - ts.y) * 0.5f), tc,
                    section.name.c_str());

        // Card count, or the number of differences in green while comparing.
        const bool changed = section.changes > 0;
        std::string count = std::to_string(changed ? section.changes : (int)section.cards.size());
        const ImVec2 cs = Measure(g_fonts.mono, S(11.5f), count.c_str());
        const ImVec2 cpos(p.x + sz.x - S(12) - cs.x, p.y + (sz.y - cs.y) * 0.5f);
        if (changed) dl->AddCircleFilled(ImVec2(cpos.x - S(8), p.y + sz.y * 0.5f), S(3), col::Green);
        dl->AddText(g_fonts.mono, S(11.5f), cpos, changed ? col::Green : active ? col::Label : col::Muted, count.c_str());
        y += S(34);
    }

    dl->AddText(g_fonts.regular, S(12), ImVec2(o.x + S(24), o.y + size.y - S(52)), col::Muted, "Click a value to copy");
    dl->AddText(g_fonts.regular, S(12), ImVec2(o.x + S(24), o.y + size.y - S(34)), col::Muted, "Shift+Check: pick a file");
}

// ---------------------------------------------------------------------------------------------------------------------

float App::MeasureCard(const hw::Card& card, float width) const
{
    const CardMetrics m = Metrics(width, dpi_);
    float h = S(kCardHeaderH) + S(6);
    for (const auto& f : card.fields) h += LayoutRow(f, m, dpi_).height;
    return h + S(6);
}

void App::DrawCard(ImDrawList* dl, const hw::Card& card, const ImVec2& p, float w, float h)
{
    const CardMetrics m = Metrics(w, dpi_);
    const float headerH = S(kCardHeaderH);
    dl->AddRectFilled(p, p + ImVec2(w, h), col::Surface, S(10));
    dl->AddRect(p, p + ImVec2(w, h), col::Border, S(10));

    // Source tag
    float titleRight = p.x + w - m.padX;
    if (!card.badge.empty()) {
        const ImVec2 bs = Measure(g_fonts.mono, S(11), card.badge.c_str());
        const ImVec2 bsize(bs.x + S(14), S(20));
        const ImVec2 bp(p.x + w - m.padX - bsize.x, p.y + (headerH - bsize.y) * 0.5f);
        dl->AddRect(bp, bp + bsize, col::BorderStrong, S(5));
        dl->AddText(g_fonts.mono, S(11), bp + (bsize - bs) * 0.5f, col::Label, card.badge.c_str());
        titleRight = bp.x - S(10);
    }
    if (card.changes > 0) {
        const std::string text = std::to_string(card.changes) + " changed";
        const ImVec2 gs = Measure(g_fonts.semibold, S(11.5f), text.c_str());
        const ImVec2 gsize(gs.x + S(14), S(20));
        const ImVec2 gp(titleRight - gsize.x + S(2), p.y + (headerH - gsize.y) * 0.5f);
        dl->AddRectFilled(gp, gp + gsize, col::GreenSoft, S(5));
        dl->AddRect(gp, gp + gsize, col::GreenLine, S(5));
        dl->AddText(g_fonts.semibold, S(11.5f), gp + (gsize - gs) * 0.5f, col::Green, text.c_str());
        titleRight = gp.x - S(10);
    }

    const ImVec2 ts = Measure(g_fonts.semibold, S(13.5f), card.title.c_str());
    dl->PushClipRect(p, ImVec2(titleRight, p.y + headerH), true);
    dl->AddText(g_fonts.semibold, S(13.5f), ImVec2(p.x + m.padX, p.y + (headerH - ts.y) * 0.5f), col::Text,
                card.title.c_str());
    dl->PopClipRect();
    dl->AddLine(ImVec2(p.x + 1, p.y + headerH), ImVec2(p.x + w - 1, p.y + headerH), col::Hairline);

    float y = p.y + headerH + S(6);
    for (int j = 0; j < (int)card.fields.size(); ++j) {
        const auto& f = card.fields[j];
        ImFont* vf = ValueFont(f);
        const float vpx = S(ValuePx(f));
        const RowLayout lay = LayoutRow(f, m, dpi_);
        const ImVec2 vs = lay.value;
        const float rowH = lay.height;
        const bool diff = f.diff != hw::Diff::None;
        const ImVec2 rmin(p.x + S(6), y), rmax(p.x + w - S(6), y + rowH);

        ImGui::SetCursorScreenPos(rmin);
        ImGui::PushID(j);
        bool clicked = ImGui::InvisibleButton("##row", rmax - rmin);
        bool hovered = ImGui::IsItemHovered();
        bool held = ImGui::IsItemActive();
        ImGui::PopID();

        if (hovered) dl->AddRectFilled(rmin, rmax, held ? col::BorderStrong : col::SurfaceHover, S(6));
        else if (j + 1 < (int)card.fields.size())
            dl->AddLine(ImVec2(p.x + m.padX, rmax.y), ImVec2(p.x + w - m.padX, rmax.y), col::Hairline);
        if (diff) {
            dl->AddRectFilled(rmin + ImVec2(0, S(2)), rmax - ImVec2(0, S(2)), col::GreenSoft, S(6));
            dl->AddRectFilled(ImVec2(rmin.x, rmin.y + S(8)), ImVec2(rmin.x + S(2), rmax.y - S(8)), col::Green, S(1));
        }

        const float textY = y + m.rowPadY;
        dl->PushClipRect(ImVec2(p.x, y), ImVec2(p.x + m.padX + m.labelW - S(12), rmax.y), true);
        dl->AddText(g_fonts.regular, S(kTextPx), ImVec2(p.x + m.padX, textY), col::Label, f.label.c_str());
        dl->PopClipRect();

        // Mono glyphs sit slightly higher than Segoe UI at the same box: nudge to share a baseline.
        const float vx = p.x + m.padX + m.labelW;
        const float vy = textY + (f.placeholder ? 0.0f : S(1));
        const ImU32 valueCol = diff ? col::Green : f.placeholder ? col::Muted : col::Text;
        dl->AddText(vf, vpx, ImVec2(vx, vy), valueCol, f.value.c_str(), nullptr, m.wrap);

        if (f.diff == hw::Diff::Changed) {
            // "was  <old value>" under the new one, struck through.
            const float py = vy + vs.y + S(5);
            dl->AddText(g_fonts.regular, S(11.5f), ImVec2(vx, py + S(0.5f)), col::Muted, "was");
            const float px = vx + lay.wasW;
            dl->AddText(g_fonts.mono, S(12), ImVec2(px, py), col::Label, f.previous.c_str(), nullptr, m.wrap - lay.wasW);
            if (lay.prev.y < S(12) * 1.6f)
                dl->AddLine(ImVec2(px, py + lay.prev.y * 0.55f), ImVec2(px + lay.prev.x, py + lay.prev.y * 0.55f), col::Label);
        } else if (f.diff == hw::Diff::Added && !hovered) {
            const ImVec2 ns = Measure(g_fonts.semibold, S(11), "NEW");
            dl->AddText(g_fonts.semibold, S(11), ImVec2(p.x + w - m.padX - ns.x, textY + S(1.5f)), col::Green, "NEW");
        }

        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            // "Copy" hint only when it doesn't collide with the value's first line.
            const char* copy = "Copy";
            const ImVec2 cs = Measure(g_fonts.regular, S(12), copy);
            const float hintX = p.x + w - m.padX - cs.x;
            const bool singleLine = vs.y < vpx * 1.6f;
            if (singleLine && vx + vs.x + S(16) < hintX)
                dl->AddText(g_fonts.regular, S(12), ImVec2(hintX, textY + S(1)), col::TextDim, copy);
        }
        if (clicked) {
            ImGui::SetClipboardText(f.value.c_str());
            ShowToast("Copied " + f.label);
        }
        y += rowH;
    }
}

void App::DrawContent(const ImVec2& o, const ImVec2& size)
{
    ImGui::SetCursorScreenPos(o);
    ImGui::BeginChild("##content", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Smooth scroll toward the section picked in the sidebar.
    if (scrollTarget_ >= 0) {
        const float target = std::min(scrollTarget_, ImGui::GetScrollMaxY());
        const float cur = ImGui::GetScrollY();
        if (io.MouseWheel != 0.0f) {
            scrollTarget_ = -1.0f;
            selectedSection_ = -1;
        } else if (std::fabs(target - cur) < 0.5f) {
            ImGui::SetScrollY(target);
            pinnedScroll_ = target;
            scrollTarget_ = -1.0f;
        } else {
            ImGui::SetScrollY(cur + (target - cur) * std::min(1.0f, io.DeltaTime * 14.0f));
        }
    }

    const float pad = S(32);
    const float innerW = std::max(S(300), size.x - pad * 2 - S(8));
    const float gap = S(16);
    const int cols = innerW >= S(860) ? 2 : 1;
    const float cardW = (innerW - gap * (cols - 1)) / cols;

    ImGui::SetCursorPos(ImVec2(pad, S(28)));
    for (int i = 0; i < (int)report_->sections.size(); ++i) {
        const auto& section = report_->sections[i];
        ImGui::SetCursorPosX(pad);
        sectionY_[i] = ImGui::GetCursorPosY() - S(24);

        const ImVec2 hp = ImGui::GetCursorScreenPos();
        const ImVec2 ns = Measure(g_fonts.semibold, S(17), section.name.c_str());
        dl->AddText(g_fonts.semibold, S(17), hp, col::Text, section.name.c_str());
        std::string count = std::to_string(section.cards.size());
        const ImVec2 cs = Measure(g_fonts.mono, S(12), count.c_str());
        dl->AddText(g_fonts.mono, S(12), ImVec2(hp.x + ns.x + S(10), hp.y + ns.y - cs.y - S(2)), col::Muted, count.c_str());
        if (section.changes > 0) {
            std::string changed = Plural(section.changes, "change");
            const ImVec2 gs = Measure(g_fonts.semibold, S(12.5f), changed.c_str());
            dl->AddText(g_fonts.semibold, S(12.5f), ImVec2(hp.x + ns.x + cs.x + S(24), hp.y + ns.y - gs.y - S(1)), col::Green,
                        changed.c_str());
        }
        ImGui::Dummy(ImVec2(innerW, ns.y + S(14)));

        for (size_t c = 0; c < section.cards.size(); c += cols) {
            float rowH = 0;
            for (int k = 0; k < cols && c + k < section.cards.size(); ++k)
                rowH = std::max(rowH, MeasureCard(section.cards[c + k], cardW));

            ImGui::SetCursorPosX(pad);
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            for (int k = 0; k < cols && c + k < section.cards.size(); ++k) {
                ImGui::PushID(i * 1000 + (int)(c + k));
                DrawCard(dl, section.cards[c + k], rp + ImVec2(k * (cardW + gap), 0), cardW, rowH);
                ImGui::PopID();
            }
            ImGui::SetCursorScreenPos(rp);
            ImGui::Dummy(ImVec2(innerW, rowH + gap));
        }
        ImGui::SetCursorPosX(pad);
        ImGui::Dummy(ImVec2(innerW, S(24)));
    }
    ImGui::SetCursorPosX(pad);
    ImGui::Dummy(ImVec2(innerW, pad));

    // Sidebar highlight: the last section whose header is above the top of the view.
    const float scrollY = ImGui::GetScrollY();
    int active = 0;
    for (int i = 0; i < (int)sectionY_.size(); ++i)
        if (sectionY_[i] <= scrollY + size.y * 0.25f) active = i;
    if (ImGui::GetScrollMaxY() > 0 && scrollY >= ImGui::GetScrollMaxY() - 1) active = (int)sectionY_.size() - 1;
    if (selectedSection_ >= 0) {
        bool stillThere = scrollTarget_ >= 0 || (pinnedScroll_ >= 0 && std::fabs(scrollY - pinnedScroll_) < 1.0f);
        if (stillThere) active = selectedSection_;
        else selectedSection_ = -1;
    }
    activeSection_ = active;

    ImGui::EndChild();
}

void App::DrawLoading(ImDrawList* dl, const ImVec2& center)
{
    const float t = (float)ImGui::GetTime();
    const float r = S(14);
    dl->AddCircle(center, r, col::Border, 48, S(2));
    dl->PathArcTo(center, r, t * 5.0f, t * 5.0f + 1.6f, 32);
    dl->PathStroke(col::White, ImDrawFlags_None, S(2));
    const char* text = "Reading hardware\xE2\x80\xA6";
    const ImVec2 ts = Measure(g_fonts.regular, S(13.5f), text);
    dl->AddText(g_fonts.regular, S(13.5f), ImVec2(center.x - ts.x * 0.5f, center.y + r + S(16)), col::Label, text);
}

void App::DrawToast(ImDrawList* dl, const ImVec2& vmax)
{
    if (toast_.empty()) return;
    const float t = (float)(ImGui::GetTime() - toastTime_);
    if (t < 0 || t > 2.0f) return;
    const float in = std::min(1.0f, t / 0.15f);
    const float alpha = t > 1.6f ? (2.0f - t) / 0.4f : in;

    const ImVec2 ts = Measure(g_fonts.semibold, S(13), toast_.c_str());
    const ImVec2 size(ts.x + S(32), S(38));
    const ImVec2 p(vmax.x - size.x - S(24), vmax.y - size.y - S(24) + (1.0f - in) * S(8));
    dl->AddRectFilled(p, p + size, WithAlpha(col::White, alpha), S(8));
    dl->AddText(g_fonts.semibold, S(13), p + (size - ts) * 0.5f, WithAlpha(col::Black, alpha), toast_.c_str());
}

} // namespace ui
