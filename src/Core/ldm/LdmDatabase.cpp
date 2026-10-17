// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
// Derived from DiskAccessLibrary (LogicalDiskManager), Copyright (C) 2014-2018
// Tal Aloni <tal.aloni.il@gmail.com>, licensed under the GNU LGPL version 3 or later.

#include "Core/ldm/LdmDatabase.h"

#include "Core/util/ByteOrder.h"

#include <algorithm>
#include <map>

namespace tiscsi::ldm {
namespace {

using util::ReadBE16;
using util::ReadBE32;
using util::ReadBE64;

constexpr size_t kTocRegionLength = 34;
constexpr size_t kVblkHeaderLength = 16;
constexpr size_t kRecordHeaderLength = 8;
// Sanity limit for the config region (Windows uses about 1 MiB).
constexpr uint64_t kMaxDatabaseLength = 64 * 1024 * 1024;

enum class RecordType : uint8_t {
    Volume = 1,
    Component = 2,
    Extent = 3,
    Disk = 4,
    DiskGroup = 5,
};

enum class RecordUpdateStatus : uint16_t {
    Active = 0,
    ActivePendingDeletion = 1,
    PendingActivation = 2,
};

// Fixed-width ANSI field without trailing NULs.
std::string FixedString(const uint8_t* p, size_t length)
{
    size_t end = 0;
    while (end < length && p[end] != 0) {
        ++end;
    }
    return std::string(reinterpret_cast<const char*>(p), end);
}

// Checksum of PRIVHEAD and TOCBLOCK: the byte sum of the first 512 bytes,
// excluding the checksum field itself (offset 8).
uint32_t HeaderChecksum(std::span<const uint8_t> sector)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; ++i) {
        if (i < 8 || i >= 12) {
            sum += sector[i];
        }
    }
    return sum;
}

int HexDigit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Bounds-checked reader over record data; any overrun sets the failure flag.
class Reader {
public:
    explicit Reader(std::span<const uint8_t> data) : m_data(data) {}

    bool Ok() const { return m_ok; }

    void Skip(size_t length) { Take(length); }

    uint8_t U8()
    {
        const uint8_t* p = Take(1);
        return p ? p[0] : 0;
    }

    uint16_t BE16()
    {
        const uint8_t* p = Take(2);
        return p ? ReadBE16(p) : 0;
    }

    uint32_t BE32()
    {
        const uint8_t* p = Take(4);
        return p ? ReadBE32(p) : 0;
    }

    uint64_t BE64()
    {
        const uint8_t* p = Take(8);
        return p ? ReadBE64(p) : 0;
    }

    // Length byte followed by that many big-endian bytes.
    uint64_t VarULong()
    {
        const size_t length = U8();
        if (length > 8) {
            m_ok = false;
            return 0;
        }
        const uint8_t* p = Take(length);
        uint64_t value = 0;
        for (size_t i = 0; p && i < length; ++i) {
            value = value << 8 | p[i];
        }
        return value;
    }

    uint32_t VarUInt() { return static_cast<uint32_t>(VarULong()); }

    // Length byte followed by that many characters.
    std::string VarString()
    {
        const size_t length = U8();
        const uint8_t* p = Take(length);
        return p ? std::string(reinterpret_cast<const char*>(p), length) : std::string();
    }

    std::string Fixed(size_t length)
    {
        const uint8_t* p = Take(length);
        return p ? FixedString(p, length) : std::string();
    }

    Guid GuidBE()
    {
        Guid guid;
        if (const uint8_t* p = Take(16)) {
            std::copy(p, p + 16, guid.bytes.begin());
        }
        return guid;
    }

private:
    const uint8_t* Take(size_t length)
    {
        if (!m_ok || length > m_data.size() - m_offset) {
            m_ok = false;
            return nullptr;
        }
        const uint8_t* p = m_data.data() + m_offset;
        m_offset += length;
        return p;
    }

    std::span<const uint8_t> m_data;
    size_t m_offset = 0;
    bool m_ok = true;
};

struct Fragment {
    uint32_t groupNumber = 0;
    uint16_t numberInGroup = 0;
    uint16_t fragmentCount = 0;
    std::span<const uint8_t> data;
};

struct RecordHeader {
    RecordUpdateStatus updateStatus;
    uint8_t flags;
    uint8_t revision;
    RecordType type;
    uint64_t id;
    std::string name;
};

