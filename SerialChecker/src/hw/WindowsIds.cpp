// Windows-side identifiers (registry / hardware profile).
#include "hw/Util.h"

#include <windows.h>
#include <ntsecapi.h>
#include <sddl.h>
#include <algorithm>
#include <cctype>
#include <ctime>

namespace hw {
namespace {

// "{b8fd5bed-...}" -> "B8FD5BED-...": same shape as the SMBIOS UUID, so both can be compared by eye.
std::string BareGuid(std::string s)
{
    if (s.size() > 2 && s.front() == '{' && s.back() == '}') s = s.substr(1, s.size() - 2);
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// SID of the local account domain (S-1-5-21-...): generated at install, reset only by Sysprep.
std::string MachineSid()
{
    LSA_OBJECT_ATTRIBUTES attrs{};
    LSA_HANDLE policy = nullptr;
    if (LsaOpenPolicy(nullptr, &attrs, POLICY_VIEW_LOCAL_INFORMATION, &policy) != 0) return {};
    std::string sid;
    POLICY_ACCOUNT_DOMAIN_INFO* info = nullptr;
    if (LsaQueryInformationPolicy(policy, PolicyAccountDomainInformation, (void**)&info) == 0 && info) {
        LPWSTR str = nullptr;
        if (info->DomainSid && ConvertSidToStringSidW(info->DomainSid, &str)) {
            sid = Narrow(str);
            LocalFree(str);
        }
        LsaFreeMemory(info);
    }
    LsaClose(policy);
    return sid;
}

// Hardware IDs Windows derives from the SMBIOS strings at boot (what ComputerHardwareIds.exe prints).
Card ComputerHardwareIds()
{
    const wchar_t* key = L"SYSTEM\\CurrentControlSet\\Control\\SystemInformation";
    Card c{ "Computer Hardware IDs", "Registry" };
    c.fields.push_back(MakeField("Primary CHID", BareGuid(RegReadString(key, L"ComputerHardwareId"))));
    std::vector<std::string> ids;
    for (auto& id : RegReadMultiString(key, L"ComputerHardwareIds"))
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);  // the list repeats some hashes
    for (size_t i = 0; i < ids.size(); ++i) c.fields.push_back(MakeField(Format("CHID %zu", i + 1), BareGuid(ids[i])));
    return c;
}

// HKLM\SYSTEM\HardwareConfig keeps one subkey per system UUID this install has booted on.
Card HardwareConfigHistory()
{
    const wchar_t* path = L"SYSTEM\\HardwareConfig";
    Card c{ "Hardware Config History", "Registry" };
    c.fields.push_back(MakeField("Current Config", BareGuid(RegReadString(path, L"LastConfig"))));

    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) return c;
    struct Entry {
        DWORD id;
        std::string guid, board;
    };
    auto readString = [&](const wchar_t* sub, const wchar_t* value) {
        wchar_t buf[256]{};
        DWORD size = sizeof(buf);
        return RegGetValueW(root, sub, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS ? Trim(Narrow(buf))
                                                                                                   : std::string();
    };
    std::vector<Entry> entries;
    wchar_t name[128];
    for (DWORD i = 0;; ++i) {
        DWORD len = _countof(name);
        LONG r = RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || name[0] != L'{') continue;  // skips "Current", a copy of the last config
        Entry e{ 0, BareGuid(Narrow(name)) };
        DWORD size = sizeof(e.id);
        RegGetValueW(root, name, L"Id", RRF_RT_REG_DWORD, nullptr, &e.id, &size);
        // Board + BIOS tell the configs apart: two UUIDs on the same board and BIOS mean the UUID itself changed.
        e.board = readString(name, L"BaseBoardProduct");
        if (std::string bios = readString(name, L"BIOSVersion"); !bios.empty())
            e.board += (e.board.empty() ? "BIOS " : ", BIOS ") + bios;
        entries.push_back(std::move(e));
    }
    RegCloseKey(root);

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
    for (const auto& e : entries) {
        c.fields.push_back(MakeField(Format("Config %lu", e.id), e.guid));
        if (!e.board.empty()) c.fields.push_back(MakeField(Format("Config %lu Board", e.id), e.board));
    }
    return c;
}

} // namespace

void CollectWindows(std::vector<Section>& out)
{
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
    ids.fields.push_back(MakeField("Machine SID", MachineSid()));
    win.cards.push_back(std::move(ids));
    win.cards.push_back(ComputerHardwareIds());
    win.cards.push_back(HardwareConfigHistory());

    out.push_back(std::move(win));
}

} // namespace hw
