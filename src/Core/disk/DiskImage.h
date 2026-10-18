// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Opens a disk image file by extension, like DiskAccessLibrary's
// DiskImage.GetDiskImage: .vhd, .vmdk, .iso (a CD/DVD-ROM disc), anything
// else is a raw image.

#pragma once

#include "Core/disk/Disk.h"

#include <windows.h>

#include <memory>
#include <string>

namespace tiscsi::disk {

struct OpenError {
    // Conditions with no fitting Win32 error, so the UI can explain them.
    enum class Problem {
        None,
        DynamicDiskWrite, // read-write access to a dynamic disk would corrupt the LDM database
        VolumeLocked,     // a volume could not be locked (files open on it); `path` names it
        AlreadyInUse,     // the device is already served by this process
        WriteProtected,   // the disk is read-only
        NoSuchVolume,     // no dynamic volume matches `path` (a GUID, name or mount point)
        AmbiguousVolume,  // several dynamic volumes have the name `path`
        VolumeFailed,     // disks holding the dynamic volume's data are missing
        DegradedWrite,    // read-write access to a dynamic volume with a missing member
    };

    DWORD code = ERROR_SUCCESS;
    // The file that could not be used: the image itself, or a parent / extent
    // (or the disk / volume device).
    std::wstring path;
    Problem problem = Problem::None;

    void Set(DWORD errorCode, std::wstring file, Problem kind = Problem::None)
    {
        code = errorCode;
        path = std::move(file);
        problem = kind;
    }
};

// Read-write images are opened exclusively; parents of differencing images
// and .iso images are always opened read-only (shared).
std::unique_ptr<Disk> OpenDiskImage(const std::wstring& path, bool readOnly, OpenError& error);

namespace detail {
// Differencing chains deeper than this are treated as corrupt (loops).
constexpr int kMaxParentDepth = 32;
std::unique_ptr<Disk> OpenDiskImage(const std::wstring& path, bool readOnly, OpenError& error, int depth);
} // namespace detail

} // namespace tiscsi::disk
