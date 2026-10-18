// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Windows disk and volume devices: raw sector I/O, enumeration, locking.
// Replaces the parts of DiskAccessLibrary.Win32 that iSCSI Console used
// (PhysicalDiskHelper, WindowsVolumeHelper, LockHelper). Opening devices for
// I/O requires administrator rights; enumeration does not.

#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tiscsi::disk {

// A device handle (\\.\PhysicalDriveN, \\?\Volume{...}) with sector I/O
// through a page-aligned bounce buffer, as non-cached device I/O may require.
class RawDevice {
public:
    static std::unique_ptr<RawDevice> Open(const std::wstring& path, bool writable, DWORD& error);
    ~RawDevice();
    RawDevice(const RawDevice&) = delete;
    RawDevice& operator=(const RawDevice&) = delete;

    HANDLE Handle() const { return m_handle; }
    const std::wstring& Path() const { return m_path; }
    bool IsWritable() const { return m_writable; }

    bool ReadAt(uint64_t offset, uint8_t* buffer, size_t length);
    bool WriteAt(uint64_t offset, const uint8_t* buffer, size_t length);
    bool Flush();

    // Volume handles only: lock and dismount the volume through this handle
    // (a locked volume is accessible only through the locking handle).
    // Unlocked when the device is closed. Returns ERROR_SUCCESS or an error.
    DWORD LockAndDismount();

private:
    RawDevice(HANDLE handle, std::wstring path, bool writable) : m_handle(handle), m_path(std::move(path)), m_writable(writable) {}
    bool EnsureBuffer(size_t length);

    HANDLE m_handle;
    std::wstring m_path;
    bool m_writable;
    bool m_locked = false;
    uint8_t* m_buffer = nullptr;
    size_t m_bufferSize = 0;
};

// Exclusive lock on a mounted volume: FSCTL_LOCK_VOLUME + FSCTL_DISMOUNT_VOLUME
// while held, unlocked (and remounted by Windows on next access) when released.
class VolumeLock {
public:
    // `volumeName` as returned by FindFirstVolume ("\\?\Volume{...}\").
    static std::unique_ptr<VolumeLock> Acquire(const std::wstring& volumeName, DWORD& error);
    ~VolumeLock();
    VolumeLock(const VolumeLock&) = delete;
    VolumeLock& operator=(const VolumeLock&) = delete;

private:
    explicit VolumeLock(HANDLE handle) : m_handle(handle) {}
    HANDLE m_handle;
};

struct DeviceGeometry {
    uint64_t size = 0;
    uint32_t bytesPerSector = 0;
};

// IOCTL_DISK_GET_DRIVE_GEOMETRY_EX (disks) / IOCTL_DISK_GET_LENGTH_INFO.
bool QueryGeometry(HANDLE device, DeviceGeometry& geometry);

struct PartitionInfo {
    uint64_t offset = 0; // in bytes
    uint64_t length = 0;
    GUID gptPartitionId = {}; // GPT disks: the partition's unique GUID
};

struct PhysicalDiskInfo {
    uint32_t index = 0;
    std::wstring path; // \\.\PhysicalDriveN
    std::string vendor;
    std::string product;
    std::string serialNumber;
    uint64_t size = 0;
    uint32_t bytesPerSector = 0;
    bool isDynamic = false;
    // Partition table: the MBR disk signature or the GPT disk GUID (zero for
    // a disk without one), and the partitions in use.
    uint32_t mbrSignature = 0;
    GUID gptDiskId = {};
    std::vector<PartitionInfo> partitions;
};

struct VolumeExtent {
    uint32_t diskNumber = 0;
    uint64_t offset = 0; // in bytes
    uint64_t length = 0;
};

struct VolumeInfo {
    std::wstring volumeName; // \\?\Volume{...}\ (with trailing backslash)
    std::vector<std::wstring> mountPoints; // "E:\", "C:\mnt\data\"
    std::wstring label;
    std::wstring fileSystem;
    uint64_t size = 0;
    std::vector<uint32_t> diskNumbers; // disks holding the volume's extents
    std::vector<VolumeExtent> extents;
};

// A SCSI pass-through candidate.
struct ScsiDeviceInfo {
    std::wstring path; // \\.\CdRomN, \\.\TapeN or \\.\ScsiN:
    std::string vendor;
    std::string product;
    std::string serialNumber;
};

std::vector<PhysicalDiskInfo> ListPhysicalDisks();
std::vector<VolumeInfo> ListVolumes();
// Existing \\.\CdRomN, \\.\TapeN and \\.\ScsiN: devices.
std::vector<ScsiDeviceInfo> ListScsiDevices();

std::wstring PhysicalDrivePath(uint32_t index);
// "E:", "E:\", a mount point folder or a volume name -> "\\?\Volume{...}\".
std::optional<std::wstring> ResolveVolumeName(const std::wstring& volume);
// Device path for opening a volume name (no trailing backslash).
std::wstring VolumeDevicePath(const std::wstring& volumeName);

// True if the disk holds LDM (dynamic disk) metadata.
bool IsDynamicDisk(HANDLE disk);

bool IsProcessElevated();

} // namespace tiscsi::disk
