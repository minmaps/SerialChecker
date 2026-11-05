// Physical drives queried through several storage paths (same set as the classic serial checkers),
// plus volume serial numbers.
#include "hw/Util.h"

#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <algorithm>
#include <cstddef>
#include <memory>

namespace hw {
namespace {

struct HandleCloser {
    void operator()(HANDLE h) const { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

// Every IOCTL goes through TimedDevice: a drive that stops answering is skipped instead of blocking the collection.
using Drive = TimedDevice;
constexpr DWORD kDriveTimeoutMs = 10000;  // generous: a sleeping HDD has to spin up first

// ATA IDENTIFY strings are stored as big-endian 16-bit words.
std::string AtaString(const BYTE* identify, int firstWord, int lastWord)
{
    std::string s;
    for (int w = firstWord; w <= lastWord; ++w) {
        s += (char)identify[w * 2 + 1];
        s += (char)identify[w * 2];
    }
    return Trim(s);
}

const char* BusTypeName(STORAGE_BUS_TYPE bus)
{
    switch (bus) {
    case BusTypeScsi: return "SCSI";
    case BusTypeAtapi: return "ATAPI";
    case BusTypeAta: return "ATA";
    case BusTypeUsb: return "USB";
    case BusTypeRAID: return "RAID";
    case BusTypeiScsi: return "iSCSI";
    case BusTypeSas: return "SAS";
    case BusTypeSata: return "SATA";
    case BusTypeSd: return "SD";
    case BusTypeMmc: return "MMC";
    case BusTypeVirtual: return "Virtual";
    case BusTypeFileBackedVirtual: return "VHD";
    case BusTypeSpaces: return "Spaces";
    case BusTypeNvme: return "NVMe";
    default: return "Disk";
    }
}

std::string OffsetString(const std::vector<BYTE>& buf, DWORD offset)
{
    if (offset == 0 || offset >= buf.size()) return {};
    return Trim(std::string((const char*)buf.data() + offset, strnlen((const char*)buf.data() + offset, buf.size() - offset)));
}

struct DeviceInfo {
    bool ok = false;
    std::string vendor, product, revision, serial;
    STORAGE_BUS_TYPE bus = BusTypeUnknown;
};

DeviceInfo QueryDeviceProperty(Drive& drive)
{
    DeviceInfo info;
    STORAGE_PROPERTY_QUERY q{ StorageDeviceProperty, PropertyStandardQuery };
    std::vector<BYTE> buf(4096);
    DWORD ret = 0;
    if (!drive.Ioctl(IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf.data(), (DWORD)buf.size(), &ret))
        return info;
    auto* d = (const STORAGE_DEVICE_DESCRIPTOR*)buf.data();
    info.ok = true;
    info.vendor = OffsetString(buf, d->VendorIdOffset);
    info.product = OffsetString(buf, d->ProductIdOffset);
    info.revision = OffsetString(buf, d->ProductRevisionOffset);
    info.serial = OffsetString(buf, d->SerialNumberOffset);
    info.bus = d->BusType;
    return info;
}

// Picks the most meaningful unique identifier (NAA > EUI-64 > SCSI name string) and dumps it as hex.
Field QueryWwn(Drive& drive)
{
    STORAGE_PROPERTY_QUERY q{ StorageDeviceIdProperty, PropertyStandardQuery };
    std::vector<BYTE> buf(4096);
    DWORD ret = 0;
    if (!drive.Ioctl(IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf.data(), (DWORD)buf.size(), &ret))
        return Unsupported("STORAGE_QUERY_WWN");

    auto* desc = (const STORAGE_DEVICE_ID_DESCRIPTOR*)buf.data();
    const STORAGE_IDENTIFIER* best = nullptr;
    int bestRank = 0;
    const BYTE* p = desc->Identifiers;
    for (ULONG i = 0; i < desc->NumberOfIdentifiers; ++i) {
        if (p + sizeof(STORAGE_IDENTIFIER) > buf.data() + ret) break;
        auto* id = (const STORAGE_IDENTIFIER*)p;
        int rank = 0;
        if (id->Association == StorageIdAssocDevice) {
            switch (id->Type) {
            case StorageIdTypeFCPHName: rank = 4; break;     // NAA (SATA/SAS WWN)
            case StorageIdTypeEUI64: rank = 3; break;
            case StorageIdTypeScsiNameString: rank = 2; break; // NVMe "eui.xxxx"
            case StorageIdTypeVendorSpecific: break;
            default: rank = 1; break;
            }
        }
        if (rank > bestRank) { bestRank = rank; best = id; }
        if (id->NextOffset == 0) break;
        p += id->NextOffset;
    }
    if (!best) return Unsupported("STORAGE_QUERY_WWN", "No identifier");

    size_t len = best->IdentifierSize;
    if (best->CodeSet == StorageIdCodeSetAscii || best->CodeSet == StorageIdCodeSetUtf8)
        while (len && best->Identifier[len - 1] == 0) --len;
    return MakeField("STORAGE_QUERY_WWN", HexBytes(best->Identifier, len));
}

Field QuerySmart(Drive& drive)
{
    SENDCMDINPARAMS in{};
    in.cBufferSize = IDENTIFY_BUFFER_SIZE;
    in.irDriveRegs.bSectorCountReg = 1;
    in.irDriveRegs.bSectorNumberReg = 1;
    in.irDriveRegs.bDriveHeadReg = 0xA0;
    in.irDriveRegs.bCommandReg = ID_CMD;
    std::vector<BYTE> out(sizeof(SENDCMDOUTPARAMS) + IDENTIFY_BUFFER_SIZE);
    DWORD ret = 0;
    if (!drive.Ioctl(SMART_RCV_DRIVE_DATA, &in, sizeof(SENDCMDINPARAMS) - 1, out.data(), (DWORD)out.size(), &ret))
        return Unsupported("SMART_RCV_DRIVE_DATA");
    auto* res = (const SENDCMDOUTPARAMS*)out.data();
    return MakeField("SMART_RCV_DRIVE_DATA", AtaString(res->bBuffer, 10, 19));
}

Field QueryScsiPassThrough(Drive& drive)
{
    struct Buffer {
        SCSI_PASS_THROUGH spt;
        ULONG filler;
        UCHAR sense[32];
        UCHAR data[252];
    } b{};
    b.spt.Length = sizeof(SCSI_PASS_THROUGH);
    b.spt.CdbLength = 6;
    b.spt.SenseInfoLength = sizeof(b.sense);
    b.spt.DataIn = SCSI_IOCTL_DATA_IN;
    b.spt.DataTransferLength = sizeof(b.data);
    b.spt.TimeOutValue = 3;
    b.spt.DataBufferOffset = offsetof(Buffer, data);
    b.spt.SenseInfoOffset = offsetof(Buffer, sense);
    b.spt.Cdb[0] = 0x12;               // INQUIRY
    b.spt.Cdb[1] = 0x01;               // EVPD
    b.spt.Cdb[2] = 0x80;               // Unit Serial Number page
    b.spt.Cdb[4] = sizeof(b.data);
    DWORD ret = 0;
    if (!drive.Ioctl(IOCTL_SCSI_PASS_THROUGH, &b, sizeof(b), &b, sizeof(b), &ret) || b.spt.ScsiStatus != 0)
        return Unsupported("SCSI_PASS_THROUGH");
    if (b.data[1] != 0x80) return Unsupported("SCSI_PASS_THROUGH", "Invalid page");
    size_t len = std::min<size_t>(b.data[3], sizeof(b.data) - 4);
    return MakeField("SCSI_PASS_THROUGH", std::string((const char*)b.data + 4, len));
}

Field QueryAtaPassThrough(Drive& drive)
{
    struct Buffer {
        ATA_PASS_THROUGH_EX apt;
        ULONG filler;
        UCHAR data[512];
    } b{};
    b.apt.Length = sizeof(ATA_PASS_THROUGH_EX);
    b.apt.AtaFlags = ATA_FLAGS_DATA_IN | ATA_FLAGS_DRDY_REQUIRED;
    b.apt.DataTransferLength = sizeof(b.data);
    b.apt.TimeOutValue = 3;
    b.apt.DataBufferOffset = offsetof(Buffer, data);
    b.apt.CurrentTaskFile[6] = 0xEC;   // IDENTIFY DEVICE
    DWORD ret = 0;
    if (!drive.Ioctl(IOCTL_ATA_PASS_THROUGH, &b, sizeof(b), &b, sizeof(b), &ret))
        return Unsupported("ATA_PASS_THROUGH");
    return MakeField("ATA_PASS_THROUGH", AtaString(b.data, 10, 19));
}

Field QueryNvmeIdentify(Drive& drive)
{
    constexpr DWORD kIdentifySize = 4096;
    const size_t len = offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) +
                       kIdentifySize;
    std::vector<BYTE> buf(len);
    auto* q = (STORAGE_PROPERTY_QUERY*)buf.data();
    q->PropertyId = StorageAdapterProtocolSpecificProperty;
    q->QueryType = PropertyStandardQuery;
    auto* proto = (STORAGE_PROTOCOL_SPECIFIC_DATA*)q->AdditionalParameters;
    proto->ProtocolType = ProtocolTypeNvme;
    proto->DataType = NVMeDataTypeIdentify;
    proto->ProtocolDataRequestValue = 1;  // CNS 01h: Identify Controller
    proto->ProtocolDataRequestSubValue = 0;
    proto->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    proto->ProtocolDataLength = kIdentifySize;

    DWORD ret = 0;
    if (!drive.Ioctl(IOCTL_STORAGE_QUERY_PROPERTY, buf.data(), (DWORD)len, buf.data(), (DWORD)len, &ret))
        return Unsupported("NVME_IDENTIFY");
    auto* desc = (const STORAGE_PROTOCOL_DATA_DESCRIPTOR*)buf.data();
    const auto* res = &desc->ProtocolSpecificData;
    if (res->ProtocolDataOffset < sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) || res->ProtocolDataLength < 24)
        return Unsupported("NVME_IDENTIFY", "Invalid data");
    const BYTE* id = (const BYTE*)res + res->ProtocolDataOffset;
    return MakeField("NVME_IDENTIFY", std::string((const char*)id + 4, 20));  // SN: bytes 4..23
}

std::string GuidString(const GUID& g)
{
    return Format("%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                  g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

// Names the partition types a Windows install creates; others stay unnamed.
const char* GptTypeName(const GUID& type)
{
    static const struct {
        GUID guid;
        const char* name;
    } kTypes[] = {
        { { 0xC12A7328, 0xF81F, 0x11D2, { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } }, "EFI" },
        { { 0xE3C9E316, 0x0B5C, 0x4DB8, { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE } }, "MSR" },
        { { 0xEBD0A0A2, 0xB9E5, 0x4433, { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 } }, "Basic data" },
        { { 0xDE94BBA4, 0x06D1, 0x4D40, { 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC } }, "Recovery" },
    };
    for (const auto& t : kTypes)
        if (memcmp(&t.guid, &type, sizeof(GUID)) == 0) return t.name;
    return nullptr;
}

// Partition table identifiers: GPT disk GUID plus one GUID per partition, or the 32-bit MBR disk signature.
std::vector<Field> QueryLayout(Drive& drive)
{
    constexpr size_t kHeader = offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry);
    std::vector<BYTE> buf(kHeader + 128 * sizeof(PARTITION_INFORMATION_EX));  // 128 = default GPT entry count
    DWORD ret = 0;
    if (!drive.Ioctl(IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buf.data(), (DWORD)buf.size(), &ret) || ret < kHeader)
        return { Unsupported("Disk ID") };

    auto* layout = (const DRIVE_LAYOUT_INFORMATION_EX*)buf.data();
    std::vector<Field> fields;
    switch (layout->PartitionStyle) {
    case PARTITION_STYLE_GPT: {
        fields.push_back(MakeField("GPT Disk GUID", GuidString(layout->Gpt.DiskId)));
        static const GUID kNone{};
        size_t count = std::min<size_t>(layout->PartitionCount, (ret - kHeader) / sizeof(PARTITION_INFORMATION_EX));
        for (size_t i = 0; i < count; ++i) {
            const auto& p = layout->PartitionEntry[i];
            if (p.PartitionStyle != PARTITION_STYLE_GPT || memcmp(&p.Gpt.PartitionType, &kNone, sizeof(GUID)) == 0)
                continue;
            std::string label = Format("Partition %lu", p.PartitionNumber);
            if (const char* type = GptTypeName(p.Gpt.PartitionType)) label += Format("  (%s)", type);
            fields.push_back(MakeField(label, GuidString(p.Gpt.PartitionId)));
        }
        break;
    }
    case PARTITION_STYLE_MBR:
        fields.push_back(MakeField("MBR Signature", Format("%08lX", layout->Mbr.Signature)));
        break;
    default:
        fields.push_back(Unsupported("Disk ID", "No partition table"));
        break;
    }
    return fields;
}

std::string Capacity(Drive& drive)
{
    GET_LENGTH_INFORMATION len{};
    DWORD ret = 0;
    if (!drive.Ioctl(IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len, sizeof(len), &ret)) return {};
    double gb = (double)len.Length.QuadPart / 1e9;
    return gb >= 1000 ? Format("%.2f TB", gb / 1000) : Format("%.0f GB", gb);
}

// NTFS and ReFS keep a 64-bit serial; GetVolumeInformation only returns its low 32 bits. FileIdInfo on the root
// directory reports the full value without the admin rights FSCTL_GET_NTFS_VOLUME_DATA needs on a volume handle.
Field VolumeSerial64(const std::string& name, const wchar_t* root)
{
    const std::string label = name + "  Serial (64-bit)";
    UniqueHandle h(CreateFileW(root, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    FILE_ID_INFO info{};
    if (h.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(h.get(), FileIdInfo, &info, sizeof(info)))
        return Unsupported(label);
    return MakeField(label, Format("%016llX", info.VolumeSerialNumber));
}

void CollectVolumes(std::vector<Section>& out)
{
    Card card{ "Volume Serials", "FS" };
    wchar_t drives[512];
    DWORD n = GetLogicalDriveStringsW(_countof(drives) - 1, drives);
    for (const wchar_t* d = drives; n && *d; d += wcslen(d) + 1) {
        UINT type = GetDriveTypeW(d);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        wchar_t label[MAX_PATH + 1]{}, fs[MAX_PATH + 1]{};
        DWORD serial = 0;
        if (!GetVolumeInformationW(d, label, MAX_PATH, &serial, nullptr, nullptr, fs, MAX_PATH)) continue;
        std::string name = Narrow(std::wstring(d, 2));  // "C:"
        std::string extra = Narrow(label);
        extra = extra.empty() ? Narrow(fs) : extra + ", " + Narrow(fs);
        card.fields.push_back(MakeField(name + "  (" + extra + ")", Format("%04X-%04X", serial >> 16, serial & 0xFFFF)));
        if (_wcsicmp(fs, L"NTFS") == 0 || _wcsicmp(fs, L"ReFS") == 0) card.fields.push_back(VolumeSerial64(name, d));

        wchar_t guid[64];
        if (GetVolumeNameForVolumeMountPointW(d, guid, _countof(guid))) {
            std::string g = Narrow(guid);
            auto a = g.find('{'), b = g.find('}');
            if (a != std::string::npos && b != std::string::npos)
                card.fields.push_back(MakeField(name + "  Volume GUID", g.substr(a + 1, b - a - 1)));
        }
    }
    if (card.fields.empty()) card.fields.push_back(Unsupported("Volumes", "No volume found"));
    out.push_back({ "Volumes", { std::move(card) } });
}

} // namespace

void CollectDisks(std::vector<Section>& out)
{
    Section section{ "Disks" };
    for (int i = 0; i < 32; ++i) {
        wchar_t path[64];
        swprintf_s(path, L"\\\\.\\PhysicalDrive%d", i);
        // Pass-through requires read/write access (admin); fall back to a query-only handle.
        UniqueHandle h(CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        bool fullAccess = h.get() != INVALID_HANDLE_VALUE;
        if (!fullAccess)
            h.reset(CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                                nullptr));
        if (h.get() == INVALID_HANDLE_VALUE) continue;
        Drive drive(h.get(), kDriveTimeoutMs);

        DeviceInfo info = QueryDeviceProperty(drive);
        std::string model = info.product;
        if (!info.vendor.empty() && info.vendor != "ATA" && info.vendor != "NVMe" && model.rfind(info.vendor, 0) != 0)
            model = info.vendor + " " + model;

        Card c{ Format("PhysicalDrive%d", i), BusTypeName(info.bus) };
        c.fields.push_back(MakeField("DISK_STORAGE_MODEL", model));
        c.fields.push_back(info.ok ? MakeField("STORAGE_QUERY_PROPERTY", info.serial)
                                   : Unsupported("STORAGE_QUERY_PROPERTY"));
        // Once a request timed out, every read that failed from then on is because the drive stopped answering.
        auto add = [&](Field f) {
            if (drive.hung && f.placeholder) f.value = "Not responding";
            c.fields.push_back(std::move(f));
        };
        const char* denied = "Requires administrator";
        add(fullAccess ? QuerySmart(drive) : Unsupported("SMART_RCV_DRIVE_DATA", denied));
        add(QueryWwn(drive));
        add(fullAccess ? QueryScsiPassThrough(drive) : Unsupported("SCSI_PASS_THROUGH", denied));
        add(fullAccess ? QueryAtaPassThrough(drive) : Unsupported("ATA_PASS_THROUGH", denied));
        if (info.bus == BusTypeNvme) add(QueryNvmeIdentify(drive));
        c.fields.push_back(MakeField("Firmware", info.revision));
        if (auto cap = Capacity(drive); !cap.empty()) c.fields.push_back(MakeField("Capacity", cap));
        for (auto& f : QueryLayout(drive)) add(std::move(f));
        section.cards.push_back(std::move(c));
    }
    if (section.cards.empty()) section.cards.push_back({ "Disks", "IOCTL", { Unsupported("Status", "No drive found") } });
    out.push_back(std::move(section));

    CollectVolumes(out);
}

} // namespace hw
