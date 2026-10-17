#include "hw/Util.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdarg>

namespace hw {

std::string Narrow(const std::wstring& s)
{
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
    return r;
}

std::string Trim(std::string s)
{
    // Firmware strings are often padded with spaces or NULs; drop control chars too.
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 && c != '\t'; }), s.end());
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string Format(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

std::string HexBytes(const uint8_t* data, size_t size, char sep)
{
    static const char* digits = "0123456789ABCDEF";
    std::string r;
    for (size_t i = 0; i < size; ++i) {
        if (i && sep) r += sep;
        r += digits[data[i] >> 4];
        r += digits[data[i] & 0xF];
    }
    return r;
}

bool IsPlaceholder(const std::string& value)
{
    if (value.empty()) return true;
    std::string v;
    for (char c : value) v += (char)std::tolower((unsigned char)c);

    static const char* defaults[] = {
        "default string", "to be filled by o.e.m.", "to be filled by oem", "unknown", "not specified",
        "not applicable", "none", "n/a", "system serial number", "system product name", "system manufacturer",
        "system version", "base board serial number", "chassis serial number", "0123456789", "123456789",
        "not supported", "not available", "empty", "not set", "oem", "o.e.m.",
    };
    for (const char* d : defaults)
        if (v == d) return true;

    // All zeros / all F (with or without separators) means "nothing programmed".
    bool allZero = true, allF = true, anyHex = false;
    for (char c : v) {
        if (c == ' ' || c == '-' || c == ':' || c == '_' || c == '.') continue;
        anyHex = true;
        if (c != '0') allZero = false;
        if (c != 'f') allF = false;
    }
    return anyHex && (allZero || allF);
}

Field MakeField(std::string label, std::string value)
{
    Field f;
    f.label = std::move(label);
    f.value = Trim(std::move(value));
    f.placeholder = IsPlaceholder(f.value);
    if (f.value.empty()) f.value = "Empty";
    return f;
}

Field Unsupported(std::string label, const char* why)
{
    return Field{ std::move(label), why, true };
}

bool IsProcessElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

std::string RegReadString(const wchar_t* subKey, const wchar_t* valueName)
{
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subKey, valueName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY,
                     nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return Narrow(buf);
}

bool RegReadDword(const wchar_t* subKey, const wchar_t* valueName, uint32_t& out)
{
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subKey, valueName, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &v,
                     &size) != ERROR_SUCCESS)
        return false;
    out = v;
    return true;
}

} // namespace hw
