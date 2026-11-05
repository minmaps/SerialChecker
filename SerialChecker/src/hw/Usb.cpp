// USB devices: serial number from the device instance ID (what Windows, WMI and most software read) and straight
// from the device's string descriptor, requested through its parent hub the way USBView does.
#include "hw/Util.h"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <winioctl.h>  // CTL_CODE, needed by usbioctl.h under WIN32_LEAN_AND_MEAN
#include <usbioctl.h>
#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>

namespace hw {
namespace {

// GUID_DEVINTERFACE_USB_HUB {F18A0E88-C30C-11D0-8815-00A0C906BED8}
const GUID kHubInterface = { 0xf18a0e88, 0xc30c, 0x11d0, { 0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8 } };
// DEVPKEY_Device_BusReportedDeviceDesc: the product string the device reported when it was enumerated.
const DEVPROPKEY kBusReportedDesc = { { 0x540b947e, 0x8b40, 0x45bc, { 0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2 } }, 4 };

constexpr DWORD kHubTimeoutMs = 3000;

// Hub interface handles, opened once and shared by every device plugged into that hub.
class Hubs {
public:
    ~Hubs()
    {
        for (auto& [path, hub] : open_)
            if (hub->handle != INVALID_HANDLE_VALUE) CloseHandle(hub->handle);
    }

    // The hub the device hangs off, or nullptr when its interface cannot be found or opened (error in `error`).
    TimedDevice* ParentOf(DEVINST device, DWORD& error)
    {
        std::wstring path = HubPath(device);
        if (path.empty()) {
            error = ERROR_NOT_FOUND;
            return nullptr;
        }
        auto it = open_.find(path);
        if (it == open_.end()) {
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OVERLAPPED, nullptr);
            auto hub = std::make_unique<Hub>(h, h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS);
            it = open_.emplace(path, std::move(hub)).first;
        }
        error = it->second->error;
        return it->second->handle == INVALID_HANDLE_VALUE ? nullptr : &it->second->device;
    }

private:
    struct Hub {
        Hub(HANDLE h, DWORD e) : handle(h), error(e), device(h, kHubTimeoutMs) {}
        HANDLE handle;
        DWORD error;
        TimedDevice device;
    };
    std::map<std::wstring, std::unique_ptr<Hub>> open_;

