// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
// Derived from DiskAccessLibrary (LogicalDiskManager), Copyright (C) 2014-2018
// Tal Aloni <tal.aloni.il@gmail.com>, licensed under the GNU LGPL version 3 or later.

#include "Core/ldm/DiskGroup.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace tiscsi::ldm {
namespace {

constexpr uint64_t kPrivateHeaderSector = 6;
constexpr uint8_t kMbrPartitionLdm = 0x42;
constexpr uint8_t kMbrPartitionGptProtective = 0xEE;
constexpr uint32_t kMaxGptEntries = 1024;

// GPT partition types as stored on disk (mixed-endian).
// 5808C8AA-7E8F-42E0-85D2-E1E90434CFB3: LDM metadata (private region).
constexpr uint8_t kGptLdmMetadata[16] = {0xAA, 0xC8, 0x08, 0x58, 0x8F, 0x7E, 0xE0, 0x42,
                                         0x85, 0xD2, 0xE1, 0xE9, 0x04, 0x34, 0xCF, 0xB3};
// AF9B60A0-1431-4F62-BC68-3311714A69AD: LDM data (public region).
constexpr uint8_t kGptLdmData[16] = {0xA0, 0x60, 0x9B, 0xAF, 0x31, 0x14, 0x62, 0x4F,
                                     0xBC, 0x68, 0x33, 0x11, 0x71, 0x4A, 0x69, 0xAD};

uint32_t ReadLE32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

uint64_t ReadLE64(const uint8_t* p) { return ReadLE32(p) | static_cast<uint64_t>(ReadLE32(p + 4)) << 32; }

bool ReadSectors(disk::Disk& disk, uint64_t lba, uint32_t count, std::vector<uint8_t>& out)
{
    return disk.ReadSectors(lba, count, out) == disk::IoResult::Ok;
}

struct GptPartition {
    uint8_t type[16];
    uint64_t firstLba;
    uint64_t lastLba;
};

std::vector<GptPartition> ReadGptPartitions(disk::Disk& disk)
{
    std::vector<GptPartition> partitions;
    std::vector<uint8_t> header;
    if (!ReadSectors(disk, 1, 1, header) || header.size() < 92 || std::memcmp(header.data(), "EFI PART", 8) != 0) {
        return partitions;
    }
    const uint64_t entriesLba = ReadLE64(header.data() + 0x48);
    const uint32_t entryCount = std::min(ReadLE32(header.data() + 0x50), kMaxGptEntries);
    const uint32_t entrySize = ReadLE32(header.data() + 0x54);
    if (entrySize < 128 || entrySize > 4096) {
        return partitions;
    }
    const uint32_t bytesPerSector = disk.BytesPerSector();
    const uint64_t length = static_cast<uint64_t>(entryCount) * entrySize;
    const uint32_t sectors = static_cast<uint32_t>((length + bytesPerSector - 1) / bytesPerSector);
    std::vector<uint8_t> entries;
    if (sectors == 0 || !ReadSectors(disk, entriesLba, sectors, entries)) {
        return partitions;
    }
    for (uint32_t i = 0; i < entryCount; ++i) {
        const uint8_t* entry = entries.data() + static_cast<size_t>(i) * entrySize;
        GptPartition partition;
        std::memcpy(partition.type, entry, 16);
        partition.firstLba = ReadLE64(entry + 0x20);
        partition.lastLba = ReadLE64(entry + 0x28);
        partitions.push_back(partition);
    }
    return partitions;
}

const GptPartition* FindGptPartition(const std::vector<GptPartition>& partitions, const uint8_t (&type)[16])
{
    for (const GptPartition& partition : partitions) {
        if (std::memcmp(partition.type, type, 16) == 0) {
            return &partition;
        }
    }
    return nullptr;
}

enum class PartitionStyle { None, Mbr, Gpt };

// Partition style from the MBR: None if sector 0 has no boot signature.
PartitionStyle ReadPartitionStyle(disk::Disk& disk, bool& hasLdmPartition)
{
    hasLdmPartition = false;
    std::vector<uint8_t> mbr;
    if (!ReadSectors(disk, 0, 1, mbr) || mbr.size() < 512 || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        return PartitionStyle::None;
    }
    bool gpt = false;
    for (int i = 0; i < 4; ++i) {
        const uint8_t type = mbr[446 + i * 16 + 4];
        hasLdmPartition |= type == kMbrPartitionLdm;
        gpt |= type == kMbrPartitionGptProtective;
    }
    return gpt ? PartitionStyle::Gpt : PartitionStyle::Mbr;
}

std::optional<PrivateHeader> ReadPrivateHeaderAt(disk::Disk& disk, uint64_t lba, bool acceptBadChecksum)
{
    std::vector<uint8_t> sector;
    if (!ReadSectors(disk, lba, 1, sector)) {
        return std::nullopt;
    }
    std::optional<PrivateHeader> header = ParsePrivateHeader(sector);
    if (header && !header->checksumValid && !acceptBadChecksum) {
        return std::nullopt;
    }
    return header;
}

// The primary private header, or the secondary one if the primary's checksum is bad.
std::optional<PrivateHeader> ReadPrivateHeaderWithFallback(disk::Disk& disk, uint64_t primaryLba)
{
    std::optional<PrivateHeader> header = ReadPrivateHeaderAt(disk, primaryLba, true);
    if (header && !header->checksumValid) {
        header = ReadPrivateHeaderAt(disk, header->privateRegionStartLba + header->secondaryPrivateHeaderLba, false);
    }
    return header;
}

std::optional<PrivateHeader> ReadPrivateHeader(disk::Disk& disk, PartitionStyle style)
{
    if (style == PartitionStyle::Gpt) {
        // The private header is the last sector of the LDM metadata partition.
        const std::vector<GptPartition> partitions = ReadGptPartitions(disk);
        const GptPartition* metadata = FindGptPartition(partitions, kGptLdmMetadata);
        return metadata ? ReadPrivateHeaderWithFallback(disk, metadata->lastLba) : std::nullopt;
    }
    // MBR: the last sector of the disk; a disk cloned to a bigger one still
    // has the copy at sector 6.
    if (disk.TotalSectors() > 0) {
        if (std::optional<PrivateHeader> header = ReadPrivateHeaderWithFallback(disk, disk.TotalSectors() - 1)) {
            return header;
        }
    }
    return ReadPrivateHeaderAt(disk, kPrivateHeaderSector, false);
}

std::optional<TocBlock> ReadTocAt(disk::Disk& disk, uint64_t lba)
{
    std::vector<uint8_t> sector;
    if (!ReadSectors(disk, lba, 1, sector)) {
        return std::nullopt;
    }
    return ParseTocBlock(sector);
}

} // namespace

