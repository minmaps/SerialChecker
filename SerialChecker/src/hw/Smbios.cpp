// SMBIOS tables read through GetSystemFirmwareTable('RSMB'): system, baseboard, chassis, BIOS, CPU, memory.
#include "hw/Util.h"

#include <windows.h>
#include <algorithm>
#include <map>

namespace hw {
namespace {

#pragma pack(push, 1)
struct RawSmbiosData {
    BYTE used20CallingMethod;
    BYTE majorVersion;
    BYTE minorVersion;
    BYTE dmiRevision;
    DWORD length;
    BYTE tableData[1];
};
struct StructHeader {
    BYTE type;
    BYTE length;
    WORD handle;
};
#pragma pack(pop)

// A formatted SMBIOS structure plus its trailing string set.
struct Structure {
    const BYTE* data = nullptr;
    BYTE length = 0;
    std::vector<std::string> strings;

    BYTE Byte(size_t off) const { return off < length ? data[off] : 0; }
    WORD Word(size_t off) const { return off + 1 < length ? *(const WORD*)(data + off) : 0; }
    DWORD Dword(size_t off) const { return off + 3 < length ? *(const DWORD*)(data + off) : 0; }
    bool Has(size_t off) const { return off < length; }

    // Strings are 1-based; 0 means "no string".
    std::string Str(size_t off) const
    {
        BYTE idx = Byte(off);
        if (idx == 0 || idx > strings.size()) return {};
        return strings[idx - 1];
    }
};

std::vector<Structure> ParseTable(const std::vector<BYTE>& raw, BYTE& major, BYTE& minor)
{
    std::vector<Structure> out;
    if (raw.size() < sizeof(RawSmbiosData)) return out;
    auto* smb = (const RawSmbiosData*)raw.data();
    major = smb->majorVersion;
    minor = smb->minorVersion;

    const BYTE* p = smb->tableData;
    const BYTE* end = smb->tableData + std::min<size_t>(smb->length, raw.size() - offsetof(RawSmbiosData, tableData));
    while (p + sizeof(StructHeader) <= end) {
        auto* hdr = (const StructHeader*)p;
        if (hdr->length < sizeof(StructHeader) || p + hdr->length > end) break;

        Structure s;
        s.data = p;
        s.length = hdr->length;

        // String set follows the formatted area and ends with a double NUL.
        const char* str = (const char*)p + hdr->length;
        while ((const BYTE*)str < end && *str) {
            size_t len = strnlen(str, end - (const BYTE*)str);
            s.strings.emplace_back(str, len);
            str += len + 1;
        }
        const BYTE* next = (const BYTE*)str;
        if (next < end && *next == 0 && s.strings.empty()) next++;  // empty string set = two NULs
        next++;

        out.push_back(std::move(s));
        if (hdr->type == 127) break;  // end-of-table
        p = next;
    }
    return out;
}

std::string FormatUuid(const BYTE* u, bool littleEndianFields)
{
    if (littleEndianFields) {
        return Format("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", u[3], u[2], u[1], u[0],
                      u[5], u[4], u[7], u[6], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    }
    return Format("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", u[0], u[1], u[2], u[3], u[4],
                  u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

std::string MemorySize(const Structure& s)
{
    WORD size = s.Word(0x0C);
    if (size == 0xFFFF) return "Unknown";
    uint64_t mb;
    if (size == 0x7FFF) mb = s.Dword(0x1C) & 0x7FFFFFFF;
    else if (size & 0x8000) mb = (size & 0x7FFF) / 1024;  // KB granularity
    else mb = size;
    if (mb >= 1024 && mb % 1024 == 0) return Format("%llu GB", mb / 1024);
    return Format("%llu MB", mb);
}

} // namespace

void CollectSmbios(std::vector<Section>& out)
{
    const DWORD sig = 'RSMB';
    UINT size = GetSystemFirmwareTable(sig, 0, nullptr, 0);
    std::vector<BYTE> raw(size);
    if (size == 0 || GetSystemFirmwareTable(sig, 0, raw.data(), size) != size) {
        out.push_back({ "System", { { "SMBIOS", "SMBIOS", { Unsupported("Status", "SMBIOS table unavailable") } } } });
        return;
    }

    BYTE major = 0, minor = 0;
    auto table = ParseTable(raw, major, minor);
    std::multimap<BYTE, const Structure*> byType;
    for (auto& s : table) byType.emplace(s.data[0], &s);
    auto first = [&](BYTE type) -> const Structure* {
        auto it = byType.find(type);
        return it == byType.end() ? nullptr : it->second;
    };
    const std::string badge = Format("SMBIOS %u.%u", major, minor);

    Section system{ "System" };
    if (auto* s = first(1)) {
        Card c{ "System Information", badge };
        c.fields.push_back(MakeField("Manufacturer", s->Str(0x04)));
        c.fields.push_back(MakeField("Product Name", s->Str(0x05)));
        c.fields.push_back(MakeField("Version", s->Str(0x06)));
        c.fields.push_back(MakeField("System Serial", s->Str(0x07)));
        if (s->Has(0x08 + 15)) {
            bool le = major > 2 || (major == 2 && minor >= 6);
            c.fields.push_back(MakeField("System UUID", FormatUuid(s->data + 0x08, le)));
        }
        c.fields.push_back(MakeField("SKU Number", s->Str(0x19)));
        c.fields.push_back(MakeField("Family", s->Str(0x1A)));
        system.cards.push_back(std::move(c));
    }
    if (auto* s = first(0)) {
        Card c{ "BIOS", badge };
        c.fields.push_back(MakeField("Vendor", s->Str(0x04)));
        c.fields.push_back(MakeField("Version", s->Str(0x05)));
        c.fields.push_back(MakeField("Release Date", s->Str(0x08)));
        system.cards.push_back(std::move(c));
    }
    if (!system.cards.empty()) out.push_back(std::move(system));

    Section board{ "Baseboard" };
    for (auto [it, end] = byType.equal_range(2); it != end; ++it) {
        auto* s = it->second;
        Card c{ "Baseboard", badge };
        c.fields.push_back(MakeField("Manufacturer", s->Str(0x04)));
        c.fields.push_back(MakeField("Product Name", s->Str(0x05)));
        c.fields.push_back(MakeField("Version", s->Str(0x06)));
        c.fields.push_back(MakeField("Serial Number", s->Str(0x07)));
        c.fields.push_back(MakeField("Asset Tag", s->Str(0x08)));
        c.fields.push_back(MakeField("Location in Chassis", s->Str(0x0A)));
        board.cards.push_back(std::move(c));
    }
    for (auto [it, end] = byType.equal_range(3); it != end; ++it) {
        auto* s = it->second;
        Card c{ "Chassis", badge };
        c.fields.push_back(MakeField("Manufacturer", s->Str(0x04)));
        c.fields.push_back(MakeField("Version", s->Str(0x06)));
        c.fields.push_back(MakeField("Serial Number", s->Str(0x07)));
        c.fields.push_back(MakeField("Asset Tag", s->Str(0x08)));
        board.cards.push_back(std::move(c));
    }
    if (!board.cards.empty()) out.push_back(std::move(board));

    Section cpu{ "Processor" };
    for (auto [it, end] = byType.equal_range(4); it != end; ++it) {
        auto* s = it->second;
        Card c{ "Processor", badge };
        c.fields.push_back(MakeField("Manufacturer", s->Str(0x07)));
        c.fields.push_back(MakeField("Processor Type", s->Str(0x10)));
        if (s->Has(0x0F)) {
            // Same layout as Win32_Processor.ProcessorId: CPUID EDX then EAX.
            c.fields.push_back(MakeField("Processor ID", Format("%08X%08X", s->Dword(0x0C), s->Dword(0x08))));
        }
        c.fields.push_back(MakeField("Serial Number", s->Str(0x20)));
        c.fields.push_back(MakeField("Part Number", s->Str(0x22)));
        c.fields.push_back(MakeField("Asset Number", s->Str(0x21)));
        c.fields.push_back(MakeField("Socket Type", s->Str(0x04)));
        cpu.cards.push_back(std::move(c));
    }
    if (!cpu.cards.empty()) out.push_back(std::move(cpu));

    Section memory{ "Memory" };
    for (auto [it, end] = byType.equal_range(17); it != end; ++it) {
        auto* s = it->second;
        WORD sz = s->Word(0x0C);
        if (sz == 0) continue;  // empty slot
        // "P0 CHANNEL A / DIMM 0": the device locator alone repeats across channels.
        std::string slot = Trim(s->Str(0x10)), bank = Trim(s->Str(0x11));
        std::string title = bank.empty() ? slot : slot.empty() ? bank : bank + " / " + slot;
        Card c{ title.empty() ? "Memory Module" : title, badge };
        c.fields.push_back(MakeField("Manufacturer", s->Str(0x17)));
        c.fields.push_back(MakeField("Part Number", s->Str(0x1A)));
        c.fields.push_back(MakeField("Serial Number", s->Str(0x18)));
        c.fields.push_back(MakeField("Asset Tag", s->Str(0x19)));
        c.fields.push_back(MakeField("Size", MemorySize(*s)));
        if (WORD speed = s->Word(0x15)) c.fields.push_back(MakeField("Speed", Format("%u MT/s", speed)));
        memory.cards.push_back(std::move(c));
    }
    if (!memory.cards.empty()) out.push_back(std::move(memory));
}

} // namespace hw
