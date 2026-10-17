// Network adapters: current MAC (IP Helper), permanent MAC (NDIS OID) and registry NetworkAddress override.
#include "hw/Util.h"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

namespace hw {
namespace {

constexpr DWORD kIoctlNdisQueryGlobalStats = 0x00170002;  // CTL_CODE(FILE_DEVICE_PHYSICAL_NETCARD, 0, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
constexpr DWORD kOid8023PermanentAddress = 0x01010101;
constexpr DWORD kOid8023CurrentAddress = 0x01010102;
constexpr const wchar_t* kNetClassKey = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}";

bool QueryOid(const std::string& adapterGuid, DWORD oid, BYTE (&mac)[6])
{
    std::wstring path = L"\\\\.\\" + Widen(adapterGuid);
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, kIoctlNdisQueryGlobalStats, &oid, sizeof(oid), mac, sizeof(mac), &ret, nullptr);
    CloseHandle(h);
    return ok && ret == 6;
}

// Finds the adapter's instance key under the network class and reads NetworkAddress.
Field RegistryOverride(const std::string& adapterGuid)
{
    HKEY cls;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kNetClassKey, 0, KEY_READ | KEY_WOW64_64KEY, &cls) != ERROR_SUCCESS)
        return Unsupported("MAC [Registry]", "Access denied");
    std::wstring want = Widen(adapterGuid);
    Field result = Field{ "MAC [Registry]", "Not set", true };
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD nameLen = _countof(name);
        LONG r = RegEnumKeyExW(cls, i, name, &nameLen, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        wchar_t id[128]{};
        DWORD size = sizeof(id);
        if (RegGetValueW(cls, name, L"NetCfgInstanceId", RRF_RT_REG_SZ, nullptr, id, &size) != ERROR_SUCCESS) continue;
        if (_wcsicmp(id, want.c_str()) != 0) continue;
        wchar_t addr[64]{};
        size = sizeof(addr);
        if (RegGetValueW(cls, name, L"NetworkAddress", RRF_RT_REG_SZ, nullptr, addr, &size) == ERROR_SUCCESS && addr[0]) {
            std::string a = Narrow(addr);
            if (a.size() == 12) {
                std::string pretty;
                for (size_t k = 0; k < 12; k += 2) pretty += (k ? ":" : "") + a.substr(k, 2);
                a = pretty;
            }
            result = MakeField("MAC [Registry]", a);
        }
        break;
    }
    RegCloseKey(cls);
    return result;
}

} // namespace

void CollectNetwork(std::vector<Section>& out)
{
    Section section{ "Network" };
    ULONG size = 16 * 1024;
    std::vector<BYTE> buf(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }

    if (r == NO_ERROR) {
        for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
            if (a->PhysicalAddressLength != 6) continue;
            if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) continue;

            std::string guid = a->AdapterName;
            const char* kind = a->IfType == IF_TYPE_IEEE80211 ? "Wi-Fi" : "Ethernet";
            Card c{ Narrow(a->Description), kind };
            c.fields.push_back(MakeField("Name", Narrow(a->FriendlyName)));
            c.fields.push_back(MakeField("MAC [Current]", HexBytes(a->PhysicalAddress, 6)));

            BYTE mac[6];
            c.fields.push_back(QueryOid(guid, kOid8023PermanentAddress, mac) ? MakeField("MAC [Permanent]", HexBytes(mac, 6))
                                                                              : Unsupported("MAC [Permanent]"));
            c.fields.push_back(QueryOid(guid, kOid8023CurrentAddress, mac) ? MakeField("MAC [NDIS Current]", HexBytes(mac, 6))
                                                                            : Unsupported("MAC [NDIS Current]"));
            c.fields.push_back(RegistryOverride(guid));
            std::string g = guid;
            if (g.size() > 2 && g.front() == '{') g = g.substr(1, g.size() - 2);
            c.fields.push_back(MakeField("Interface GUID", g));
            c.fields.push_back(MakeField("Status", a->OperStatus == IfOperStatusUp ? "Connected" : "Disconnected"));
            section.cards.push_back(std::move(c));
        }
    }
    if (section.cards.empty()) section.cards.push_back({ "Network", "NDIS", { Unsupported("Status", "No adapter found") } });
    out.push_back(std::move(section));
}

} // namespace hw