    static std::wstring HubPath(DEVINST device)
    {
        DEVINST parent;
        wchar_t id[MAX_DEVICE_ID_LEN];
        if (CM_Get_Parent(&parent, device, 0) != CR_SUCCESS ||
            CM_Get_Device_IDW(parent, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
            return {};
        ULONG len = 0;
        GUID guid = kHubInterface;
        if (CM_Get_Device_Interface_List_SizeW(&len, &guid, id, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
            len <= 1)
            return {};
        std::vector<wchar_t> list(len);
        if (CM_Get_Device_Interface_ListW(&guid, id, list.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
            return {};
        return list.data();  // first (and only) interface of the hub
    }
};

bool DeviceDescriptor(TimedDevice& hub, ULONG port, USB_DEVICE_DESCRIPTOR& out)
{
    std::vector<BYTE> buf(sizeof(USB_NODE_CONNECTION_INFORMATION_EX) + 32 * sizeof(USB_PIPE_INFO));
    auto* info = (USB_NODE_CONNECTION_INFORMATION_EX*)buf.data();
    info->ConnectionIndex = port;
    DWORD ret = 0;
    if (!hub.Ioctl(IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, buf.data(), (DWORD)buf.size(), buf.data(),
                   (DWORD)buf.size(), &ret) ||
        ret < offsetof(USB_NODE_CONNECTION_INFORMATION_EX, CurrentConfigurationValue) ||
        info->ConnectionStatus != DeviceConnected)
        return false;
    out = info->DeviceDescriptor;
    return true;
}

// Body of string descriptor `index` (UTF-16, or the LANGID list for index 0). Empty on failure.
std::wstring StringDescriptor(TimedDevice& hub, ULONG port, UCHAR index, USHORT lang)
{
    constexpr size_t kHeader = offsetof(USB_DESCRIPTOR_REQUEST, Data);
    std::vector<BYTE> buf(kHeader + MAXIMUM_USB_STRING_LENGTH);
    auto* req = (USB_DESCRIPTOR_REQUEST*)buf.data();
    req->ConnectionIndex = port;
    req->SetupPacket.wValue = (USHORT)(USB_STRING_DESCRIPTOR_TYPE << 8 | index);
    req->SetupPacket.wIndex = lang;
    req->SetupPacket.wLength = MAXIMUM_USB_STRING_LENGTH;
    DWORD ret = 0;
    if (!hub.Ioctl(IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, buf.data(), (DWORD)buf.size(), buf.data(),
                   (DWORD)buf.size(), &ret) ||
        ret < kHeader + 2)
        return {};
    auto* s = (const USB_STRING_DESCRIPTOR*)(buf.data() + kHeader);
    if (s->bDescriptorType != USB_STRING_DESCRIPTOR_TYPE) return {};
    size_t len = std::min<size_t>(s->bLength, ret - kHeader);
    if (len < 2) return {};
    return std::wstring(s->bString, (len - 2) / sizeof(wchar_t));
}

std::string RegistryString(HDEVINFO devs, SP_DEVINFO_DATA& dev, DWORD prop)
{
    wchar_t buf[512]{};
    if (!SetupDiGetDeviceRegistryPropertyW(devs, &dev, prop, nullptr, (BYTE*)buf, sizeof(buf) - sizeof(wchar_t), nullptr))
        return {};
    return Trim(Narrow(buf));
}

// USBHUB3 reports hubs as USB\USB20_HUB / USB\USB30_HUB, the older stack as USB\Class_09.
bool IsHub(HDEVINFO devs, SP_DEVINFO_DATA& dev)
{
    wchar_t buf[2048]{};
    if (!SetupDiGetDeviceRegistryPropertyW(devs, &dev, SPDRP_COMPATIBLEIDS, nullptr, (BYTE*)buf,
                                           sizeof(buf) - 2 * sizeof(wchar_t), nullptr))
        return false;
    for (const wchar_t* s = buf; *s; s += wcslen(s) + 1)
        if (_wcsicmp(s, L"USB\\USB20_HUB") == 0 || _wcsicmp(s, L"USB\\USB30_HUB") == 0 || _wcsicmp(s, L"USB\\Class_09") == 0)
            return true;
    return false;
}

} // namespace

void CollectUsb(std::vector<Section>& out)
{
    Section section{ "USB" };
    Hubs hubs;
    HDEVINFO devs = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devs != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dev{ sizeof(dev) };
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev); ++i) {
            wchar_t idBuf[MAX_DEVICE_ID_LEN];
            if (!SetupDiGetDeviceInstanceIdW(devs, &dev, idBuf, _countof(idBuf), nullptr)) continue;
            // "USB\VID_046D&PID_C54D\3859364F3435": keep physical devices only. Interfaces of composite devices
            // (&MI_xx), other virtual children (&LAMPARRAY) and root hubs have a different middle part.
            std::string id = Narrow(idBuf);
            size_t a = id.find('\\'), b = id.rfind('\\');
            if (a == std::string::npos || b == a) continue;
            std::string hw = id.substr(a + 1, b - a - 1), tail = id.substr(b + 1);
            if (hw.size() != 17 || hw.compare(0, 4, "VID_") != 0 || hw.compare(8, 5, "&PID_") != 0) continue;

            // Windows puts the serial in the instance ID only when the device reports a usable one; otherwise the
            // last part is a port-based ID like "7&108D691B&0&3".
            DWORD caps = 0;
            SetupDiGetDeviceRegistryPropertyW(devs, &dev, SPDRP_CAPABILITIES, nullptr, (BYTE*)&caps, sizeof(caps), nullptr);
            bool idHasSerial = (caps & CM_DEVCAP_UNIQUEID) || tail.find('&') == std::string::npos;

            std::string title;
            DEVPROPTYPE type = 0;
            wchar_t desc[256]{};
            if (SetupDiGetDevicePropertyW(devs, &dev, &kBusReportedDesc, &type, (BYTE*)desc,
                                          sizeof(desc) - sizeof(wchar_t), nullptr, 0) &&
                type == DEVPROP_TYPE_STRING)
                title = Trim(Narrow(desc));

            Field descSerial = Unsupported("Serial [Descriptor]");
            std::string manufacturer;
            DWORD port = 0, error = 0;
            TimedDevice* hub = hubs.ParentOf(dev.DevInst, error);
            USB_DEVICE_DESCRIPTOR dd{};
            if (!hub) {
                if (error == ERROR_ACCESS_DENIED && !IsProcessElevated()) descSerial.value = "Requires administrator";
            } else if (SetupDiGetDeviceRegistryPropertyW(devs, &dev, SPDRP_ADDRESS, nullptr, (BYTE*)&port, sizeof(port),
                                                         nullptr) &&
                       DeviceDescriptor(*hub, port, dd)) {
                std::wstring langs = StringDescriptor(*hub, port, 0, 0);
                USHORT lang = langs.empty() ? 0x0409 : (USHORT)langs[0];
                if (dd.iSerialNumber) {
                    std::wstring s = StringDescriptor(*hub, port, dd.iSerialNumber, lang);
                    if (!s.empty()) descSerial = MakeField(descSerial.label, Narrow(s));
                } else {
                    descSerial.value = "No serial";
                }
                if (dd.iManufacturer) manufacturer = Trim(Narrow(StringDescriptor(*hub, port, dd.iManufacturer, lang)));
                if (title.empty() && dd.iProduct) title = Trim(Narrow(StringDescriptor(*hub, port, dd.iProduct, lang)));
            }
            if (hub && hub->hung && descSerial.placeholder) descSerial.value = "Not responding";
            // Hubs built into the motherboard, a monitor or a case carry no serial: they would only add noise.
            bool isHub = dd.bDeviceClass == USB_DEVICE_CLASS_HUB || IsHub(devs, dev);
            if (isHub && !idHasSerial && descSerial.placeholder) continue;
            if (title.empty()) title = RegistryString(devs, dev, SPDRP_DEVICEDESC);

            Card c{ title.empty() ? "USB Device" : title, "USB" };
            c.fields.push_back(MakeField("VID / PID", hw.substr(4, 4) + ":" + hw.substr(13, 4)));
            if (!manufacturer.empty()) c.fields.push_back(MakeField("Manufacturer", manufacturer));
            c.fields.push_back(idHasSerial ? MakeField("Serial [Instance ID]", tail)
                                           : Field{ "Serial [Instance ID]", "No serial", true });
            c.fields.push_back(std::move(descSerial));
            section.cards.push_back(std::move(c));
        }
        SetupDiDestroyDeviceInfoList(devs);
    }
    std::stable_sort(section.cards.begin(), section.cards.end(),
                     [](const Card& x, const Card& y) { return _stricmp(x.title.c_str(), y.title.c_str()) < 0; });
    if (section.cards.empty()) section.cards.push_back({ "USB", "SetupAPI", { Unsupported("Status", "No device found") } });
    out.push_back(std::move(section));
}

} // namespace hw
