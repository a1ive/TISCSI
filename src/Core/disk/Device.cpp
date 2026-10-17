// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/disk/Device.h"

#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace tiscsi::disk {
namespace {

constexpr size_t kMaxChunk = 16 * 1024 * 1024;
constexpr uint32_t kMaxDiskIndex = 128;

// GPT partition type of the LDM metadata partition on dynamic disks.
constexpr GUID kPartitionLdmMetadata = {0x5808C8AA, 0x7E8F, 0x42E0, {0x85, 0xD2, 0xE1, 0xE9, 0x04, 0x34, 0xCF, 0xB3}};
constexpr BYTE kMbrPartitionLdm = 0x42;

class Handle {
public:
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : m_h(h) {}
    ~Handle()
    {
        if (m_h != INVALID_HANDLE_VALUE) {
            CloseHandle(m_h);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return m_h; }
    bool valid() const { return m_h != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_h;
};

// Opening with no access rights is enough for metadata IOCTLs and does not
// need administrator rights.
HANDLE OpenForQuery(const std::wstring& path)
{
    return CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

template <typename T>
bool Ioctl(HANDLE device, DWORD code, T& out)
{
    DWORD returned = 0;
    return DeviceIoControl(device, code, nullptr, 0, &out, sizeof(out), &returned, nullptr) != FALSE;
}

OVERLAPPED OffsetOverlapped(uint64_t offset)
{
    OVERLAPPED ov = {};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return ov;
}

std::string TrimmedField(const std::vector<uint8_t>& buffer, DWORD offset)
{
    if (offset == 0 || offset >= buffer.size()) {
        return {};
    }
    std::string text(reinterpret_cast<const char*>(buffer.data() + offset));
    const size_t first = text.find_first_not_of(' ');
    const size_t last = text.find_last_not_of(' ');
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

void QueryDeviceDescriptor(HANDLE device, PhysicalDiskInfo& info)
{
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    std::vector<uint8_t> buffer(1024);
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(),
                         static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
        return;
    }
    buffer.push_back(0); // fields are NUL-terminated within the buffer; guard the end
    const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
    info.vendor = TrimmedField(buffer, descriptor->VendorIdOffset);
    info.product = TrimmedField(buffer, descriptor->ProductIdOffset);
    info.serialNumber = TrimmedField(buffer, descriptor->SerialNumberOffset);
}

std::vector<uint32_t> VolumeDiskNumbers(HANDLE volume)
{
    std::vector<uint8_t> buffer(sizeof(VOLUME_DISK_EXTENTS) + 31 * sizeof(DISK_EXTENT));
    DWORD returned = 0;
    std::vector<uint32_t> disks;
    if (!DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buffer.data(),
                         static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
        return disks;
    }
    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
    for (DWORD i = 0; i < extents->NumberOfDiskExtents && i < 32; ++i) {
        const uint32_t disk = extents->Extents[i].DiskNumber;
        if (std::find(disks.begin(), disks.end(), disk) == disks.end()) {
            disks.push_back(disk);
        }
    }
    return disks;
}

} // namespace

// ---- RawDevice -------------------------------------------------------------

std::unique_ptr<RawDevice> RawDevice::Open(const std::wstring& path, bool writable, DWORD& error)
{
    // Devices must be opened shared; exclusivity comes from volume locks.
    HANDLE handle = CreateFileW(path.c_str(), writable ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return nullptr;
    }
    error = ERROR_SUCCESS;
    return std::unique_ptr<RawDevice>(new RawDevice(handle, path, writable));
}

RawDevice::~RawDevice()
{
    if (m_buffer) {
        VirtualFree(m_buffer, 0, MEM_RELEASE);
    }
    if (m_locked) {
        DWORD returned = 0;
        DeviceIoControl(m_handle, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &returned, nullptr);
    }
    CloseHandle(m_handle);
}

DWORD RawDevice::LockAndDismount()
{
    DWORD returned = 0;
    // Locking fails while files on the volume are open (e.g. the system volume).
    if (!DeviceIoControl(m_handle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &returned, nullptr)) {
        return GetLastError();
    }
    m_locked = true;
    // Dismounting makes the file system drop its cached state, so raw writes
    // are allowed (Vista+) and nothing stale is written back later.
    DeviceIoControl(m_handle, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &returned, nullptr);
    return ERROR_SUCCESS;
}

bool RawDevice::EnsureBuffer(size_t length)
{
    if (length <= m_bufferSize) {
        return true;
    }
    if (m_buffer) {
        VirtualFree(m_buffer, 0, MEM_RELEASE);
    }
    m_bufferSize = std::max(length, size_t{1024 * 1024});
    m_buffer = static_cast<uint8_t*>(VirtualAlloc(nullptr, m_bufferSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!m_buffer) {
        m_bufferSize = 0;
        return false;
    }
    return true;
}

bool RawDevice::ReadAt(uint64_t offset, uint8_t* buffer, size_t length)
{
    while (length > 0) {
        const size_t chunk = std::min(length, kMaxChunk);
        if (!EnsureBuffer(chunk)) {
            return false;
        }
        OVERLAPPED ov = OffsetOverlapped(offset);
        DWORD read = 0;
        if (!ReadFile(m_handle, m_buffer, static_cast<DWORD>(chunk), &read, &ov) || read != chunk) {
            return false;
        }
        std::memcpy(buffer, m_buffer, chunk);
        buffer += chunk;
        offset += chunk;
        length -= chunk;
    }
    return true;
}

bool RawDevice::WriteAt(uint64_t offset, const uint8_t* buffer, size_t length)
{
    while (length > 0) {
        const size_t chunk = std::min(length, kMaxChunk);
        if (!EnsureBuffer(chunk)) {
            return false;
        }
        std::memcpy(m_buffer, buffer, chunk);
        OVERLAPPED ov = OffsetOverlapped(offset);
        DWORD written = 0;
        if (!WriteFile(m_handle, m_buffer, static_cast<DWORD>(chunk), &written, &ov) || written != chunk) {
            return false;
        }
        buffer += chunk;
        offset += chunk;
        length -= chunk;
    }
    return true;
}

bool RawDevice::Flush() { return !m_writable || FlushFileBuffers(m_handle); }

// ---- VolumeLock ------------------------------------------------------------

std::unique_ptr<VolumeLock> VolumeLock::Acquire(const std::wstring& volumeName, DWORD& error)
{
    HANDLE handle = CreateFileW(VolumeDevicePath(volumeName).c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return nullptr;
    }
    DWORD returned = 0;
    if (!DeviceIoControl(handle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &returned, nullptr)) {
        error = GetLastError();
        CloseHandle(handle);
        return nullptr;
    }
    // With the file system dismounted, Vista+ allows raw writes to the
    // volume's sectors through the disk handle.
    DeviceIoControl(handle, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &returned, nullptr);
    error = ERROR_SUCCESS;
    return std::unique_ptr<VolumeLock>(new VolumeLock(handle));
}

VolumeLock::~VolumeLock()
{
    DWORD returned = 0;
    DeviceIoControl(m_handle, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &returned, nullptr);
    CloseHandle(m_handle);
}

// ---- Queries -----------------------------------------------------------------

bool QueryGeometry(HANDLE device, DeviceGeometry& geometry)
{
    DISK_GEOMETRY_EX diskGeometry = {};
    if (Ioctl(device, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, diskGeometry)) {
        geometry.bytesPerSector = diskGeometry.Geometry.BytesPerSector;
        geometry.size = static_cast<uint64_t>(diskGeometry.DiskSize.QuadPart);
    } else {
        DISK_GEOMETRY basic = {};
        if (!Ioctl(device, IOCTL_DISK_GET_DRIVE_GEOMETRY, basic)) {
            return false;
        }
        geometry.bytesPerSector = basic.BytesPerSector;
    }
    // Volumes: the length of the volume itself (the geometry describes the disk).
    GET_LENGTH_INFORMATION length = {};
    if (Ioctl(device, IOCTL_DISK_GET_LENGTH_INFO, length)) {
        geometry.size = static_cast<uint64_t>(length.Length.QuadPart);
    }
    if (geometry.bytesPerSector == 0) {
        geometry.bytesPerSector = 512;
    }
    geometry.size = geometry.size / geometry.bytesPerSector * geometry.bytesPerSector;
    return geometry.size > 0;
}

bool IsDynamicDisk(HANDLE disk)
{
    std::vector<uint8_t> buffer(sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 127 * sizeof(PARTITION_INFORMATION_EX));
    DWORD returned = 0;
    if (!DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buffer.data(),
                         static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
        return false;
    }
    const auto* layout = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buffer.data());
    for (DWORD i = 0; i < layout->PartitionCount && i < 128; ++i) {
        const PARTITION_INFORMATION_EX& partition = layout->PartitionEntry[i];
        if (partition.PartitionStyle == PARTITION_STYLE_MBR && partition.Mbr.PartitionType == kMbrPartitionLdm) {
            return true;
        }
        if (partition.PartitionStyle == PARTITION_STYLE_GPT &&
            IsEqualGUID(partition.Gpt.PartitionType, kPartitionLdmMetadata)) {
            return true;
        }
    }
    return false;
}

std::wstring PhysicalDrivePath(uint32_t index) { return std::format(L"\\\\.\\PhysicalDrive{}", index); }

std::vector<PhysicalDiskInfo> ListPhysicalDisks()
{
    std::vector<PhysicalDiskInfo> disks;
    for (uint32_t index = 0; index < kMaxDiskIndex; ++index) {
        PhysicalDiskInfo info;
        info.index = index;
        info.path = PhysicalDrivePath(index);
        Handle device(OpenForQuery(info.path));
        if (!device.valid()) {
            continue;
        }
        DeviceGeometry geometry;
        if (QueryGeometry(device.get(), geometry)) {
            info.size = geometry.size;
            info.bytesPerSector = geometry.bytesPerSector;
        }
        QueryDeviceDescriptor(device.get(), info);
        info.isDynamic = IsDynamicDisk(device.get());
        disks.push_back(std::move(info));
    }
    return disks;
}

std::vector<std::wstring> ListScsiDevicePaths()
{
    std::vector<std::wstring> paths;
    for (const wchar_t* format : {L"\\\\.\\CdRom{}", L"\\\\.\\Tape{}", L"\\\\.\\Scsi{}:"}) {
        for (uint32_t index = 0; index < 32; ++index) {
            const std::wstring path = std::vformat(format, std::make_wformat_args(index));
            Handle device(OpenForQuery(path));
            if (device.valid()) {
                paths.push_back(path);
            }
        }
    }
    return paths;
}

std::wstring VolumeDevicePath(const std::wstring& volumeName)
{
    std::wstring path = volumeName;
    if (!path.empty() && path.back() == L'\\') {
        path.pop_back();
    }
    return path;
}

std::optional<std::wstring> ResolveVolumeName(const std::wstring& volume)
{
    std::wstring mountPoint = volume;
    if (mountPoint.size() == 2 && mountPoint[1] == L':') {
        mountPoint += L'\\';
    }
    if (mountPoint.empty() || mountPoint.back() != L'\\') {
        mountPoint += L'\\';
    }
    wchar_t name[MAX_PATH] = {};
    if (!GetVolumeNameForVolumeMountPointW(mountPoint.c_str(), name, MAX_PATH)) {
        return std::nullopt;
    }
    return std::wstring(name);
}

std::vector<VolumeInfo> ListVolumes()
{
    std::vector<VolumeInfo> volumes;
    wchar_t name[MAX_PATH] = {};
    HANDLE find = FindFirstVolumeW(name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) {
        return volumes;
    }
    do {
        VolumeInfo info;
        info.volumeName = name;

        DWORD length = 0;
        GetVolumePathNamesForVolumeNameW(name, nullptr, 0, &length);
        std::vector<wchar_t> paths(length + 1, L'\0');
        if (length && GetVolumePathNamesForVolumeNameW(name, paths.data(), length, &length)) {
            for (const wchar_t* p = paths.data(); *p; p += wcslen(p) + 1) {
                info.mountPoints.emplace_back(p);
            }
        }

        wchar_t label[MAX_PATH + 1] = {};
        wchar_t fileSystem[MAX_PATH + 1] = {};
        if (GetVolumeInformationW(name, label, MAX_PATH + 1, nullptr, nullptr, nullptr, fileSystem, MAX_PATH + 1)) {
            info.label = label;
            info.fileSystem = fileSystem;
        }

        Handle device(OpenForQuery(VolumeDevicePath(name)));
        if (device.valid()) {
            info.diskNumbers = VolumeDiskNumbers(device.get());
            GET_LENGTH_INFORMATION lengthInfo = {};
            if (Ioctl(device.get(), IOCTL_DISK_GET_LENGTH_INFO, lengthInfo)) {
                info.size = static_cast<uint64_t>(lengthInfo.Length.QuadPart);
            }
        }
        if (info.size == 0) {
            // Without administrator rights: the file system's size.
            ULARGE_INTEGER total = {};
            if (GetDiskFreeSpaceExW(name, nullptr, &total, nullptr)) {
                info.size = total.QuadPart;
            }
        }
        volumes.push_back(std::move(info));
    } while (FindNextVolumeW(find, name, MAX_PATH));
    FindVolumeClose(find);
    return volumes;
}

bool IsProcessElevated()
{
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (!AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0,
                                  0, 0, &administrators)) {
        return false;
    }
    BOOL member = FALSE;
    CheckTokenMembership(nullptr, administrators, &member);
    FreeSid(administrators);
    return member != FALSE;
}

} // namespace tiscsi::disk
