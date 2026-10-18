// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// What a server serves, independent of the front end: the targets, their
// disks and the listening address. Devices are named by the text given on
// the command line (see disk/DeviceSelector.h), so a configuration keeps
// meaning the same devices when disk numbers change.

#pragma once

#include "Core/disk/DiskImage.h"
#include "Core/util/Log.h"

#include <cstdint>
#include <memory>
#include <string>
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
};

// Opens (or, for a RAM disk, creates) the disk. Device names that match no
// device or several report OpenError::Problem::NoSuchDevice / AmbiguousDevice
// with `path` set to the name. A RAM disk that cannot be allocated reports
// ERROR_NOT_ENOUGH_MEMORY.
std::unique_ptr<disk::Disk> OpenDisk(const DiskSpec& spec, const std::vector<std::wstring>& ldmImages,
                                     disk::OpenError& error);

// The device path (\\.\CdRom0, ...) for a pass-through target's selector, or
// an OpenError with NoSuchDevice / AmbiguousDevice.
bool ResolveSptiDevice(const std::wstring& selector, std::wstring& path, disk::OpenError& error);

} // namespace tiscsi::config
