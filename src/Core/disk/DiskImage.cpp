// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/disk/DiskImage.h"

#include "Core/disk/RawDiskImage.h"
#include "Core/disk/Vhd.h"
#include "Core/disk/Vmdk.h"

namespace tiscsi::disk {
namespace {

bool HasExtension(const std::wstring& path, const wchar_t* extension)
{
    const size_t dot = path.find_last_of(L'.');
    const size_t separator = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (separator != std::wstring::npos && dot < separator)) {
        return false;
    }
    return CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, path.c_str() + dot, -1, extension, -1) == CSTR_EQUAL;
}

} // namespace

std::unique_ptr<Disk> OpenDiskImage(const std::wstring& path, bool readOnly, OpenError& error)
{
    return detail::OpenDiskImage(path, readOnly, error, 0);
}

std::unique_ptr<Disk> detail::OpenDiskImage(const std::wstring& path, bool readOnly, OpenError& error, int depth)
{
    if (HasExtension(path, L".vhd")) {
        return VhdDisk::Open(path, readOnly, error, depth);
    }
    if (HasExtension(path, L".vmdk")) {
        return VmdkDisk::Open(path, readOnly, error, depth);
    }
    return RawDiskImage::Open(path, readOnly, error);
}

} // namespace tiscsi::disk
