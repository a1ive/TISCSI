// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
// Derived from DiskAccessLibrary (LogicalDiskManager), Copyright (C) 2014-2018
// Tal Aloni <tal.aloni.il@gmail.com>, licensed under the GNU LGPL version 3 or later.
//
// A dynamic volume assembled from its extents on the member disks, served as a
// disk (its first sector is the volume's boot sector).

#pragma once

#include "Core/disk/Disk.h"
#include "Core/ldm/DiskGroup.h"

#include <memory>
#include <string>
#include <vector>

namespace tiscsi::ldm {

enum class VolumeLayout { Simple, Spanned, Striped, Mirrored, Raid5 };

enum class VolumeHealth {
    Healthy,
    FailedRedundancy, // a mirror or RAID-5 member is missing; the data is still complete
    Failed,           // data is missing
};

class DynamicVolume final : public disk::Disk {
public:
    using IoResult = disk::IoResult;

    struct Extent {
        std::shared_ptr<disk::Disk> disk; // null if the disk is missing
        Guid diskGuid;
        uint64_t firstSector = 0; // in disk sectors
        uint64_t sectorCount = 0;
    };

    // A sequence of extents read as one contiguous range.
    struct Column {
        std::vector<Extent> extents;

        uint64_t SectorCount() const;
        bool IsPresent() const; // every extent's disk is present
    };

    // One copy of the volume's data (a component record).
    struct Plex {
        ExtentLayout layout = ExtentLayout::Concatenated;
        std::vector<Column> columns; // a single column when concatenated
        uint64_t sectorsPerStripe = 0;

        uint64_t SectorCount() const;
        bool IsOperational() const;
        bool IsHealthy() const;
    };

    DynamicVolume(const VolumeRecord& record, std::string diskGroupName, std::vector<Plex> plexes,
                  uint32_t bytesPerSector);

    const Guid& VolumeGuid() const { return m_guid; }
    const std::string& Name() const { return m_name; }
    const std::string& DiskGroupName() const { return m_diskGroupName; }
    VolumeLayout Layout() const;
    VolumeHealth Health() const;
    // Disks holding the volume's extents, present or not, without duplicates.
    std::vector<Guid> MemberDisks() const;

    uint32_t BytesPerSector() const override { return m_bytesPerSector; }
    uint64_t Size() const override { return m_sectorCount * m_bytesPerSector; }
    bool IsReadOnly() const override { return m_readOnly; }
    std::wstring Description() const override;

    IoResult ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out) override;
    // Refused unless the volume is healthy: writing to a degraded mirror or
    // RAID-5 volume would leave the members inconsistent without the LDM
    // database recording it.
    IoResult WriteSectors(uint64_t lba, std::span<const uint8_t> data) override;
    IoResult Flush() override;

    // Member disks may be writable while the volume is served read-only.
    void MakeReadOnly() { m_readOnly = true; }
    // Released when the volume is destroyed (used to track open volumes).
    void SetLease(std::shared_ptr<void> lease) { m_lease = std::move(lease); }

private:
    IoResult ReadPlex(const Plex& plex, uint64_t lba, uint32_t count, uint8_t* out);
    IoResult WritePlex(const Plex& plex, uint64_t lba, uint32_t count, const uint8_t* data);
    IoResult ReadColumn(const Column& column, uint64_t sector, uint32_t count, uint8_t* out);
    IoResult WriteColumn(const Column& column, uint64_t sector, uint32_t count, const uint8_t* data);

    Guid m_guid;
    std::string m_name;
    std::string m_diskGroupName;
    std::vector<Plex> m_plexes; // more than one: mirrored
    uint32_t m_bytesPerSector;
    uint64_t m_sectorCount;
    bool m_readOnly;
    std::vector<uint8_t> m_scratch;
    std::shared_ptr<void> m_lease;
};

// Builds every volume of the group whose records are consistent; extents on
// missing disks are kept with a null disk.
std::vector<std::unique_ptr<DynamicVolume>> BuildVolumes(const DiskGroup& group);

} // namespace tiscsi::ldm
