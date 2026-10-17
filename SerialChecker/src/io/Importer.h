#pragma once
#include "hw/Types.h"
#include <string>

namespace io {

// Loads a report previously written by WriteReport(..., Format::Json).
bool ReadReport(const std::wstring& path, hw::Report& out, std::string& error);

std::wstring DesktopDir();

// Newest "serials_*.json" on the Desktop, or empty when there is none.
std::wstring FindLatestExport();

// Native "Open" dialog for a .json export, starting on the Desktop. Empty when cancelled.
std::wstring PickOpenPath(void* ownerHwnd);

} // namespace io
