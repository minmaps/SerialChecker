#pragma once
#include "hw/Types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace hw {

std::string Narrow(const std::wstring& s);
std::wstring Widen(const std::string& s);
std::string Trim(std::string s);
std::string Format(const char* fmt, ...);

// "AA:BB:CC" style hex dump.
std::string HexBytes(const uint8_t* data, size_t size, char sep = ':');

// Builds a field and flags OEM defaults / empty values as placeholders.
Field MakeField(std::string label, std::string value);
Field Unsupported(std::string label, const char* why = "Not supported");

bool IsPlaceholder(const std::string& value);
bool IsProcessElevated();

// DeviceIoControl with a deadline, on a handle opened with FILE_FLAG_OVERLAPPED: a device that stops answering
// (crashed SSD firmware, dying disk, wedged USB device) would otherwise block the whole collection forever.
// After one timeout the device is marked hung and every later call fails at once.
class TimedDevice {
public:
    TimedDevice(void* handle, unsigned long timeoutMs) : h_(handle), timeoutMs_(timeoutMs) {}
    bool hung = false;
    bool Ioctl(unsigned long code, const void* in, unsigned long inSize, void* out, unsigned long outSize,
               unsigned long* returned);

private:
    void* h_;
    unsigned long timeoutMs_;
};

// Registry helpers (HKLM, 64-bit view). Return empty string when missing.
std::string RegReadString(const wchar_t* subKey, const wchar_t* valueName);
bool RegReadDword(const wchar_t* subKey, const wchar_t* valueName, uint32_t& out);
std::vector<uint8_t> RegReadBinary(const wchar_t* subKey, const wchar_t* valueName);
std::vector<std::string> RegReadMultiString(const wchar_t* subKey, const wchar_t* valueName);

// Probing functions, one per hardware family. Each appends its sections.
void CollectSmbios(std::vector<Section>& out);
void CollectAcpi(std::vector<Section>& out);  // adds its card to the System section
void CollectTpm(std::vector<Section>& out);
void CollectDisks(std::vector<Section>& out);
void CollectNetwork(std::vector<Section>& out);
void CollectUsb(std::vector<Section>& out);
void CollectMonitors(std::vector<Section>& out);
void CollectGraphics(std::vector<Section>& out);
void CollectWindows(std::vector<Section>& out);

} // namespace hw
