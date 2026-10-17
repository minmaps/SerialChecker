#pragma once
#include "hw/Types.h"
#include <string>

namespace io {

enum class Format { Text, Json };

std::string ToText(const hw::Report& report);
std::string ToJson(const hw::Report& report);

// Writes the report as UTF-8. Returns false on I/O error.
bool WriteReport(const hw::Report& report, Format format, const std::wstring& path);

// Native "Save as" dialog. Returns an empty string when cancelled.
std::wstring PickSavePath(void* ownerHwnd, Format format, const hw::Report& report);

} // namespace io
