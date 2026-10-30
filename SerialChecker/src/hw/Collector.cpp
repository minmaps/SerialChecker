#include "hw/Collector.h"
#include "hw/Util.h"

#include <windows.h>
#include <ctime>

namespace hw {

Report Collect()
{
    Report r;
    r.elevated = IsProcessElevated();

    wchar_t name[256];
    DWORD len = _countof(name);
    if (GetComputerNameW(name, &len)) r.computerName = Narrow(name);

    time_t now = time(nullptr);
    tm local{};
    localtime_s(&local, &now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    r.capturedAt = buf;

    CollectSmbios(r.sections);
    CollectTpm(r.sections);
    CollectDisks(r.sections);
    CollectNetwork(r.sections);
    CollectMonitors(r.sections);
    CollectWindows(r.sections);
    return r;
}

} // namespace hw
