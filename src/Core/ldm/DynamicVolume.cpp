// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
// Derived from DiskAccessLibrary (LogicalDiskManager), Copyright (C) 2014-2018
// Tal Aloni <tal.aloni.il@gmail.com>, licensed under the GNU LGPL version 3 or later.

#include "Core/ldm/DynamicVolume.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace tiscsi::ldm {
namespace {

// Sector values in the LDM records are 512-byte units within the public
// region, whatever the disk's sector size.
constexpr uint64_t kRecordSectorSize = 512;
// Assumed for missing disks.
constexpr uint32_t kDefaultBytesPerSector = 512;

void XorInto(uint8_t* target, const uint8_t* source, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        target[i] ^= source[i];
    }
}

// Builds one plex from a component record; false if the records do not fit together.
bool BuildPlex(const DiskGroup& group, const ComponentRecord& component, DynamicVolume::Plex& plex,
               uint32_t& bytesPerSector)
{
    const std::vector<const ExtentRecord*> records = group.database.ExtentsOfComponent(component.id);
    if (records.empty() || records.size() != component.numberOfExtents) {
        return false;
    }
    std::map<uint32_t, DynamicVolume::Column> columns;
    for (const ExtentRecord* record : records) {
        const DiskRecord* diskRecord = group.database.FindDisk(record->diskId);
        if (!diskRecord) {
            return false;
        }
        DynamicVolume::Extent extent;
        extent.diskGuid = diskRecord->diskGuid;
        uint32_t diskSectorSize = kDefaultBytesPerSector;
        uint64_t publicRegionStart = 0;
        if (const DynamicDisk* disk = group.FindDisk(diskRecord->diskGuid)) {
            extent.disk = disk->disk;
            diskSectorSize = disk->disk->BytesPerSector();
            publicRegionStart = disk->privateHeader.publicRegionStartLba;
            // "All disks holding extents for a given volume must have the same sector size."
            if (bytesPerSector != 0 && bytesPerSector != diskSectorSize) {
                return false;
            }
            bytesPerSector = diskSectorSize;
        }
        extent.firstSector = publicRegionStart + record->diskOffsetLba * kRecordSectorSize / diskSectorSize;
        extent.sectorCount = record->sizeLba * kRecordSectorSize / diskSectorSize;
        // Concatenated plexes form a single column in offset order.
        const uint32_t columnIndex = component.layout == ExtentLayout::Concatenated ? 0 : record->columnIndex;
        columns[columnIndex].extents.push_back(std::move(extent));
    }

    plex.layout = component.layout;
    uint32_t expectedIndex = 0;
    for (auto& [index, column] : columns) {
        if (index != expectedIndex++) {
            return false; // a column without extents
        }
        plex.columns.push_back(std::move(column));
    }
    switch (component.layout) {
    case ExtentLayout::Concatenated:
        return true;
    case ExtentLayout::Stripe:
    case ExtentLayout::Raid5: {
        const size_t minimumColumns = component.layout == ExtentLayout::Raid5 ? 3 : 1;
        if (plex.columns.size() < minimumColumns ||
            (component.numberOfColumns != 0 && component.numberOfColumns != plex.columns.size())) {
            return false;
        }
        const uint32_t sectorSize = bytesPerSector != 0 ? bytesPerSector : kDefaultBytesPerSector;
        plex.sectorsPerStripe = component.stripeSizeLba * kRecordSectorSize / sectorSize;
        return plex.sectorsPerStripe > 0;
    }
    }
    return false;
}

} // namespace

// ---- Column / Plex -------------------------------------------------------------

uint64_t DynamicVolume::Column::SectorCount() const
{
    uint64_t total = 0;
    for (const Extent& extent : extents) {
        total += extent.sectorCount;
    }
    return total;
}

bool DynamicVolume::Column::IsPresent() const
{
    return std::all_of(extents.begin(), extents.end(), [](const Extent& extent) { return extent.disk != nullptr; });
}

uint64_t DynamicVolume::Plex::SectorCount() const
{
    if (layout == ExtentLayout::Concatenated) {
        return columns.front().SectorCount();
    }
    uint64_t columnSectors = UINT64_MAX;
    for (const Column& column : columns) {
        columnSectors = std::min(columnSectors, column.SectorCount());
    }
    columnSectors -= columnSectors % sectorsPerStripe; // whole stripes only
    const uint64_t dataColumns = layout == ExtentLayout::Raid5 ? columns.size() - 1 : columns.size();
    return columnSectors * dataColumns;
}

