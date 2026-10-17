#include "io/Exporter.h"
#include "hw/Util.h"

#include <windows.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstdio>

namespace io {
namespace {

std::string JsonEscape(const std::string& s)
{
    std::string r;
    for (unsigned char c : s) {
        switch (c) {
        case '"': r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n"; break;
        case '\r': r += "\\r"; break;
        case '\t': r += "\\t"; break;
        default:
            if (c < 0x20) r += hw::Format("\\u%04x", c);
            else r += (char)c;
        }
    }
    return r;
}

std::string Quote(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }

} // namespace

std::string ToText(const hw::Report& report)
{
    std::string t;
    t += "SERIAL CHECKER\n";
    t += "Computer : " + report.computerName + "\n";
    t += "Captured : " + report.capturedAt + "\n";
    t += std::string("Elevated : ") + (report.elevated ? "yes" : "no") + "\n";

    for (auto& section : report.sections) {
        t += "\n==================== " + section.name + " ====================\n";
        for (auto& card : section.cards) {
            t += "\n[" + card.title + "]  (" + card.badge + ")\n";
            size_t width = 0;
            for (auto& f : card.fields) width = std::max(width, f.label.size());
            for (auto& f : card.fields)
                t += "  " + f.label + std::string(width - f.label.size() + 2, ' ') + f.value + "\n";
        }
    }
    return t;
}

std::string ToJson(const hw::Report& report)
{
    std::string j = "{\n";
    j += "  \"computer\": " + Quote(report.computerName) + ",\n";
    j += "  \"captured\": " + Quote(report.capturedAt) + ",\n";
    j += std::string("  \"elevated\": ") + (report.elevated ? "true" : "false") + ",\n";
    j += "  \"sections\": [";
    for (size_t s = 0; s < report.sections.size(); ++s) {
        auto& section = report.sections[s];
        j += s ? ",\n" : "\n";
        j += "    {\n      \"name\": " + Quote(section.name) + ",\n      \"cards\": [";
        for (size_t c = 0; c < section.cards.size(); ++c) {
            auto& card = section.cards[c];
            j += c ? ",\n" : "\n";
            j += "        {\n          \"title\": " + Quote(card.title) + ",\n          \"source\": " + Quote(card.badge) +
                 ",\n          \"fields\": [";
            for (size_t f = 0; f < card.fields.size(); ++f) {
                auto& field = card.fields[f];
                j += f ? ",\n" : "\n";
                j += "            { \"label\": " + Quote(field.label) + ", \"value\": " + Quote(field.value) +
                     ", \"placeholder\": " + (field.placeholder ? "true" : "false") + " }";
            }
            j += "\n          ]\n        }";
        }
        j += "\n      ]\n    }";
    }
    j += "\n  ]\n}\n";
    return j;
}

bool WriteReport(const hw::Report& report, Format format, const std::wstring& path)
{
    std::string data = format == Format::Json ? ToJson(report) : ToText(report);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

std::wstring PickSavePath(void* ownerHwnd, Format format, const hw::Report& report)
{
    std::wstring result;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;

    const bool json = format == Format::Json;
    COMDLG_FILTERSPEC spec[] = { json ? COMDLG_FILTERSPEC{ L"JSON (*.json)", L"*.json" }
                                      : COMDLG_FILTERSPEC{ L"Text (*.txt)", L"*.txt" } };
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(json ? L"json" : L"txt");

    std::string stamp = report.capturedAt;  // "YYYY-MM-DD HH:MM:SS" -> "YYYY-MM-DD_HH-MM-SS"
    for (auto& ch : stamp) {
        if (ch == ' ') ch = '_';
        else if (ch == ':') ch = '-';
    }
    std::wstring name = L"serials_" + hw::Widen(report.computerName) + L"_" + hw::Widen(stamp);
    dlg->SetFileName(name.c_str());
    dlg->SetTitle(json ? L"Export JSON" : L"Export TXT");

    if (SUCCEEDED(dlg->Show((HWND)ownerHwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

} // namespace io