bool IsDynamicDisk(disk::Disk& disk)
{
    bool hasLdmPartition = false;
    switch (ReadPartitionStyle(disk, hasLdmPartition)) {
    case PartitionStyle::Mbr:
        return hasLdmPartition;
    case PartitionStyle::Gpt: {
        const std::vector<GptPartition> partitions = ReadGptPartitions(disk);
        return FindGptPartition(partitions, kGptLdmMetadata) && FindGptPartition(partitions, kGptLdmData);
    }
    case PartitionStyle::None:
        return ReadPrivateHeaderAt(disk, kPrivateHeaderSector, false).has_value();
    }
    return false;
}

std::optional<DynamicDisk> ReadDynamicDisk(std::shared_ptr<disk::Disk> disk)
{
    if (!disk || disk->BytesPerSector() < 512 || !IsDynamicDisk(*disk)) {
        return std::nullopt;
    }
    bool hasLdmPartition = false;
    const PartitionStyle style = ReadPartitionStyle(*disk, hasLdmPartition);
    std::optional<PrivateHeader> header = ReadPrivateHeader(*disk, style);
    if (!header) {
        return std::nullopt;
    }
    std::optional<TocBlock> toc = ReadTocAt(*disk, header->privateRegionStartLba + header->primaryTocLba);
    if (!toc) {
        toc = ReadTocAt(*disk, header->privateRegionStartLba + header->secondaryTocLba);
    }
    const std::optional<Guid> diskGuid = header->DiskGuid();
    const std::optional<Guid> groupGuid = header->DiskGroupGuid();
    if (!toc || !toc->ConfigStart() || !diskGuid || !groupGuid) {
        return std::nullopt;
    }
    DynamicDisk result;
    result.disk = std::move(disk);
    result.privateHeader = std::move(*header);
    result.toc = std::move(*toc);
    result.diskGuid = *diskGuid;
    result.diskGroupGuid = *groupGuid;
    return result;
}