bool DynamicVolume::Plex::IsOperational() const
{
    const size_t missing = std::count_if(columns.begin(), columns.end(), [](const Column& c) { return !c.IsPresent(); });
    // RAID-5 can run with one column missing.
    return missing == 0 || (layout == ExtentLayout::Raid5 && missing == 1);
}

bool DynamicVolume::Plex::IsHealthy() const
{
    return std::all_of(columns.begin(), columns.end(), [](const Column& c) { return c.IsPresent(); });
}

// ---- DynamicVolume -------------------------------------------------------------

DynamicVolume::DynamicVolume(const VolumeRecord& record, std::string diskGroupName, std::vector<Plex> plexes,
                             uint32_t bytesPerSector)
    : m_guid(record.volumeGuid), m_name(record.name), m_diskGroupName(std::move(diskGroupName)),
      m_plexes(std::move(plexes)), m_bytesPerSector(bytesPerSector)
{
    m_sectorCount = UINT64_MAX;
    for (const Plex& plex : m_plexes) {
        m_sectorCount = std::min(m_sectorCount, plex.SectorCount());
    }
    // Read-only if any member was opened read-only (or none is present).
    m_readOnly = true;
    for (const Plex& plex : m_plexes) {
        for (const Column& column : plex.columns) {
            for (const Extent& extent : column.extents) {
                if (extent.disk) {
                    if (extent.disk->IsReadOnly()) {
                        m_readOnly = true;
                        return;
                    }
                    m_readOnly = false;
                }
            }
        }
    }
}

VolumeLayout DynamicVolume::Layout() const
{
    if (m_plexes.size() > 1) {
        return VolumeLayout::Mirrored;
    }
    switch (m_plexes.front().layout) {
    case ExtentLayout::Stripe:
        return VolumeLayout::Striped;
    case ExtentLayout::Raid5:
        return VolumeLayout::Raid5;
    case ExtentLayout::Concatenated:
        break;
    }
    return m_plexes.front().columns.front().extents.size() == 1 ? VolumeLayout::Simple : VolumeLayout::Spanned;
}

VolumeHealth DynamicVolume::Health() const
{
    if (std::all_of(m_plexes.begin(), m_plexes.end(), [](const Plex& plex) { return plex.IsHealthy(); })) {
        return VolumeHealth::Healthy;
    }
    // A mirror needs one operational plex; RAID-5 one missing column at most.
    if (std::any_of(m_plexes.begin(), m_plexes.end(), [](const Plex& plex) { return plex.IsOperational(); })) {
        return VolumeHealth::FailedRedundancy;
    }
    return VolumeHealth::Failed;
}

std::vector<Guid> DynamicVolume::MemberDisks() const
{
    std::vector<Guid> disks;
    for (const Plex& plex : m_plexes) {
        for (const Column& column : plex.columns) {
            for (const Extent& extent : column.extents) {
                if (std::find(disks.begin(), disks.end(), extent.diskGuid) == disks.end()) {
                    disks.push_back(extent.diskGuid);
                }
            }
        }
    }
    return disks;
}

std::wstring DynamicVolume::Description() const
{
    // Record names and GUIDs are ASCII.
    const std::string text = m_name + " {" + m_guid.ToString() + "}";
    return std::wstring(text.begin(), text.end());
}

disk::IoResult DynamicVolume::ReadColumn(const Column& column, uint64_t sector, uint32_t count, uint8_t* out)
{
    uint64_t extentStart = 0;
    for (const Extent& extent : column.extents) {
        if (count == 0) {
            break;
        }
        const uint64_t extentEnd = extentStart + extent.sectorCount;
        if (sector < extentEnd) {
            const uint64_t offset = sector - extentStart;
            const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(count, extent.sectorCount - offset));
            if (!extent.disk) {
                return IoResult::ReadError;
            }
            std::vector<uint8_t> buffer;
            if (extent.disk->ReadSectors(extent.firstSector + offset, chunk, buffer) != IoResult::Ok) {
                return IoResult::ReadError;
            }
            std::memcpy(out, buffer.data(), static_cast<size_t>(chunk) * m_bytesPerSector);
            out += static_cast<size_t>(chunk) * m_bytesPerSector;
            sector += chunk;
            count -= chunk;
        }
        extentStart = extentEnd;
    }
    return count == 0 ? IoResult::Ok : IoResult::ReadError;
}

