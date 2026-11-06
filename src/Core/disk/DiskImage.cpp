// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/disk/DiskImage.h"

#include "Core/disk/IsoImage.h"
#include "Core/disk/RawDiskImage.h"
#include "Core/disk/Vhd.h"
#include "Core/disk/Vhdx.h"
#include "Core/disk/Vmdk.h"

namespace tiscsi::disk {

bool HasExtension(const std::wstring& path, const wchar_t* extension)
{
    const size_t dot = path.find_last_of(L'.');
    const size_t separator = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (separator != std::wstring::npos && dot < separator)) {
        return false;
    }
    return CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, path.c_str() + dot, -1, extension, -1) == CSTR_EQUAL;
}

std::unique_ptr<Disk> OpenDiskImage(const std::wstring& path, bool readOnly, OpenError& error)
{
    return detail::OpenDiskImage(path, readOnly, error, 0);
}

const wchar_t* DifferencingExtension(const std::wstring& path)
{
    if (HasExtension(path, L".vhd")) {
        return L".vhd";
    }
    if (HasExtension(path, L".vhdx") || HasExtension(path, L".avhdx")) {
        return L".vhdx";
    }
    return HasExtension(path, L".vmdk") ? L".vmdk" : nullptr;
}

DWORD CreateDifferencingImage(const std::wstring& path, const Disk& parent)
{
    if (const auto* vhd = dynamic_cast<const VhdDisk*>(&parent)) {
        return vhd->CreateChild(path);
    }
    if (const auto* vhdx = dynamic_cast<const VhdxDisk*>(&parent)) {
        return vhdx->CreateChild(path);
    }
    if (const auto* vmdk = dynamic_cast<const VmdkDisk*>(&parent)) {
        return vmdk->CreateChild(path);
    }
    return ERROR_NOT_SUPPORTED;
}

std::unique_ptr<Disk> detail::OpenDiskImage(const std::wstring& path, bool readOnly, OpenError& error, int depth)
{
    if (HasExtension(path, L".vhdx") || HasExtension(path, L".avhdx")) {
        return VhdxDisk::Open(path, readOnly, error, depth);
    }
    if (HasExtension(path, L".vhd")) {
        return VhdDisk::Open(path, readOnly, error, depth);
    }
    if (HasExtension(path, L".vmdk")) {
        return VmdkDisk::Open(path, readOnly, error, depth);
    }
    if (HasExtension(path, L".iso")) {
        return IsoImage::Open(path, error); // read-only whatever was asked
    }
    return RawDiskImage::Open(path, readOnly, error);
}

} // namespace tiscsi::disk
