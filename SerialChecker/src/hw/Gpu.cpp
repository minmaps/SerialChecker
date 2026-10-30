// Graphics adapters (DXGI) and their UUID, read through NVML (NVIDIA driver) and Vulkan (any vendor).
// Both libraries are loaded at runtime, so no SDK is needed and a missing driver only hides that field.
#include "hw/Util.h"

#include <windows.h>
#include <dxgi.h>
#include <cstring>
#include <memory>

namespace hw {
namespace {

constexpr UINT kVendorNvidia = 0x10DE;

std::string UuidString(const uint8_t* b, bool nvidia)
{
    // NVML prints the same 16 bytes as "GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
    std::string s = Format("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
                           b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return nvidia ? "GPU-" + s : s;
}

// ---- Vulkan: VkPhysicalDeviceIDProperties gives deviceUUID and the LUID that DXGI reports ----

using VkInstance = struct VkInstance_T*;
using VkPhysicalDevice = struct VkPhysicalDevice_T*;
using PFN_vkVoidFunction = void(__stdcall*)();

struct VkApplicationInfo {
    uint32_t sType;  // VK_STRUCTURE_TYPE_APPLICATION_INFO = 0
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};

struct VkInstanceCreateInfo {
    uint32_t sType;  // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
    const void* pNext;
    uint32_t flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};

struct VkPhysicalDeviceIDProperties {
    uint32_t sType;  // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES = 1000071004
    void* pNext;
    uint8_t deviceUUID[16];
    uint8_t driverUUID[16];
    uint8_t deviceLUID[8];
    uint32_t deviceNodeMask;
    uint32_t deviceLUIDValid;
};

struct VkPhysicalDeviceProperties2 {
    uint32_t sType;  // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 = 1000059001
    void* pNext;
    alignas(8) uint8_t properties[4096];  // VkPhysicalDeviceProperties (~830 bytes), not needed here
};

using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(__stdcall*)(VkInstance, const char*);
using PFN_vkCreateInstance = int32_t(__stdcall*)(const VkInstanceCreateInfo*, const void*, VkInstance*);
using PFN_vkDestroyInstance = void(__stdcall*)(VkInstance, const void*);
using PFN_vkEnumeratePhysicalDevices = int32_t(__stdcall*)(VkInstance, uint32_t*, VkPhysicalDevice*);
using PFN_vkGetPhysicalDeviceProperties2 = void(__stdcall*)(VkPhysicalDevice, VkPhysicalDeviceProperties2*);

struct VulkanGpu {
    LUID luid;
    uint8_t uuid[16];
};

std::vector<VulkanGpu> VulkanDevices()
{
    std::vector<VulkanGpu> result;
    HMODULE lib = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return result;
    auto getProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
    auto create = getProc ? (PFN_vkCreateInstance)getProc(nullptr, "vkCreateInstance") : nullptr;
    VkInstance instance = nullptr;
    VkApplicationInfo app{ 0, nullptr, "SerialChecker", 1, nullptr, 0, (1u << 22) | (1u << 12) };  // Vulkan 1.1
    VkInstanceCreateInfo info{ 1, nullptr, 0, &app };
    if (create && create(&info, nullptr, &instance) == 0) {
        auto destroy = (PFN_vkDestroyInstance)getProc(instance, "vkDestroyInstance");
        auto enumerate = (PFN_vkEnumeratePhysicalDevices)getProc(instance, "vkEnumeratePhysicalDevices");
        auto props2 = (PFN_vkGetPhysicalDeviceProperties2)getProc(instance, "vkGetPhysicalDeviceProperties2");
        uint32_t count = 0;
        if (enumerate && props2 && enumerate(instance, &count, nullptr) == 0 && count) {
            std::vector<VkPhysicalDevice> devices(count);
            if (enumerate(instance, &count, devices.data()) >= 0) {
                for (uint32_t i = 0; i < count; ++i) {
                    VkPhysicalDeviceIDProperties id{ 1000071004 };
                    auto props = std::make_unique<VkPhysicalDeviceProperties2>();
                    props->sType = 1000059001;
                    props->pNext = &id;
                    props2(devices[i], props.get());
                    if (!id.deviceLUIDValid) continue;
                    VulkanGpu g{};
                    memcpy(&g.luid, id.deviceLUID, sizeof(g.luid));
                    memcpy(g.uuid, id.deviceUUID, sizeof(g.uuid));
                    result.push_back(g);
                }
            }
        }
        if (destroy) destroy(instance, nullptr);
    }
    FreeLibrary(lib);
    return result;
}

// ---- NVML ----

struct NvmlPciInfo {  // nvmlPciInfo_t (v3)
    char busIdLegacy[16];
    unsigned domain, bus, device;
    unsigned pciDeviceId;     // device << 16 | vendor
    unsigned pciSubSystemId;
    char busId[32];
};

struct NvmlGpu {
    unsigned pciDeviceId, pciSubSystemId;
    std::string uuid;
    bool used = false;
};

std::vector<NvmlGpu> NvmlDevices()
{
    std::vector<NvmlGpu> result;
    HMODULE lib = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib)
        lib = LoadLibraryExW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll", nullptr,
                             LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lib) return result;

