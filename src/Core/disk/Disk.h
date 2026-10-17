// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Block device abstraction served as a SCSI logical unit (the counterpart of
// DiskAccessLibrary's Disk class used by iSCSI Console).

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tiscsi::disk {

enum class IoResult {
    Ok,
    OutOfRange, // LBA range outside the disk, or a partial sector
    ReadError,
    WriteError,
};

class Disk {
public:
    virtual ~Disk() = default;

    virtual uint32_t BytesPerSector() const = 0;
    virtual uint64_t Size() const = 0; // in bytes, a multiple of BytesPerSector()
    virtual bool IsReadOnly() const = 0;
    // Short human-readable description for logs and UI (path, "RAM disk", ...).
    virtual std::wstring Description() const = 0;

    uint64_t TotalSectors() const { return Size() / BytesPerSector(); }

    // Implementations must be safe to call from one thread at a time; the
    // SCSI target serializes access per target.
    virtual IoResult ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out) = 0;
    // `data.size()` must be a multiple of BytesPerSector().
    virtual IoResult WriteSectors(uint64_t lba, std::span<const uint8_t> data) = 0;
    virtual IoResult Flush() { return IoResult::Ok; }

protected:
    bool IsInRange(uint64_t lba, uint64_t sectorCount) const
    {
        const uint64_t total = TotalSectors();
        return lba <= total && sectorCount <= total - lba;
    }
};

} // namespace tiscsi::disk
