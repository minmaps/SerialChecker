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

bool TimedDevice::Ioctl(DWORD code, const void* in, DWORD inSize, void* out, DWORD outSize, DWORD* returned)
{
    if (hung) return false;
    // The output buffer lives on the heap: a request the driver never completes must not write into freed memory.
    struct Request {
        OVERLAPPED ov{};
        std::vector<BYTE> out;
    };
    auto* r = new Request;
    r->out.resize(outSize);
    r->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = DeviceIoControl(h_, code, const_cast<void*>(in), inSize, r->out.data(), outSize, nullptr, &r->ov);
    if (ok || GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(r->ov.hEvent, timeoutMs_) == WAIT_TIMEOUT) {
            hung = true;
            CancelIoEx(h_, &r->ov);
            if (WaitForSingleObject(r->ov.hEvent, 1000) == WAIT_TIMEOUT) return false;  // leak r: still owned by the driver
        }
        DWORD n = 0;
        ok = GetOverlappedResult(h_, &r->ov, &n, FALSE);
        if (ok) {
            if (out && n) memcpy(out, r->out.data(), n);
            *returned = n;
        }
    }
    CloseHandle(r->ov.hEvent);
    delete r;
    return ok;
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

std::vector<uint8_t> RegReadBinary(const wchar_t* subKey, const wchar_t* valueName)
{
    const DWORD flags = RRF_RT_REG_BINARY | RRF_SUBKEY_WOW6464KEY;
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subKey, valueName, flags, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return {};
    std::vector<uint8_t> data(size);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subKey, valueName, flags, nullptr, data.data(), &size) != ERROR_SUCCESS) return {};
    data.resize(size);
    return data;
}

std::vector<std::string> RegReadMultiString(const wchar_t* subKey, const wchar_t* valueName)
{
    const DWORD flags = RRF_RT_REG_MULTI_SZ | RRF_SUBKEY_WOW6464KEY;
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subKey, valueName, flags, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return {};
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2);
    size = (DWORD)(buf.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subKey, valueName, flags, nullptr, buf.data(), &size) != ERROR_SUCCESS) return {};
    buf.back() = buf[buf.size() - 2] = 0;  // guarantee the double-NUL terminator
    std::vector<std::string> out;
    for (const wchar_t* s = buf.data(); *s; s += wcslen(s) + 1) out.push_back(Narrow(s));
    return out;
}

} // namespace hw