    using Device = void*;
    auto init = (int(__cdecl*)())GetProcAddress(lib, "nvmlInit_v2");
    auto shutdown = (int(__cdecl*)())GetProcAddress(lib, "nvmlShutdown");
    auto count = (int(__cdecl*)(unsigned*))GetProcAddress(lib, "nvmlDeviceGetCount_v2");
    auto handle = (int(__cdecl*)(unsigned, Device*))GetProcAddress(lib, "nvmlDeviceGetHandleByIndex_v2");
    auto uuid = (int(__cdecl*)(Device, char*, unsigned))GetProcAddress(lib, "nvmlDeviceGetUUID");
    auto pci = (int(__cdecl*)(Device, NvmlPciInfo*))GetProcAddress(lib, "nvmlDeviceGetPciInfo_v3");
    if (init && shutdown && count && handle && uuid && pci && init() == 0) {
        unsigned n = 0;
        if (count(&n) == 0) {
            for (unsigned i = 0; i < n; ++i) {
                Device d = nullptr;
                char buf[96]{};
                NvmlPciInfo info{};
                if (handle(i, &d) != 0 || uuid(d, buf, sizeof(buf)) != 0 || pci(d, &info) != 0) continue;
                result.push_back({ info.pciDeviceId, info.pciSubSystemId, buf });
            }
        }
        shutdown();
    }
    FreeLibrary(lib);
    return result;
}

} // namespace

void CollectGraphics(std::vector<Section>& out)
{
    Section gpu{ "Graphics" };
    std::vector<VulkanGpu> vulkan = VulkanDevices();
    std::vector<NvmlGpu> nvml = NvmlDevices();

    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            adapter->Release();
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            bool nvidia = d.VendorId == kVendorNvidia;
            Card c{ Narrow(d.Description), "DXGI" };
            c.fields.push_back(MakeField("Vendor / Device", Format("VEN_%04X  DEV_%04X", d.VendorId, d.DeviceId)));
            c.fields.push_back(MakeField("Subsystem", Format("SUBSYS_%08X  REV_%02X", d.SubSysId, d.Revision)));

            // NVML has no LUID: match on PCI IDs, in enumeration order when identical cards are installed.
            if (nvidia) {
                Field f = Unsupported("UUID [NVML]", "Not available");
                for (auto& g : nvml)
                    if (!g.used && g.pciDeviceId == (d.DeviceId << 16 | d.VendorId) && g.pciSubSystemId == d.SubSysId) {
                        g.used = true;
                        f = MakeField("UUID [NVML]", g.uuid);
                        break;
                    }
                c.fields.push_back(std::move(f));
            }
            Field vk = Unsupported("UUID [Vulkan]", "Not available");
            for (const auto& g : vulkan)
                if (g.luid.LowPart == d.AdapterLuid.LowPart && g.luid.HighPart == d.AdapterLuid.HighPart)
                    vk = MakeField("UUID [Vulkan]", UuidString(g.uuid, nvidia));
            c.fields.push_back(std::move(vk));

            c.fields.push_back(MakeField("Dedicated VRAM", Format("%llu MB", (unsigned long long)d.DedicatedVideoMemory >> 20)));
            c.fields.push_back(MakeField("Adapter LUID", Format("%08lX-%08lX", d.AdapterLuid.HighPart, d.AdapterLuid.LowPart)));
            gpu.cards.push_back(std::move(c));
        }
        factory->Release();
    }
    if (!gpu.cards.empty()) out.push_back(std::move(gpu));
}

} // namespace hw