RecordHeader ReadRecordHeader(Reader& reader)
{
    RecordHeader header;
    header.updateStatus = static_cast<RecordUpdateStatus>(reader.BE16());
    header.flags = reader.U8();
    const uint8_t revisionAndType = reader.U8();
    header.revision = revisionAndType >> 4;
    header.type = static_cast<RecordType>(revisionAndType & 0x0F);
    reader.Skip(4); // data length, already used to assemble the record
    header.id = reader.VarULong();
    header.name = reader.VarString();
    return header;
}

bool ParseVolume(const RecordHeader& header, Reader& reader, VolumeRecord& record)
{
    if (header.revision != 5) {
        return false;
    }
    record.id = header.id;
    record.name = header.name;
    record.volumeType = reader.VarString();
    reader.VarString(); // disable drive letter assignment
    record.state = reader.Fixed(14);
    reader.U8(); // read policy
    record.volumeNumber = reader.VarUInt();
    record.volumeFlags = reader.BE32();
    record.numberOfComponents = reader.VarUInt();
    reader.BE64(); // commit transaction ID
    reader.BE64(); // unknown transaction ID
    record.sizeLba = reader.VarULong();
    reader.Skip(4);
    record.partitionType = reader.U8();
    record.volumeGuid = reader.GuidBE();
    if (header.flags & 0x08) {
        reader.VarULong(); // unknown ID 1
    }
    if (header.flags & 0x20) {
        reader.VarULong(); // unknown ID 2
    }
    if (header.flags & 0x80) {
        reader.VarULong(); // column size
    }
    if (header.flags & 0x02) {
        record.mountHint = reader.VarString();
    }
    return reader.Ok();
}

bool ParseComponent(const RecordHeader& header, Reader& reader, ComponentRecord& record)
{
    if (header.revision != 3) {
        return false;
    }
    record.id = header.id;
    record.name = header.name;
    record.state = reader.VarString();
    record.layout = static_cast<ExtentLayout>(reader.U8());
    reader.BE32(); // component flags
    record.numberOfExtents = reader.VarUInt();
    reader.BE64(); // commit transaction ID
    reader.Skip(8);
    record.volumeId = reader.VarULong();
    reader.VarULong(); // log subdisk
    if (header.flags & 0x10) {
        record.stripeSizeLba = reader.VarULong();
        record.numberOfColumns = reader.VarUInt();
    }
    return reader.Ok();
}

bool ParseExtent(const RecordHeader& header, Reader& reader, ExtentRecord& record)
{
    if (header.revision != 3) {
        return false;
    }
    record.id = header.id;
    record.name = header.name;
    reader.BE32(); // extent flags
    reader.BE64(); // commit transaction ID
    record.diskOffsetLba = reader.BE64();
    record.offsetInColumnLba = reader.BE64();
    record.sizeLba = reader.VarULong();
    record.componentId = reader.VarULong();
    record.diskId = reader.VarULong();
    if (header.flags & 0x08) {
        record.columnIndex = reader.VarUInt();
    }
    return reader.Ok();
}

bool ParseDisk(const RecordHeader& header, Reader& reader, DiskRecord& record)
{
    record.id = header.id;
    record.name = header.name;
    if (header.revision == 3) {
        const std::optional<Guid> guid = Guid::Parse(reader.VarString());
        if (!guid) {
            return false;
        }
        record.diskGuid = *guid;
    } else if (header.revision == 4) {
        record.diskGuid = reader.GuidBE();
    } else {
        return false;
    }
    return reader.Ok();
}

// Joins the fragments of one record (sorted by number in group) and parses it.
bool AddRecord(const std::vector<Fragment>& fragments, Database& database)
{
    std::vector<uint8_t> data;
    for (const Fragment& fragment : fragments) {
        data.insert(data.end(), fragment.data.begin(), fragment.data.end());
    }
    if (data.size() < kRecordHeaderLength) {
        return false;
    }
    const uint64_t length = kRecordHeaderLength + ReadBE32(data.data() + 4);
    if (length > data.size()) {
        return false;
    }
    Reader reader(std::span<const uint8_t>(data.data(), static_cast<size_t>(length)));
    const RecordHeader header = ReadRecordHeader(reader);
    if (!reader.Ok()) {
        return false;
    }
    if (header.updateStatus == RecordUpdateStatus::PendingActivation) {
        return true;
    }
    switch (header.type) {
    case RecordType::Volume: {
        VolumeRecord record;
        if (!ParseVolume(header, reader, record)) {
            return false;
        }
        database.volumes.push_back(std::move(record));
        return true;
    }
    case RecordType::Component: {
        ComponentRecord record;
        if (!ParseComponent(header, reader, record)) {
            return false;
        }
        database.components.push_back(std::move(record));
        return true;
    }
    case RecordType::Extent: {
        ExtentRecord record;
        if (!ParseExtent(header, reader, record)) {
            return false;
        }
        database.extents.push_back(std::move(record));
        return true;
    }
    case RecordType::Disk: {
        DiskRecord record;
        if (!ParseDisk(header, reader, record)) {
            return false;
        }
        database.disks.push_back(std::move(record));
        return true;
    }
    case RecordType::DiskGroup:
        return true; // nothing in it is needed to assemble volumes
    }
    return false; // unknown record type: do not guess
}

} // namespace

