// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Names for physical disks, volumes and SCSI devices that stay the same when
// disk numbers and drive letters change (after a restart, on another
// computer or in Windows PE), for the command line and configuration files.
//
// Physical disks:
//   "2"                       disk number (not stable)
//   "serial:<text>"           serial number of the device, as Windows reports it
//   "gpt:{<disk GUID>}"       GPT disk GUID (braces optional)
//   "mbr:<signature>"         MBR disk signature, in hex
// Volumes (basic volumes on one partition):
//   "gpt:{<partition GUID>}"  unique GUID of the GPT partition
//   "mbr:<signature>:<offset>" MBR disk signature and the partition's
//                             starting offset in bytes
//   anything else is a drive letter, mount point or volume name (not stable)
// SCSI devices:
//   "serial:<text>"
//   anything else is a device path such as \\.\CdRom0 (not stable)
//
// Prefixes are case-insensitive, as are the values. A name must match exactly
// one device: a serial number or signature shared by two devices matches none.

#pragma once

#include "Core/disk/Device.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tiscsi::disk {

enum class SelectorMatch {
    Found,
    Invalid,  // not a valid name
    NotFound,
    Ambiguous,
};

// ---- Physical disks ----------------------------------------------------------

bool IsValidDiskSelector(std::wstring_view selector);
// A disk number is returned as is, without checking that the disk exists.
SelectorMatch ResolveDiskSelector(std::wstring_view selector, const std::vector<PhysicalDiskInfo>& disks,
                                  uint32_t& index);
// The stable names that match only `disk`, the most robust first: the serial
// number (which survives repartitioning), then the partition table's identity.
std::vector<std::wstring> StableDiskSelectors(const PhysicalDiskInfo& disk, const std::vector<PhysicalDiskInfo>& disks);

// ---- Volumes -----------------------------------------------------------------

// True for "gpt:" and "mbr:" names; other text is passed to ResolveVolumeName.
bool IsStableVolumeSelector(std::wstring_view selector);
bool IsValidVolumeSelector(std::wstring_view selector);
// Stable names only. `volumeName` receives "\\?\Volume{...}\".
SelectorMatch ResolveVolumeSelector(std::wstring_view selector, const std::vector<PhysicalDiskInfo>& disks,
                                    const std::vector<VolumeInfo>& volumes, std::wstring& volumeName);
// The stable name of a basic volume, if it has one that matches only it.
std::optional<std::wstring> StableVolumeSelector(const VolumeInfo& volume, const std::vector<PhysicalDiskInfo>& disks,
                                                 const std::vector<VolumeInfo>& volumes);

// ---- SCSI devices ------------------------------------------------------------

bool IsValidScsiSelector(std::wstring_view selector);
// A device path is returned as is.
SelectorMatch ResolveScsiSelector(std::wstring_view selector, const std::vector<ScsiDeviceInfo>& devices,
                                  std::wstring& path);
std::vector<std::wstring> StableScsiSelectors(const ScsiDeviceInfo& device, const std::vector<ScsiDeviceInfo>& devices);

} // namespace tiscsi::disk
