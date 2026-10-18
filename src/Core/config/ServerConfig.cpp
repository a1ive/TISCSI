// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/config/ServerConfig.h"

#include "Core/disk/DeviceDisk.h"
#include "Core/disk/DeviceSelector.h"
#include "Core/disk/IsoImage.h"
#include "Core/disk/RamDisk.h"
#include "Core/ldm/DynamicVolumes.h"

namespace tiscsi::config {
namespace {

// Found, or reports the name in `error`.
bool CheckMatch(disk::SelectorMatch match, const std::wstring& selector, disk::OpenError& error)
{
    switch (match) {
    case disk::SelectorMatch::Found:
        return true;
    case disk::SelectorMatch::Ambiguous:
        error.Set(ERROR_INVALID_NAME, selector, disk::OpenError::Problem::AmbiguousDevice);
        return false;
    case disk::SelectorMatch::NotFound:
        error.Set(ERROR_FILE_NOT_FOUND, selector, disk::OpenError::Problem::NoSuchDevice);
        return false;
    case disk::SelectorMatch::Invalid:
        break;
    }
    error.Set(ERROR_INVALID_NAME, selector);
    return false;
}

} // namespace

std::unique_ptr<disk::Disk> OpenDisk(const DiskSpec& spec, const std::vector<std::wstring>& ldmImages,
                                     disk::OpenError& error)
{
    switch (spec.kind) {
    case DiskSpec::Kind::Image:
        return spec.optical ? disk::IsoImage::Open(spec.path, error)
                            : disk::OpenDiskImage(spec.path, spec.readOnly, error);
    case DiskSpec::Kind::RamDisk: {
        auto ramDisk = disk::RamDisk::Create(spec.size);
        if (!ramDisk) {
            error.Set(ERROR_NOT_ENOUGH_MEMORY, {});
        }
        return ramDisk;
    }
    case DiskSpec::Kind::PhysicalDisk: {
        uint32_t index = 0;
        if (!CheckMatch(disk::ResolveDiskSelector(spec.path, disk::ListPhysicalDisks(), index), spec.path, error)) {
            return nullptr;
        }
        return disk::PhysicalDisk::Open(index, spec.readOnly, error);
    }
    case DiskSpec::Kind::Volume: {
        std::wstring volume = spec.path;
        if (disk::IsStableVolumeSelector(spec.path) &&
            !CheckMatch(disk::ResolveVolumeSelector(spec.path, disk::ListPhysicalDisks(), disk::ListVolumes(), volume),
                        spec.path, error)) {
            return nullptr;
        }
        return disk::VolumeDisk::Open(volume, spec.readOnly, error);
    }
    case DiskSpec::Kind::DynamicVolume:
        return ldm::OpenDynamicVolume(spec.path, spec.readOnly, ldmImages, error);
    }
    error.Set(ERROR_INVALID_PARAMETER, spec.path);
    return nullptr;
}

bool ResolveSptiDevice(const std::wstring& selector, std::wstring& path, disk::OpenError& error)
{
    return CheckMatch(disk::ResolveScsiSelector(selector, disk::ListScsiDevices(), path), selector, error);
}

} // namespace tiscsi::config
