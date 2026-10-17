// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Physical disks and volumes served as SCSI logical units.

#pragma once

#include "Core/disk/Device.h"
#include "Core/disk/DiskImage.h"

#include <memory>
#include <string>
#include <vector>

namespace tiscsi::disk {

// Common sector I/O over a RawDevice.
class DeviceDisk : public Disk {
public:
    uint32_t BytesPerSector() const override { return m_geometry.bytesPerSector; }
    uint64_t Size() const override { return m_geometry.size; }
    bool IsReadOnly() const override { return !m_device->IsWritable(); }

    IoResult ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out) override;
    IoResult WriteSectors(uint64_t lba, std::span<const uint8_t> data) override;
    IoResult Flush() override;

protected:
    DeviceDisk(std::unique_ptr<RawDevice> device, const DeviceGeometry& geometry);

    std::unique_ptr<RawDevice> m_device;
    DeviceGeometry m_geometry;
};

// \\.\PhysicalDriveN. For read-write access every volume on the disk is locked
// and dismounted for as long as the disk is open; Windows re-reads the
// partition table when it is released. Read-only access takes no locks (the
// data may change underneath, as in iSCSI Console).
class PhysicalDisk final : public DeviceDisk {
public:
    static std::unique_ptr<PhysicalDisk> Open(uint32_t index, bool readOnly, OpenError& error);
    ~PhysicalDisk() override;

    std::wstring Description() const override { return m_device->Path(); }
    uint32_t Index() const { return m_index; }

private:
    PhysicalDisk(uint32_t index, std::unique_ptr<RawDevice> device, const DeviceGeometry& geometry,
                 std::vector<std::unique_ptr<VolumeLock>> locks);

    uint32_t m_index;
    std::vector<std::unique_ptr<VolumeLock>> m_locks;
};

// \\.\PhysicalDriveN with no volume locks or checks: a member disk of a
// dynamic volume Windows has not mounted (see ldm::OpenDynamicVolume).
class RawPhysicalDisk final : public DeviceDisk {
public:
    // Read-write access fails with ERROR_WRITE_PROTECT on a read-only disk.
    static std::unique_ptr<RawPhysicalDisk> Open(uint32_t index, bool readOnly, DWORD& error);

    std::wstring Description() const override { return m_device->Path(); }
    uint32_t Index() const { return m_index; }

private:
    RawPhysicalDisk(uint32_t index, std::unique_ptr<RawDevice> device, const DeviceGeometry& geometry)
        : DeviceDisk(std::move(device), geometry), m_index(index)
    {
    }

    uint32_t m_index;
};

// A single volume served as a disk (its first sector is the volume's boot
// sector, like iSCSI Console's VolumeDisk). Uses the operating system's volume
// handle, so it works for any volume Windows has mounted, including dynamic
// ones. Read-write access locks and dismounts the volume.
class VolumeDisk final : public DeviceDisk {
public:
    // `volume`: "E:", a mount point folder or "\\?\Volume{...}\".
    static std::unique_ptr<VolumeDisk> Open(const std::wstring& volume, bool readOnly, OpenError& error);

    std::wstring Description() const override { return m_volumeName; }

private:
    VolumeDisk(std::wstring volumeName, std::unique_ptr<RawDevice> device, const DeviceGeometry& geometry);

    std::wstring m_volumeName;
};

} // namespace tiscsi::disk
