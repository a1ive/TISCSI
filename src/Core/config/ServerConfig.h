// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// What a server serves, independent of the front end: the targets, their
// disks and the listening address. Devices are named by the text given on
// the command line (see disk/DeviceSelector.h), so a configuration keeps
// meaning the same devices when disk numbers change.

#pragma once

#include "Core/disk/DiskImage.h"
#include "Core/iscsi/IscsiTarget.h"
#include "Core/util/Log.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tiscsi::config {

struct DiskSpec {
    enum class Kind { Image, RamDisk, PhysicalDisk, Volume, DynamicVolume };
    Kind kind = Kind::Image;
    // Image: file; PhysicalDisk: disk selector; Volume: volume selector,
    // drive letter, mount point or volume name; DynamicVolume: GUID, name,
    // drive letter or mount point
    std::wstring path;
    bool readOnly = false; // Image, PhysicalDisk, Volume, DynamicVolume
    bool optical = false;  // Image: served as a CD/DVD-ROM whatever its extension
    uint64_t size = 0;     // RamDisk, in bytes
};

struct TargetSpec {
    std::string name;
    std::vector<DiskSpec> disks;
    std::wstring sptiDevice; // pass-through target (SCSI device selector); excludes disks
    iscsi::TargetAccess access;
};

struct ServerConfig {
    std::vector<TargetSpec> targets;
    uint32_t address = 0; // IPv4, network byte order; 0 = all interfaces
    std::wstring addressText = L"0.0.0.0";
    uint16_t port = 3260;
    std::wstring logFile;
    util::Severity verbosity = util::Severity::Information;
    // Images of dynamic disks to find dynamic volumes in, instead of the
    // physical disks.
    std::vector<std::wstring> ldmImages;
    // How long to wait at startup for disks, volumes and images that are not
    // there yet (devices still being detected, a USB drive), and for the
    // listening address.
    uint32_t waitSeconds = 0;
    // Serve the other targets when one cannot be opened, instead of none.
    bool skipUnavailable = false;
};

constexpr uint32_t kMaxWaitSeconds = 24 * 60 * 60;

// "512M", "2G", "1048576" (bytes); binary units K, M, G, T with optional "B"/"iB".
std::optional<uint64_t> ParseSize(std::wstring_view text);
// The shortest form ParseSize reads back ("512M").
std::wstring FormatSize(uint64_t bytes);
// Dotted IPv4 address, network byte order.
std::optional<uint32_t> ParseIpv4(std::wstring_view text);
// "error", "warning", "info", "verbose", "debug".
std::optional<util::Severity> ParseVerbosity(std::wstring_view text);
std::wstring VerbosityName(util::Severity severity);

// Opens (or, for a RAM disk, creates) the disk. Device names that match no
// device or several report OpenError::Problem::NoSuchDevice / AmbiguousDevice
// with `path` set to the name. A RAM disk that cannot be allocated reports
// ERROR_NOT_ENOUGH_MEMORY.
std::unique_ptr<disk::Disk> OpenDisk(const DiskSpec& spec, const std::vector<std::wstring>& ldmImages,
                                     disk::OpenError& error);

// The device path (\\.\CdRom0, ...) for a pass-through target's selector, or
// an OpenError with NoSuchDevice / AmbiguousDevice.
bool ResolveSptiDevice(const std::wstring& selector, std::wstring& path, disk::OpenError& error);

// Whether a failure to open a disk may go away by itself: the device, volume
// or file is not there (yet), or another process has the file open.
bool MayAppearLater(const disk::OpenError& error);

// Replaces a disk number, drive letter, mount point, volume name or device
// path by the device's stable name (see disk/DeviceSelector.h), and a
// dynamic volume's name or drive letter by its GUID, so the configuration
// keeps naming the same device. Image paths become absolute. Returns false
// if the device has no stable name (the spec is left as it was).
bool UseStableName(DiskSpec& spec, const std::vector<std::wstring>& ldmImages);
bool UseStableSptiName(std::wstring& device);
// Whether the spec already names its device by a stable name.
bool HasStableName(const DiskSpec& spec);
bool IsStableSptiName(const std::wstring& device);

} // namespace tiscsi::config