disk::IoResult DynamicVolume::WriteColumn(const Column& column, uint64_t sector, uint32_t count, const uint8_t* data)
{
    uint64_t extentStart = 0;
    for (const Extent& extent : column.extents) {
        if (count == 0) {
            break;
        }
        const uint64_t extentEnd = extentStart + extent.sectorCount;
        if (sector < extentEnd) {
            const uint64_t offset = sector - extentStart;
            const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(count, extent.sectorCount - offset));
            const size_t length = static_cast<size_t>(chunk) * m_bytesPerSector;
            if (!extent.disk ||
                extent.disk->WriteSectors(extent.firstSector + offset, std::span<const uint8_t>(data, length)) !=
                    IoResult::Ok) {
                return IoResult::WriteError;
            }
            data += length;
            sector += chunk;
            count -= chunk;
        }
        extentStart = extentEnd;
    }
    return count == 0 ? IoResult::Ok : IoResult::WriteError;
}

disk::IoResult DynamicVolume::ReadPlex(const Plex& plex, uint64_t lba, uint32_t count, uint8_t* out)
{
    if (plex.layout == ExtentLayout::Concatenated) {
        return ReadColumn(plex.columns.front(), lba, count, out);
    }
    const uint64_t columns = plex.columns.size();
    const uint64_t perStripe = plex.sectorsPerStripe;
    while (count > 0) {
        const uint64_t stripe = lba / perStripe;
        const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(count, perStripe - lba % perStripe));
        const size_t length = static_cast<size_t>(chunk) * m_bytesPerSector;
        uint64_t columnIndex;
        uint64_t row;
        if (plex.layout == ExtentLayout::Stripe) {
            // Column 0: 0 3 6 ...  Column 1: 1 4 7 ...  Column 2: 2 5 8 ...
            columnIndex = stripe % columns;
            row = stripe / columns;
        } else {
            // Column 0: 0 3 P ...  Column 1: 1 P 4 ...  Column 2: P 2 5 ...
            columnIndex = stripe % columns;
            row = stripe / (columns - 1);
        }
        const uint64_t columnSector = row * perStripe + lba % perStripe;
        const Column& column = plex.columns[static_cast<size_t>(columnIndex)];
        if (column.IsPresent()) {
            if (ReadColumn(column, columnSector, chunk, out) != IoResult::Ok) {
                return IoResult::ReadError;
            }
        } else {
            if (plex.layout != ExtentLayout::Raid5) {
                return IoResult::ReadError;
            }
            // Missing RAID-5 column: the XOR of the others at the same offset.
            std::memset(out, 0, length);
            m_scratch.resize(length);
            for (size_t other = 0; other < plex.columns.size(); ++other) {
                if (other == columnIndex) {
                    continue;
                }
                if (ReadColumn(plex.columns[other], columnSector, chunk, m_scratch.data()) != IoResult::Ok) {
                    return IoResult::ReadError;
                }
                XorInto(out, m_scratch.data(), length);
            }
        }
        out += length;
        lba += chunk;
        count -= chunk;
    }
    return IoResult::Ok;
}

