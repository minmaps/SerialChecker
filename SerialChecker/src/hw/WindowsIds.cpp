// Windows-side identifiers (registry / hardware profile) and graphics adapters (DXGI).
#include "hw/Util.h"

#include <windows.h>
#include <dxgi.h>
#include <ctime>

namespace hw {

void CollectWindows(std::vector<Section>& out)
{
    // GPU
    Section gpu{ "Graphics" };
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            adapter->Release();
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            Card c{ Narrow(d.Description), "DXGI" };
            c.fields.push_back(MakeField("Vendor / Device", Format("VEN_%04X  DEV_%04X", d.VendorId, d.DeviceId)));
            c.fields.push_back(MakeField("Subsystem", Format("SUBSYS_%08X  REV_%02X", d.SubSysId, d.Revision)));
            c.fields.push_back(MakeField("Dedicated VRAM", Format("%llu MB", (unsigned long long)d.DedicatedVideoMemory >> 20)));
            c.fields.push_back(MakeField("Adapter LUID", Format("%08lX-%08lX", d.AdapterLuid.HighPart, d.AdapterLuid.LowPart)));
            gpu.cards.push_back(std::move(c));
        }
        factory->Release();
    }
    if (!gpu.cards.empty()) out.push_back(std::move(gpu));

    // Windows
    Section win{ "Windows" };
    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

    Card os{ "Operating System", "Registry" };
    std::string product = RegReadString(cv, L"ProductName");
    std::string build = RegReadString(cv, L"CurrentBuild");
    if (atoi(build.c_str()) >= 22000 && product.rfind("Windows 10", 0) == 0) product.replace(8, 2, "11");
    uint32_t ubr = 0;
    RegReadDword(cv, L"UBR", ubr);
    os.fields.push_back(MakeField("Edition", product));
    os.fields.push_back(MakeField("Version", RegReadString(cv, L"DisplayVersion")));
    os.fields.push_back(MakeField("Build", ubr ? build + "." + std::to_string(ubr) : build));
    uint32_t installDate = 0;
    if (RegReadDword(cv, L"InstallDate", installDate)) {
        time_t t = installDate;
        tm local{};
        localtime_s(&local, &t);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
        os.fields.push_back(MakeField("Install Date", buf));
    }
    wchar_t computer[256];
    DWORD len = _countof(computer);
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, computer, &len))
        os.fields.push_back(MakeField("Computer Name", Narrow(computer)));
    win.cards.push_back(std::move(os));

    Card ids{ "Identifiers", "Registry" };
    ids.fields.push_back(MakeField("Machine GUID", RegReadString(L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid")));
    ids.fields.push_back(MakeField("Product ID", RegReadString(cv, L"ProductId")));
    HW_PROFILE_INFOW hwp{};
    if (GetCurrentHwProfileW(&hwp)) ids.fields.push_back(MakeField("HW Profile GUID", Narrow(hwp.szHwProfileGuid)));
    ids.fields.push_back(MakeField("SQM Machine ID", RegReadString(L"SOFTWARE\\Microsoft\\SQMClient", L"MachineId")));
    ids.fields.push_back(MakeField("WU SusClientId",
                                   RegReadString(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate", L"SusClientId")));
    win.cards.push_back(std::move(ids));

    out.push_back(std::move(win));
}

} // namespace hw