// ---- Guid --------------------------------------------------------------------

bool Guid::IsNull() const
{
    return std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; });
}

std::string Guid::ToString() const
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            text += '-';
        }
        text += kHex[bytes[i] >> 4];
        text += kHex[bytes[i] & 0x0F];
    }
    return text;
}

std::optional<Guid> Guid::Parse(std::string_view text)
{
    if (text.size() == 38 && text.front() == '{' && text.back() == '}') {
        text = text.substr(1, 36);
    }
    if (text.size() != 36) {
        return std::nullopt;
    }
    Guid guid;
    size_t index = 0;
    for (size_t i = 0; i < text.size();) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') {
                return std::nullopt;
            }
            ++i;
            continue;
        }
        const int high = HexDigit(text[i]);
        const int low = HexDigit(text[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        guid.bytes[index++] = static_cast<uint8_t>(high << 4 | low);
        i += 2;
    }
    return guid;
}

// ---- Headers -----------------------------------------------------------------

std::optional<PrivateHeader> ParsePrivateHeader(std::span<const uint8_t> sector)
{
    if (sector.size() < PrivateHeader::kLength || FixedString(sector.data(), 8) != "PRIVHEAD") {
        return std::nullopt;
    }
    const uint8_t* p = sector.data();
    PrivateHeader header;
    header.majorVersion = ReadBE16(p + 0x0C);
    header.minorVersion = ReadBE16(p + 0x0E);
    header.updateSequenceNumber = ReadBE64(p + 0x18);
    header.primaryPrivateHeaderLba = ReadBE64(p + 0x20);
    header.secondaryPrivateHeaderLba = ReadBE64(p + 0x28);
    header.diskGuid = FixedString(p + 0x30, 0x40);
    header.hostGuid = FixedString(p + 0x70, 0x40);
    header.diskGroupGuid = FixedString(p + 0xB0, 0x40);
    header.diskGroupName = FixedString(p + 0xF0, 31);
    header.bytesPerSector = ReadBE32(p + 0x10F);
    header.publicRegionStartLba = ReadBE64(p + 0x11B);
    header.publicRegionSizeLba = ReadBE64(p + 0x123);
    header.privateRegionStartLba = ReadBE64(p + 0x12B);
    header.privateRegionSizeLba = ReadBE64(p + 0x133);
    header.primaryTocLba = ReadBE64(p + 0x13B);
    header.secondaryTocLba = ReadBE64(p + 0x143);
    header.checksumValid = ReadBE32(p + 0x08) == HeaderChecksum(sector);
    return header;
}

std::optional<uint64_t> TocBlock::ConfigStart() const
{
    for (const TocRegion& region : regions) {
        if (region.name == "config") {
            return region.startLba;
        }
    }
    return std::nullopt;
}

std::optional<TocBlock> ParseTocBlock(std::span<const uint8_t> sector)
{
    if (sector.size() < 512 || FixedString(sector.data(), 8) != "TOCBLOCK" ||
        ReadBE32(sector.data() + 0x08) != HeaderChecksum(sector)) {
        return std::nullopt;
    }
    TocBlock toc;
    toc.updateSequenceNumber = ReadBE64(sector.data() + 0x0C);
    // Regions follow until one with an empty name.
    for (size_t offset = 0x24; offset + kTocRegionLength <= 512 && sector[offset] != 0; offset += kTocRegionLength) {
        TocRegion region;
        region.name = FixedString(sector.data() + offset, 8);
        region.startLba = ReadBE64(sector.data() + offset + 0x0A);
        region.sizeLba = ReadBE64(sector.data() + offset + 0x12);
        toc.regions.push_back(std::move(region));
    }
    return toc;
}

