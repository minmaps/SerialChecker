// ACPI tables read through GetSystemFirmwareTable('ACPI'): OEM identifiers of the FADT and the MSDM product key.
#include "hw/Util.h"

#include <windows.h>
#include <algorithm>
#include <cstddef>

namespace hw {
namespace {

#pragma pack(push, 1)
struct AcpiHeader {
    char signature[4];
    DWORD length;
    BYTE revision;
    BYTE checksum;
    char oemId[6];
    char oemTableId[8];
    DWORD oemRevision;
    char creatorId[4];
    DWORD creatorRevision;
};
// Microsoft Data Management table: the OEM Windows product key burned into the firmware (OA 3.0).
struct Msdm {
    AcpiHeader header;
    DWORD version;
    DWORD reserved;
    DWORD dataType;
    DWORD dataReserved;
    DWORD dataLength;
    char data[29];
};
#pragma pack(pop)

// Table IDs are the 4 signature bytes read as a little-endian DWORD ("FACP" -> 'PCAF').
std::vector<BYTE> ReadTable(const char (&signature)[5])
{
    DWORD id;
    memcpy(&id, signature, 4);
    UINT size = GetSystemFirmwareTable('ACPI', id, nullptr, 0);
    if (size < sizeof(AcpiHeader)) return {};
    std::vector<BYTE> buf(size);
    if (GetSystemFirmwareTable('ACPI', id, buf.data(), size) != size) return {};
    return buf;
}

std::string FixedString(const char* s, size_t max) { return std::string(s, strnlen(s, max)); }

} // namespace

void CollectAcpi(std::vector<Section>& out)
{
    Card c{ "ACPI", "ACPI" };
    auto fadt = ReadTable("FACP");
    if (fadt.empty()) {
        c.fields.push_back(Unsupported("OEM ID", "ACPI tables unavailable"));
    } else {
        auto* h = (const AcpiHeader*)fadt.data();
        c.fields.push_back(MakeField("OEM ID", FixedString(h->oemId, sizeof(h->oemId))));
        c.fields.push_back(MakeField("OEM Table ID", FixedString(h->oemTableId, sizeof(h->oemTableId))));
        c.fields.push_back(MakeField("OEM Revision", Format("%08lX", h->oemRevision)));
    }

    auto msdm = ReadTable("MSDM");
    if (msdm.size() >= sizeof(Msdm)) {
        auto* m = (const Msdm*)msdm.data();
        size_t len = std::min<size_t>(m->dataLength, msdm.size() - offsetof(Msdm, data));
        c.fields.push_back(MakeField("MSDM Product Key", FixedString(m->data, len)));
    } else {
        c.fields.push_back(Field{ "MSDM Product Key", "Not present", true });
    }

    auto it = std::find_if(out.begin(), out.end(), [](const Section& s) { return s.name == "System"; });
    if (it == out.end()) out.push_back({ "System", { std::move(c) } });
    else it->cards.push_back(std::move(c));
}

} // namespace hw
