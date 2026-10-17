#include "io/Importer.h"
#include "hw/Util.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <cstdio>
#include <cstdlib>

namespace io {
namespace {

// Minimal JSON reader: enough for the files this app writes (objects, arrays, strings, bools, numbers, null).
struct JValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JValue> items;      // Array values, or Object values
    std::vector<std::string> keys;  // Object keys, parallel to items

    const JValue* Get(const char* key) const
    {
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return &items[i];
        return nullptr;
    }
    std::string Str(const char* key) const
    {
        const JValue* v = Get(key);
        return v && v->type == String ? v->str : std::string();
    }
    bool Flag(const char* key) const
    {
        const JValue* v = Get(key);
        return v && v->type == Bool && v->b;
    }
    const std::vector<JValue>& Arr(const char* key) const
    {
        static const std::vector<JValue> empty;
        const JValue* v = Get(key);
        return v && v->type == Array ? v->items : empty;
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool Parse(JValue& out)
    {
        if (!Value(out, 0)) return false;
        Skip();
        return i_ == s_.size();
    }

private:
    void Skip()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool Eat(char c)
    {
        Skip();
        if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; }
        return false;
    }
    bool Literal(const char* word)
    {
        size_t n = strlen(word);
        if (s_.compare(i_, n, word) != 0) return false;
        i_ += n;
        return true;
    }

    static void AppendUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 0x3F));
            out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(unsigned& cp)
    {
        if (i_ + 4 > s_.size()) return false;
        char buf[5] = { s_[i_], s_[i_ + 1], s_[i_ + 2], s_[i_ + 3], 0 };
        char* end = nullptr;
        cp = (unsigned)strtoul(buf, &end, 16);
        if (end != buf + 4) return false;
        i_ += 4;
        return true;
    }

    bool String(std::string& out)
    {
        if (!Eat('"')) return false;
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) return false;
            char e = s_[i_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && Literal("\\u")) {  // surrogate pair
                    unsigned lo;
                    if (!Hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool Value(JValue& v, int depth)
    {
        if (depth > 32) return false;
        Skip();
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '"') { v.type = JValue::String; return String(v.str); }
        if (c == '{') {
            ++i_;
            v.type = JValue::Object;
            if (Eat('}')) return true;
            do {
                std::string key;
                if (!String(key) || !Eat(':')) return false;
                v.keys.push_back(std::move(key));
                v.items.emplace_back();
                if (!Value(v.items.back(), depth + 1)) return false;
            } while (Eat(','));
            return Eat('}');
        }
        if (c == '[') {
            ++i_;
            v.type = JValue::Array;
            if (Eat(']')) return true;
            do {
                v.items.emplace_back();
                if (!Value(v.items.back(), depth + 1)) return false;
            } while (Eat(','));
            return Eat(']');
        }
        if (Literal("true")) { v.type = JValue::Bool; v.b = true; return true; }
        if (Literal("false")) { v.type = JValue::Bool; return true; }
        if (Literal("null")) return true;
        char* end = nullptr;
        v.num = strtod(s_.c_str() + i_, &end);
        if (end == s_.c_str() + i_) return false;
        v.type = JValue::Number;
        i_ = end - s_.c_str();
        return true;
    }

    const std::string& s_;
    size_t i_ = 0;
};

} // namespace

bool ReadReport(const std::wstring& path, hw::Report& out, std::string& error)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) { error = "Cannot open file"; return false; }
    std::string text;
    char buf[16384];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    fclose(f);
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);

    JValue root;
    if (!Parser(text).Parse(root) || root.type != JValue::Object || !root.Get("sections")) {
        error = "Not a Serial Checker JSON export";
        return false;
    }

    out = hw::Report{};
    out.computerName = root.Str("computer");
    out.capturedAt = root.Str("captured");
    out.elevated = root.Flag("elevated");
    for (const auto& js : root.Arr("sections")) {
        hw::Section s{ js.Str("name") };
        for (const auto& jc : js.Arr("cards")) {
            hw::Card c{ jc.Str("title"), jc.Str("source") };
            for (const auto& jf : jc.Arr("fields"))
                c.fields.push_back(hw::Field{ jf.Str("label"), jf.Str("value"), jf.Flag("placeholder") });
            s.cards.push_back(std::move(c));
        }
        out.sections.push_back(std::move(s));
    }
    return true;
}

std::wstring DesktopDir()
{
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir;
}

std::wstring FindLatestExport()
{
    std::wstring dir = DesktopDir();
    if (dir.empty()) return {};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\serials_*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring best;
    FILETIME bestTime{};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (best.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) {
            best = dir + L"\\" + fd.cFileName;
            bestTime = fd.ftLastWriteTime;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

std::wstring PickOpenPath(void* ownerHwnd)
{
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;

    COMDLG_FILTERSPEC spec[] = { { L"Serial Checker export (*.json)", L"*.json" } };
    dlg->SetFileTypes(1, spec);
    dlg->SetTitle(L"Compare with an export");
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(DesktopDir().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        dlg->SetFolder(folder);
        folder->Release();
    }
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
