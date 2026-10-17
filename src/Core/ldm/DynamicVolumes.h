// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Finding and opening dynamic volumes (the counterpart of DiskAccessLibrary's
// WindowsDynamicVolumeHelper used by iSCSI Console's volume list).
//
// Member disks are the computer's physical disks (administrator rights
// needed), or, when `images` is not empty, only those disk images: a volume
// can be served from images of its member disks.

#pragma once

#include "Core/disk/DiskImage.h"
#include "Core/ldm/DynamicVolume.h"

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace tiscsi::ldm {

struct DynamicVolumeInfo {
    Guid guid;
    std::string name;          // "Volume1"
    std::string diskGroupName; // Windows uses the computer name followed by "Dg0"
    VolumeLayout layout = VolumeLayout::Simple;
    VolumeHealth health = VolumeHealth::Healthy;
    uint64_t size = 0;
    // Member disks that are present: physical disk numbers, or image paths.
    std::vector<std::wstring> disks;
    // "\\?\Volume{...}\" if Windows has the volume, else empty.
    std::wstring windowsVolumeName;
};

// Returns ERROR_SUCCESS or the error that kept the member disks from being
// read (ERROR_ACCESS_DENIED without administrator rights; for images, the
// error of the file named in `failedPath`).
DWORD ListDynamicVolumes(const std::vector<std::wstring>& images, std::vector<DynamicVolumeInfo>& volumes,
                         std::wstring& failedPath);

// `id`: the volume GUID (bare, in braces, or as "\\?\Volume{...}\"), its name
// if unique ("Volume1"), or, for physical disks, a drive letter or mount point.
//
// A volume Windows has is served through the operating system's volume
// handle (disk::VolumeDisk, which locks it for read-write access), as iSCSI
// Console does on Vista and later. Other volumes are assembled from their
// member disks; read-write access needs every member to be present.
std::unique_ptr<disk::Disk> OpenDynamicVolume(const std::wstring& id, bool readOnly,
                                              const std::vector<std::wstring>& images, disk::OpenError& error);

} // namespace tiscsi::ldm
