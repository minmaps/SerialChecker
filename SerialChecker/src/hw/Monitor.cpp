// Monitors: EDID from the device registry key of each present monitor interface, matched to \\.\DISPLAYn.
#include "hw/Util.h"

#include <windows.h>
#include <setupapi.h>
#include <algorithm>

namespace hw {
namespace {

// GUID_DEVINTERFACE_MONITOR {E6F07B5F-EE97-4A90-B076-33F57BF4EAA7}
const GUID kMonitorInterface = { 0xe6f07b5f, 0xee97, 0x4a90, { 0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7 } };

struct ActiveDisplay {
    std::string displayName;  // "DISPLAY2"
    DWORD hz = 0;
    DWORD width = 0, height = 0;
};

std::wstring Lower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// Maps each monitor interface path to the display it is attached to.
std::vector<std::pair<std::wstring, ActiveDisplay>> EnumerateDisplays()
{
    std::vector<std::pair<std::wstring, ActiveDisplay>> result;
    DISPLAY_DEVICEW adapter{ sizeof(adapter) };
    for (DWORD a = 0; EnumDisplayDevicesW(nullptr, a, &adapter, 0); ++a) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        bool hasMode = EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &dm);

        DISPLAY_DEVICEW mon{ sizeof(mon) };
        for (DWORD m = 0; EnumDisplayDevicesW(adapter.DeviceName, m, &mon, EDD_GET_DEVICE_INTERFACE_NAME); ++m) {
            if (!(mon.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
            ActiveDisplay d;
            std::wstring name = adapter.DeviceName;
            if (name.rfind(L"\\\\.\\", 0) == 0) name = name.substr(4);
            d.displayName = Narrow(name);
            if (hasMode) {
                d.hz = dm.dmDisplayFrequency;
                d.width = dm.dmPelsWidth;
                d.height = dm.dmPelsHeight;
            }
            result.emplace_back(Lower(mon.DeviceID), d);
            mon = { sizeof(mon) };
        }
        adapter = { sizeof(adapter) };
    }
    return result;
}

std::string EdidText(const BYTE* edid, size_t size, BYTE tag)
{
    for (size_t off = 54; off + 18 <= std::min<size_t>(size, 126); off += 18) {
        const BYTE* d = edid + off;
        if (d[0] || d[1] || d[2] || d[3] != tag) continue;
        std::string s;
        for (int i = 5; i < 18 && d[i] != 0x0A && d[i] != 0; ++i) s += (char)d[i];
        return Trim(s);
    }
    return {};
}

} // namespace

void CollectMonitors(std::vector<Section>& out)
{
    Section section{ "Monitors" };
    auto displays = EnumerateDisplays();

    HDEVINFO devs = SetupDiGetClassDevsW(&kMonitorInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA iface{ sizeof(iface) };
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devs, nullptr, &kMonitorInterface, i, &iface); ++i) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(devs, &iface, nullptr, 0, &need, nullptr);
            std::vector<BYTE> detailBuf(need ? need : sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W));
            auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)detailBuf.data();
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA dev{ sizeof(dev) };
            if (!SetupDiGetDeviceInterfaceDetailW(devs, &iface, detail, need, nullptr, &dev)) continue;

            const ActiveDisplay* display = nullptr;
            std::wstring path = Lower(detail->DevicePath);
            for (auto& [id, d] : displays)
                if (id == path) display = &d;

            BYTE edid[512]{};
            DWORD edidSize = 0;
            HKEY key = SetupDiOpenDevRegKey(devs, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (key != INVALID_HANDLE_VALUE) {
                edidSize = sizeof(edid);
                if (RegQueryValueExW(key, L"EDID", nullptr, nullptr, edid, &edidSize) != ERROR_SUCCESS) edidSize = 0;
                RegCloseKey(key);
            }

            std::string title = display ? Format("Active Monitor: \\%s \\ %luHz", display->displayName.c_str(), display->hz)
                                        : std::string("Monitor (inactive)");
            Card c{ title, "EDID" };
            if (edidSize < 128) {
                c.fields.push_back(Unsupported("EDID", "Not available"));
            } else {
                WORD mfg = (WORD)(edid[8] << 8 | edid[9]);
                char pnp[4] = { (char)('A' - 1 + ((mfg >> 10) & 31)), (char)('A' - 1 + ((mfg >> 5) & 31)),
                                (char)('A' - 1 + (mfg & 31)), 0 };
                WORD product = (WORD)(edid[10] | edid[11] << 8);
                DWORD idSerial = edid[12] | edid[13] << 8 | edid[14] << 16 | (DWORD)edid[15] << 24;

                c.fields.push_back(MakeField("Manufacturer", pnp));
                c.fields.push_back(MakeField("Model Name", EdidText(edid, edidSize, 0xFC)));
                c.fields.push_back(MakeField("Monitor Serial", EdidText(edid, edidSize, 0xFF)));
                c.fields.push_back(MakeField("ID Serial Number", idSerial ? Format("%lu", idSerial) : ""));
                c.fields.push_back(MakeField("Product Code", Format("%s%04X", pnp, product)));
                if (edid[17]) c.fields.push_back(MakeField("Manufactured", Format("Week %u, %u", edid[16], 1990 + edid[17])));
            }
            if (display && display->width)
                c.fields.push_back(MakeField("Resolution", Format("%lu x %lu", display->width, display->height)));
            section.cards.push_back(std::move(c));
            iface = { sizeof(iface) };
        }
        SetupDiDestroyDeviceInfoList(devs);
    }
    // Active monitors first, ordered by display number.
    std::stable_sort(section.cards.begin(), section.cards.end(), [](const Card& a, const Card& b) {
        bool aa = a.title.rfind("Active", 0) == 0, ba = b.title.rfind("Active", 0) == 0;
        if (aa != ba) return aa;
        return a.title < b.title;
    });
    if (section.cards.empty()) section.cards.push_back({ "Monitors", "EDID", { Unsupported("Status", "No monitor found") } });
    out.push_back(std::move(section));
}

} // namespace hw
