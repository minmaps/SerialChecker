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

// Registry helpers (HKLM, 64-bit view). Return empty string when missing.
std::string RegReadString(const wchar_t* subKey, const wchar_t* valueName);
bool RegReadDword(const wchar_t* subKey, const wchar_t* valueName, uint32_t& out);

// Probing functions, one per hardware family. Each appends its sections.
void CollectSmbios(std::vector<Section>& out);
void CollectDisks(std::vector<Section>& out);
void CollectNetwork(std::vector<Section>& out);
void CollectMonitors(std::vector<Section>& out);
void CollectWindows(std::vector<Section>& out);

} // namespace hw
