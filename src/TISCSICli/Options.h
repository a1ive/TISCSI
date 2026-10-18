// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Command-line options of TISCSICli.

#pragma once

#include "Core/util/Log.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tiscsi::cli {

struct DiskSpec {
    enum class Kind { Image, RamDisk, PhysicalDisk, Volume, DynamicVolume };
    Kind kind = Kind::Image;
    // Image: file; Volume: drive letter, mount point or volume name;
    // DynamicVolume: GUID, name, drive letter or mount point
    std::wstring path;
    bool readOnly = false;  // Image, PhysicalDisk, Volume, DynamicVolume
    bool optical = false;   // Image: served as a CD/DVD-ROM whatever its extension (--cdrom)
    uint64_t size = 0;      // RamDisk, in bytes
    uint32_t diskIndex = 0; // PhysicalDisk
};

struct TargetSpec {
    std::string name;
    std::vector<DiskSpec> disks;
    std::wstring sptiDevice; // pass-through target; excludes disks
};

struct Options {
    std::vector<TargetSpec> targets;
    uint32_t address = 0; // IPv4, network byte order; 0 = all interfaces
    std::wstring addressText = L"0.0.0.0";
    uint16_t port = 3260;
    std::wstring logFile;
    util::Severity verbosity = util::Severity::Information;
    bool help = false;
    bool list = false; // --list: show disks, volumes and SCSI devices
    // --ldm-disk: images of dynamic disks to find dynamic volumes in, instead
    // of the physical disks.
    std::vector<std::wstring> ldmImages;

    // --create-vhd <path> --size <size> [--dynamic]
    std::wstring createVhdPath;
    uint64_t createSize = 0;
    bool createDynamic = false;
};

// Value of --lang, looked up before anything is printed.
std::wstring FindLanguageOption(int argc, wchar_t** argv);

// Returns a localized error message, or std::nullopt on success.
std::optional<std::wstring> ParseOptions(int argc, wchar_t** argv, Options& options);

// "512M", "2G", "1048576" (bytes); binary units K, M, G, T with optional "B"/"iB".
std::optional<uint64_t> ParseSize(std::wstring_view text);

} // namespace tiscsi::cli
