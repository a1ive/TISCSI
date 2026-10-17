// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/disk/DeviceDisk.h"

#include <winioctl.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <utility>

namespace tiscsi::disk {
namespace {

// Disks opened read-write by this process; serving one twice would defeat the locking.
std::mutex g_openDisksMutex;
std::set<uint32_t> g_openDisks;

} // namespace

// ---- DeviceDisk ------------------------------------------------------------

DeviceDisk::DeviceDisk(std::unique_ptr<RawDevice> device, const DeviceGeometry& geometry)
    : m_device(std::move(device)), m_geometry(geometry)
{
}

IoResult DeviceDisk::ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out)
{
    if (!IsInRange(lba, count)) {
        return IoResult::OutOfRange;
    }
    out.resize(static_cast<size_t>(count) * BytesPerSector());
    return m_device->ReadAt(lba * BytesPerSector(), out.data(), out.size()) ? IoResult::Ok : IoResult::ReadError;
}

IoResult DeviceDisk::WriteSectors(uint64_t lba, std::span<const uint8_t> data)
{
    if (IsReadOnly()) {
        return IoResult::WriteError;
    }
    if (data.size() % BytesPerSector() != 0 || !IsInRange(lba, data.size() / BytesPerSector())) {
        return IoResult::OutOfRange;
    }
    return m_device->WriteAt(lba * BytesPerSector(), data.data(), data.size()) ? IoResult::Ok : IoResult::WriteError;
}

IoResult DeviceDisk::Flush() { return m_device->Flush() ? IoResult::Ok : IoResult::WriteError; }

// ---- PhysicalDisk ----------------------------------------------------------

std::unique_ptr<PhysicalDisk> PhysicalDisk::Open(uint32_t index, bool readOnly, OpenError& error)
{
    const std::wstring path = PhysicalDrivePath(index);
    DWORD code = ERROR_SUCCESS;
    auto device = RawDevice::Open(path, !readOnly, code);
    if (!device) {
        error.Set(code, path);
        return nullptr;
    }
    DeviceGeometry geometry;
    if (!QueryGeometry(device->Handle(), geometry)) {
        error.Set(GetLastError(), path);
        return nullptr;
    }
    std::vector<std::unique_ptr<VolumeLock>> locks;
    if (!readOnly) {
        DWORD returned = 0;
        if (!DeviceIoControl(device->Handle(), IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &returned, nullptr)) {
            error.Set(ERROR_WRITE_PROTECT, path, OpenError::Problem::WriteProtected);
            return nullptr;
        }
        if (IsDynamicDisk(device->Handle())) {
            // The LDM database spans all dynamic disks; changing one from the
            // initiator side would likely corrupt it (iSCSI Console asked first).
            error.Set(ERROR_NOT_SUPPORTED, path, OpenError::Problem::DynamicDiskWrite);
            return nullptr;
        }
        {
            std::lock_guard lock(g_openDisksMutex);
            if (!g_openDisks.insert(index).second) {
                error.Set(ERROR_SHARING_VIOLATION, path, OpenError::Problem::AlreadyInUse);
                return nullptr;
            }
        }
        // Locking the disk alone does not stop Windows from using mounted
        // volumes, so lock and dismount each volume on it.
        for (const VolumeInfo& volume : ListVolumes()) {
            if (std::find(volume.diskNumbers.begin(), volume.diskNumbers.end(), index) == volume.diskNumbers.end()) {
                continue;
            }
            auto volumeLock = VolumeLock::Acquire(volume.volumeName, code);
            if (!volumeLock) {
                const std::wstring name = volume.mountPoints.empty() ? volume.volumeName : volume.mountPoints.front();
                error.Set(code, name, OpenError::Problem::VolumeLocked);
                std::lock_guard lock(g_openDisksMutex);
                g_openDisks.erase(index);
                return nullptr;
            }
            locks.push_back(std::move(volumeLock));
        }
    }
    return std::unique_ptr<PhysicalDisk>(new PhysicalDisk(index, std::move(device), geometry, std::move(locks)));
}

PhysicalDisk::PhysicalDisk(uint32_t index, std::unique_ptr<RawDevice> device, const DeviceGeometry& geometry,
                           std::vector<std::unique_ptr<VolumeLock>> locks)
    : DeviceDisk(std::move(device), geometry), m_index(index), m_locks(std::move(locks))
{
}

PhysicalDisk::~PhysicalDisk()
{
    if (!m_device->IsWritable()) {
        return;
    }
    // The initiator may have changed the partition table: have Windows
    // re-read it, then release the volumes so they are mounted again.
    DWORD returned = 0;
    m_device->Flush();
    DeviceIoControl(m_device->Handle(), IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &returned, nullptr);
    m_device.reset();
    m_locks.clear();
    std::lock_guard lock(g_openDisksMutex);
    g_openDisks.erase(m_index);
}

// ---- RawPhysicalDisk ---------------------------------------------------------

std::unique_ptr<RawPhysicalDisk> RawPhysicalDisk::Open(uint32_t index, bool readOnly, DWORD& error)
{
    auto device = RawDevice::Open(PhysicalDrivePath(index), !readOnly, error);
    if (!device) {
        return nullptr;
    }
    DeviceGeometry geometry;
    if (!QueryGeometry(device->Handle(), geometry)) {
        error = GetLastError();
        return nullptr;
    }
    DWORD returned = 0;
    if (!readOnly &&
        !DeviceIoControl(device->Handle(), IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &returned, nullptr)) {
        error = ERROR_WRITE_PROTECT;
        return nullptr;
    }
    error = ERROR_SUCCESS;
    return std::unique_ptr<RawPhysicalDisk>(new RawPhysicalDisk(index, std::move(device), geometry));
}

// ---- VolumeDisk --------------------------------------------------------------

std::unique_ptr<VolumeDisk> VolumeDisk::Open(const std::wstring& volume, bool readOnly, OpenError& error)
{
    const std::optional<std::wstring> volumeName = ResolveVolumeName(volume);
    if (!volumeName) {
        error.Set(GetLastError(), volume);
        return nullptr;
    }
    DWORD code = ERROR_SUCCESS;
    auto device = RawDevice::Open(VolumeDevicePath(*volumeName), !readOnly, code);
    if (!device) {
        error.Set(code, volume);
        return nullptr;
    }
    DeviceGeometry geometry;
    if (!QueryGeometry(device->Handle(), geometry)) {
        error.Set(GetLastError(), volume);
        return nullptr;
    }
    if (!readOnly) {
        // Also keeps a second instance (or this one) from serving it twice.
        code = device->LockAndDismount();
        if (code != ERROR_SUCCESS) {
            error.Set(code, volume, OpenError::Problem::VolumeLocked);
            return nullptr;
        }
    }
    return std::unique_ptr<VolumeDisk>(new VolumeDisk(*volumeName, std::move(device), geometry));
}

VolumeDisk::VolumeDisk(std::wstring volumeName, std::unique_ptr<RawDevice> device, const DeviceGeometry& geometry)
    : DeviceDisk(std::move(device), geometry), m_volumeName(std::move(volumeName))
{
}

} // namespace tiscsi::disk