std::optional<Database> ReadDatabase(const DynamicDisk& dynamicDisk)
{
    disk::Disk& disk = *dynamicDisk.disk;
    const uint64_t firstSector = dynamicDisk.privateHeader.privateRegionStartLba + *dynamicDisk.toc.ConfigStart();
    std::vector<uint8_t> bytes;
    if (!ReadSectors(disk, firstSector, 1, bytes)) {
        return std::nullopt;
    }
    const std::optional<DatabaseHeader> header = ParseDatabaseHeader(bytes);
    if (!header || !header->IsVersionSupported()) {
        return std::nullopt;
    }
    // On disks with 4 KiB sectors the first VBLKs share the VMDB's sector.
    const uint32_t bytesPerSector = disk.BytesPerSector();
    const uint64_t sectors = (header->DatabaseLength() + bytesPerSector - 1) / bytesPerSector;
    if (!ReadSectors(disk, firstSector, static_cast<uint32_t>(sectors), bytes)) {
        return std::nullopt;
    }
    return ParseDatabase(*header, bytes);
}

const DynamicDisk* DiskGroup::FindDisk(const Guid& diskGuid) const
{
    for (const DynamicDisk& disk : disks) {
        if (disk.diskGuid == diskGuid) {
            return &disk;
        }
    }
    return nullptr;
}

std::vector<DiskGroup> ReadDiskGroups(const std::vector<DynamicDisk>& disks)
{
    std::map<std::string, std::vector<const DynamicDisk*>> byGroup;
    std::vector<std::string> order; // keep the order in which groups are first seen
    for (const DynamicDisk& disk : disks) {
        const std::string key = disk.diskGroupGuid.ToString();
        if (byGroup.find(key) == byGroup.end()) {
            order.push_back(key);
        }
        std::vector<const DynamicDisk*>& members = byGroup[key];
        // The same disk twice (an image given twice, a copy of a disk) would
        // serve two handles for one member; keep the first.
        if (std::none_of(members.begin(), members.end(),
                         [&disk](const DynamicDisk* member) { return member->diskGuid == disk.diskGuid; })) {
            members.push_back(&disk);
        }
    }

    std::vector<DiskGroup> groups;
    for (const std::string& key : order) {
        const std::vector<const DynamicDisk*>& members = byGroup[key];
        std::optional<Database> database = ReadDatabase(*members.front());
        if (!database || database->IsDirty()) {
            continue;
        }
        bool consistent = true;
        for (size_t i = 1; i < members.size() && consistent; ++i) {
            const std::optional<Database> other = ReadDatabase(*members[i]);
            consistent = other && !other->IsDirty() &&
                         other->header.diskGroupGuid == database->header.diskGroupGuid &&
                         other->header.diskGroupName == database->header.diskGroupName &&
                         other->header.commitTransactionId == database->header.commitTransactionId;
        }
        if (!consistent) {
            continue;
        }
        DiskGroup group;
        group.guid = members.front()->diskGroupGuid;
        group.name = database->header.diskGroupName;
        group.database = std::move(*database);
        for (const DynamicDisk* member : members) {
            group.disks.push_back(*member);
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

} // namespace tiscsi::ldm