disk::IoResult DynamicVolume::WritePlex(const Plex& plex, uint64_t lba, uint32_t count, const uint8_t* data)
{
    if (plex.layout == ExtentLayout::Concatenated) {
        return WriteColumn(plex.columns.front(), lba, count, data);
    }
    const uint64_t columns = plex.columns.size();
    const uint64_t perStripe = plex.sectorsPerStripe;
    while (count > 0) {
        const uint64_t stripe = lba / perStripe;
        const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(count, perStripe - lba % perStripe));
        const size_t length = static_cast<size_t>(chunk) * m_bytesPerSector;
        if (plex.layout == ExtentLayout::Stripe) {
            const uint64_t columnSector = stripe / columns * perStripe + lba % perStripe;
            if (WriteColumn(plex.columns[static_cast<size_t>(stripe % columns)], columnSector, chunk, data) !=
                IoResult::Ok) {
                return IoResult::WriteError;
            }
        } else {
            const uint64_t row = stripe / (columns - 1);
            const uint64_t columnSector = row * perStripe + lba % perStripe;
            const Column& dataColumn = plex.columns[static_cast<size_t>(stripe % columns)];
            const Column& parityColumn = plex.columns[static_cast<size_t>(columns - 1 - row % columns)];
            // New parity = old parity ^ old data ^ new data.
            std::vector<uint8_t> parity(length);
            m_scratch.resize(length);
            if (ReadColumn(parityColumn, columnSector, chunk, parity.data()) != IoResult::Ok ||
                ReadColumn(dataColumn, columnSector, chunk, m_scratch.data()) != IoResult::Ok) {
                return IoResult::WriteError;
            }
            XorInto(parity.data(), m_scratch.data(), length);
            XorInto(parity.data(), data, length);
            if (WriteColumn(dataColumn, columnSector, chunk, data) != IoResult::Ok ||
                WriteColumn(parityColumn, columnSector, chunk, parity.data()) != IoResult::Ok) {
                return IoResult::WriteError;
            }
        }
        data += length;
        lba += chunk;
        count -= chunk;
    }
    return IoResult::Ok;
}

disk::IoResult DynamicVolume::ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out)
{
    if (!IsInRange(lba, count)) {
        return IoResult::OutOfRange;
    }
    out.resize(static_cast<size_t>(count) * m_bytesPerSector);
    // A mirror reads from the first plex that works.
    for (const Plex& plex : m_plexes) {
        if (plex.IsOperational() && ReadPlex(plex, lba, count, out.data()) == IoResult::Ok) {
            return IoResult::Ok;
        }
    }
    return IoResult::ReadError;
}

disk::IoResult DynamicVolume::WriteSectors(uint64_t lba, std::span<const uint8_t> data)
{
    if (m_readOnly || Health() != VolumeHealth::Healthy) {
        return IoResult::WriteError;
    }
    if (data.size() % m_bytesPerSector != 0 || !IsInRange(lba, data.size() / m_bytesPerSector)) {
        return IoResult::OutOfRange;
    }
    const uint32_t count = static_cast<uint32_t>(data.size() / m_bytesPerSector);
    for (const Plex& plex : m_plexes) {
        if (WritePlex(plex, lba, count, data.data()) != IoResult::Ok) {
            return IoResult::WriteError;
        }
    }
    return IoResult::Ok;
}

disk::IoResult DynamicVolume::Flush()
{
    std::vector<disk::Disk*> flushed;
    IoResult result = IoResult::Ok;
    for (const Plex& plex : m_plexes) {
        for (const Column& column : plex.columns) {
            for (const Extent& extent : column.extents) {
                disk::Disk* disk = extent.disk.get();
                if (disk && std::find(flushed.begin(), flushed.end(), disk) == flushed.end()) {
                    flushed.push_back(disk);
                    if (disk->Flush() != IoResult::Ok) {
                        result = IoResult::WriteError;
                    }
                }
            }
        }
    }
    return result;
}

// ---- BuildVolumes --------------------------------------------------------------

std::vector<std::unique_ptr<DynamicVolume>> BuildVolumes(const DiskGroup& group)
{
    std::vector<std::unique_ptr<DynamicVolume>> volumes;
    for (const VolumeRecord& record : group.database.volumes) {
        const std::vector<const ComponentRecord*> components = group.database.ComponentsOfVolume(record.id);
        if (components.empty() || components.size() != record.numberOfComponents) {
            continue;
        }
        std::vector<DynamicVolume::Plex> plexes;
        uint32_t bytesPerSector = 0;
        bool valid = true;
        for (const ComponentRecord* component : components) {
            DynamicVolume::Plex plex;
            if (!BuildPlex(group, *component, plex, bytesPerSector)) {
                valid = false;
                break;
            }
            plexes.push_back(std::move(plex));
        }
        if (!valid) {
            continue;
        }
        if (bytesPerSector == 0) {
            bytesPerSector = kDefaultBytesPerSector;
        }
        auto volume = std::make_unique<DynamicVolume>(record, group.name, std::move(plexes), bytesPerSector);
        if (volume->Size() > 0) {
            volumes.push_back(std::move(volume));
        }
    }
    return volumes;
}

} // namespace tiscsi::ldm