uint64_t DatabaseHeader::DatabaseLength() const
{
    const uint64_t vblks = numberOfVblks > kFirstSequenceNumber ? numberOfVblks - kFirstSequenceNumber : 0;
    return headerSize + vblks * blockSize;
}

std::optional<DatabaseHeader> ParseDatabaseHeader(std::span<const uint8_t> sector)
{
    if (sector.size() < DatabaseHeader::kLength || FixedString(sector.data(), 4) != "VMDB") {
        return std::nullopt;
    }
    const uint8_t* p = sector.data();
    DatabaseHeader header;
    header.numberOfVblks = ReadBE32(p + 0x04);
    header.blockSize = ReadBE32(p + 0x08);
    header.headerSize = ReadBE32(p + 0x0C);
    header.updateStatus = static_cast<DatabaseUpdateStatus>(ReadBE16(p + 0x10));
    header.majorVersion = ReadBE16(p + 0x12);
    header.minorVersion = ReadBE16(p + 0x14);
    header.diskGroupName = FixedString(p + 0x16, 31);
    header.diskGroupGuid = FixedString(p + 0x35, 64);
    header.commitTransactionId = ReadBE64(p + 0x75);
    header.pendingTransactionId = ReadBE64(p + 0x7D);
    if (header.blockSize <= kVblkHeaderLength || header.headerSize < DatabaseHeader::kLength ||
        header.DatabaseLength() > kMaxDatabaseLength) {
        return std::nullopt;
    }
    return header;
}

// ---- Database ----------------------------------------------------------------

const DiskRecord* Database::FindDisk(uint64_t diskId) const
{
    for (const DiskRecord& disk : disks) {
        if (disk.id == diskId) {
            return &disk;
        }
    }
    return nullptr;
}

std::vector<const ComponentRecord*> Database::ComponentsOfVolume(uint64_t volumeId) const
{
    std::vector<const ComponentRecord*> result;
    for (const ComponentRecord& component : components) {
        if (component.volumeId == volumeId) {
            result.push_back(&component);
        }
    }
    return result;
}

std::vector<const ExtentRecord*> Database::ExtentsOfComponent(uint64_t componentId) const
{
    std::vector<const ExtentRecord*> result;
    for (const ExtentRecord& extent : extents) {
        if (extent.componentId == componentId) {
            result.push_back(&extent);
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const ExtentRecord* a, const ExtentRecord* b) {
        return a->offsetInColumnLba < b->offsetInColumnLba;
    });
    return result;
}

std::optional<Database> ParseDatabase(const DatabaseHeader& header, std::span<const uint8_t> bytes)
{
    if (!header.IsVersionSupported() || bytes.size() < header.DatabaseLength()) {
        return std::nullopt;
    }
    // Records may span several VBLKs, which need not be contiguous; the
    // group number ties them together.
    std::map<uint32_t, std::vector<Fragment>> groups;
    const uint32_t count = header.numberOfVblks - DatabaseHeader::kFirstSequenceNumber;
    for (uint32_t index = 0; index < count; ++index) {
        const uint8_t* p = bytes.data() + header.headerSize + static_cast<size_t>(index) * header.blockSize;
        Fragment fragment;
        fragment.fragmentCount = ReadBE16(p + 0x0E);
        if (FixedString(p, 4) != "VBLK" || fragment.fragmentCount == 0) {
            continue; // free VBLK
        }
        fragment.groupNumber = ReadBE32(p + 0x08);
        fragment.numberInGroup = ReadBE16(p + 0x0C);
        fragment.data = std::span<const uint8_t>(p + kVblkHeaderLength, header.blockSize - kVblkHeaderLength);
        groups[fragment.groupNumber].push_back(fragment);
    }

    Database database;
    database.header = header;
    for (auto& [groupNumber, fragments] : groups) {
        std::stable_sort(fragments.begin(), fragments.end(),
                         [](const Fragment& a, const Fragment& b) { return a.numberInGroup < b.numberInGroup; });
        for (size_t i = 0; i < fragments.size(); ++i) {
            if (fragments[i].numberInGroup != i || fragments[i].fragmentCount != fragments.size()) {
                return std::nullopt; // incomplete record
            }
        }
        if (!AddRecord(fragments, database)) {
            return std::nullopt;
        }
    }
    return database;
}

} // namespace tiscsi::ldm
