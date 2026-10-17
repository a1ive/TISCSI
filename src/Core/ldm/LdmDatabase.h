// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
// Derived from DiskAccessLibrary (LogicalDiskManager), Copyright (C) 2014-2018
// Tal Aloni <tal.aloni.il@gmail.com>, licensed under the GNU LGPL version 3 or later.
//
// On-disk structures of the Logical Disk Manager (dynamic disks): the private
// header (PRIVHEAD), table of contents (TOCBLOCK), and the volume manager
// database (VMDB header plus VBLK records). Read-only: TISCSI never changes the
// database. All multi-byte fields are big-endian.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tiscsi::ldm {

// A GUID stored in textual order (big-endian), as the LDM database stores it.
struct Guid {
    std::array<uint8_t, 16> bytes = {};

    bool operator==(const Guid&) const = default;
    bool IsNull() const;
    // Lowercase "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
    std::string ToString() const;
    // Accepts the form above, optionally in braces, any case.
    static std::optional<Guid> Parse(std::string_view text);
};

struct PrivateHeader {
    static constexpr uint32_t kLength = 512;

    uint16_t majorVersion = 0;
    uint16_t minorVersion = 0;
    uint64_t updateSequenceNumber = 0;
    uint64_t primaryPrivateHeaderLba = 0;   // relative to the private region
    uint64_t secondaryPrivateHeaderLba = 0; // relative to the private region
    std::string diskGuid;
    std::string hostGuid;
    std::string diskGroupGuid;
    std::string diskGroupName;
    uint32_t bytesPerSector = 0;
    uint64_t publicRegionStartLba = 0;
    uint64_t publicRegionSizeLba = 0;
    uint64_t privateRegionStartLba = 0;
    uint64_t privateRegionSizeLba = 0;
    uint64_t primaryTocLba = 0;   // relative to the private region
    uint64_t secondaryTocLba = 0; // relative to the private region
    bool checksumValid = false;

    std::optional<Guid> DiskGuid() const { return Guid::Parse(diskGuid); }
    std::optional<Guid> DiskGroupGuid() const { return Guid::Parse(diskGroupGuid); }
};

// Returns nullopt unless `sector` starts with the PRIVHEAD signature.
std::optional<PrivateHeader> ParsePrivateHeader(std::span<const uint8_t> sector);

struct TocRegion {
    std::string name; // "config" or "log"
    uint64_t startLba = 0; // relative to the private region
    uint64_t sizeLba = 0;
};

struct TocBlock {
    uint64_t updateSequenceNumber = 0;
    std::vector<TocRegion> regions;

    std::optional<uint64_t> ConfigStart() const;
};

// Returns nullopt unless the sector holds a TOCBLOCK with a valid checksum.
std::optional<TocBlock> ParseTocBlock(std::span<const uint8_t> sector);

enum class DatabaseUpdateStatus : uint16_t {
    Clean = 1,
    Change = 2,
    Commit = 3,
    Abort = 4,
    New = 5,
    Stale = 6,
    Offline = 8,
};

struct DatabaseHeader {
    static constexpr uint32_t kLength = 512;
    static constexpr uint32_t kFirstSequenceNumber = 4; // VBLKs 0-3 are taken by the VMDB

    uint32_t numberOfVblks = 0; // including the 4 taken by the VMDB
    uint32_t blockSize = 0;
    uint32_t headerSize = 0;
    DatabaseUpdateStatus updateStatus = DatabaseUpdateStatus::Clean;
    uint16_t majorVersion = 0;
    uint16_t minorVersion = 0;
    std::string diskGroupName;
    std::string diskGroupGuid;
    uint64_t commitTransactionId = 0;
    uint64_t pendingTransactionId = 0;

    // 4.10: Windows; 4.12: Veritas Storage Foundation groups.
    bool IsVersionSupported() const { return majorVersion == 4 && (minorVersion == 10 || minorVersion == 12); }
    // Length of the VMDB header and every VBLK, in bytes.
    uint64_t DatabaseLength() const;
};

std::optional<DatabaseHeader> ParseDatabaseHeader(std::span<const uint8_t> sector);

// Component layouts.
enum class ExtentLayout : uint8_t {
    Stripe = 1,
    Concatenated = 2,
    Raid5 = 3,
};

struct VolumeRecord {
    uint64_t id = 0;
    std::string name;       // "Volume1"
    std::string volumeType; // "gen" or "raid5"
    std::string state;      // "ACTIVE", "SYNC"
    uint32_t volumeNumber = 0;
    uint32_t volumeFlags = 0;
    uint32_t numberOfComponents = 0;
    uint64_t sizeLba = 0; // 512-byte sectors
    uint8_t partitionType = 0;
    Guid volumeGuid; // also the Windows volume GUID (\\?\Volume{...})
    std::string mountHint;
};

// a.k.a. plex: one copy of the volume's data.
struct ComponentRecord {
    uint64_t id = 0;
    std::string name;
    std::string state;
    ExtentLayout layout = ExtentLayout::Concatenated;
    uint32_t numberOfExtents = 0;
    uint64_t volumeId = 0;
    uint64_t stripeSizeLba = 0; // 512-byte sectors; striped and RAID-5 only
    uint32_t numberOfColumns = 0;
};

// a.k.a. subdisk: a contiguous range on one disk.
struct ExtentRecord {
    uint64_t id = 0;
    std::string name;
    uint64_t diskOffsetLba = 0;     // 512-byte sectors from the start of the public region
    uint64_t offsetInColumnLba = 0; // 512-byte sectors
    uint64_t sizeLba = 0;           // 512-byte sectors
    uint64_t componentId = 0;
    uint64_t diskId = 0;
    uint32_t columnIndex = 0;
};

struct DiskRecord {
    uint64_t id = 0;
    std::string name; // "Disk1"
    Guid diskGuid;
};

struct Database {
    DatabaseHeader header;
    std::vector<VolumeRecord> volumes;
    std::vector<ComponentRecord> components;
    std::vector<ExtentRecord> extents;
    std::vector<DiskRecord> disks;

    // An update was interrupted; the records cannot be trusted.
    bool IsDirty() const
    {
        return header.pendingTransactionId != header.commitTransactionId ||
               header.updateStatus != DatabaseUpdateStatus::Clean;
    }

    const DiskRecord* FindDisk(uint64_t diskId) const;
    std::vector<const ComponentRecord*> ComponentsOfVolume(uint64_t volumeId) const;
    // Sorted by offset in column.
    std::vector<const ExtentRecord*> ExtentsOfComponent(uint64_t componentId) const;
};

// Parses the config region: `bytes` starts with the VMDB header and holds at
// least header.DatabaseLength() bytes. Records pending activation are left
// out (they belong to an uncommitted change). Returns nullopt if the database
// is malformed or uses an unsupported version or record revision.
std::optional<Database> ParseDatabase(const DatabaseHeader& header, std::span<const uint8_t> bytes);

} // namespace tiscsi::ldm
