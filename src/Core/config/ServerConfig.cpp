// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/config/ServerConfig.h"

#include "Core/disk/DeviceDisk.h"
#include "Core/disk/DeviceSelector.h"
#include "Core/disk/File.h"
#include "Core/disk/IsoImage.h"
#include "Core/disk/RamDisk.h"
#include "Core/ldm/DynamicVolumes.h"

#include <winsock2.h>

#include <algorithm>
#include <cwctype>
#include <format>

namespace tiscsi::config {
namespace {

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
               return std::towlower(x) == std::towlower(y);
           });
}

bool IsNumber(std::wstring_view text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

std::wstring GuidName(const ldm::Guid& guid)
{
    const std::string text = guid.ToString();
    return L"{" + std::wstring(text.begin(), text.end()) + L"}";
}

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

// ---- Values ------------------------------------------------------------------

std::optional<uint64_t> ParseSize(std::wstring_view text)
{
    size_t digits = 0;
    while (digits < text.size() && std::iswdigit(text[digits])) {
        ++digits;
    }
    if (digits == 0 || digits > 19) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < digits; ++i) {
        value = value * 10 + (text[i] - L'0');
    }
    std::wstring_view suffix = text.substr(digits);
    if (suffix.ends_with(L"iB") || suffix.ends_with(L"ib")) {
        suffix.remove_suffix(2);
    } else if (suffix.ends_with(L"B") || suffix.ends_with(L"b")) {
        suffix.remove_suffix(1);
    }
    int shift = 0;
    if (suffix.size() == 1) {
        switch (std::towupper(suffix[0])) {
        case L'K':
            shift = 10;
            break;
        case L'M':
            shift = 20;
            break;
        case L'G':
            shift = 30;
            break;
        case L'T':
            shift = 40;
            break;
        default:
            return std::nullopt;
        }
    } else if (!suffix.empty()) {
        return std::nullopt;
    }
    if (shift > 0 && value > (UINT64_MAX >> shift)) {
        return std::nullopt;
    }
    return value << shift;
}

std::wstring FormatSize(uint64_t bytes)
{
    static constexpr struct {
        int shift;
        wchar_t unit;
    } kUnits[] = {{40, L'T'}, {30, L'G'}, {20, L'M'}, {10, L'K'}};
    for (const auto& unit : kUnits) {
        if (bytes != 0 && bytes % (uint64_t{1} << unit.shift) == 0) {
            return std::format(L"{}{}", bytes >> unit.shift, unit.unit);
        }
    }
    return std::to_wstring(bytes);
}

std::optional<uint32_t> ParseIpv4(std::wstring_view text)
{
    // inet_addr is available on XP (inet_pton is not); reject its quirks
    // (e.g. "1") by requiring four dotted parts.
    if (std::count(text.begin(), text.end(), L'.') != 3 ||
        std::any_of(text.begin(), text.end(), [](wchar_t c) { return c != L'.' && (c < L'0' || c > L'9'); })) {
        return std::nullopt;
    }
    const std::string ascii = util::ToUtf8(text);
    const unsigned long address = inet_addr(ascii.c_str());
    if (address == INADDR_NONE && ascii != "255.255.255.255") {
        return std::nullopt;
    }
    return static_cast<uint32_t>(address);
}

std::optional<util::Severity> ParseVerbosity(std::wstring_view text)
{
    for (util::Severity severity : {util::Severity::Error, util::Severity::Warning, util::Severity::Information,
                                    util::Severity::Verbose, util::Severity::Debug}) {
        if (text == VerbosityName(severity)) {
            return severity;
        }
    }
    return std::nullopt;
}

std::wstring VerbosityName(util::Severity severity)
{
    switch (severity) {
    case util::Severity::Critical:
    case util::Severity::Error:
        return L"error";
    case util::Severity::Warning:
        return L"warning";
    case util::Severity::Information:
        return L"info";
    case util::Severity::Verbose:
        return L"verbose";
    case util::Severity::Debug:
    case util::Severity::Trace:
        return L"debug";
    }
    return L"info";
}

// ---- Stable names --------------------------------------------------------------

bool MayAppearLater(const disk::OpenError& error)
{
    switch (error.problem) {
    case disk::OpenError::Problem::NoSuchDevice:
    case disk::OpenError::Problem::NoSuchVolume:
    case disk::OpenError::Problem::VolumeFailed:
        return true;
    case disk::OpenError::Problem::None:
        switch (error.code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_NOT_READY:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_UNRECOGNIZED_VOLUME:
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

bool HasStableName(const DiskSpec& spec)
{
    switch (spec.kind) {
    case DiskSpec::Kind::PhysicalDisk:
        return disk::IsValidDiskSelector(spec.path) && !IsNumber(spec.path);
    case DiskSpec::Kind::Volume:
        return disk::IsStableVolumeSelector(spec.path);
    case DiskSpec::Kind::DynamicVolume:
        return ldm::ParseDynamicVolumeGuid(spec.path).has_value();
    case DiskSpec::Kind::Image:
    case DiskSpec::Kind::RamDisk:
        break;
    }
    return true;
}

bool UseStableName(DiskSpec& spec, const std::vector<std::wstring>& ldmImages)
{
    switch (spec.kind) {
    case DiskSpec::Kind::Image:
        spec.path = disk::FullPath(spec.path);
        return true;
    case DiskSpec::Kind::RamDisk:
        return true;
    case DiskSpec::Kind::PhysicalDisk: {
        if (HasStableName(spec)) {
            return true;
        }
        const std::vector<disk::PhysicalDiskInfo> disks = disk::ListPhysicalDisks();
        uint32_t index = 0;
        if (disk::ResolveDiskSelector(spec.path, disks, index) != disk::SelectorMatch::Found) {
            return false;
        }
        for (const disk::PhysicalDiskInfo& info : disks) {
            if (info.index == index) {
                const std::vector<std::wstring> names = disk::StableDiskSelectors(info, disks);
                if (!names.empty()) {
                    spec.path = names.front();
                    return true;
                }
            }
        }
        return false;
    }
    case DiskSpec::Kind::Volume: {
        if (HasStableName(spec)) {
            return true;
        }
        const std::optional<std::wstring> volumeName = disk::ResolveVolumeName(spec.path);
        if (!volumeName) {
            return false;
        }
        const std::vector<disk::PhysicalDiskInfo> disks = disk::ListPhysicalDisks();
        const std::vector<disk::VolumeInfo> volumes = disk::ListVolumes();
        for (const disk::VolumeInfo& volume : volumes) {
            if (EqualsIgnoreCase(volume.volumeName, *volumeName)) {
                if (const std::optional<std::wstring> name = disk::StableVolumeSelector(volume, disks, volumes)) {
                    spec.path = *name;
                    return true;
                }
            }
        }
        // A dynamic volume Windows has: its volume GUID is the LDM volume GUID,
        // which stays the same everywhere.
        std::vector<ldm::DynamicVolumeInfo> dynamic;
        std::wstring failedPath;
        if (ldm::ListDynamicVolumes({}, dynamic, failedPath) == ERROR_SUCCESS) {
            for (const ldm::DynamicVolumeInfo& info : dynamic) {
                if (EqualsIgnoreCase(info.windowsVolumeName, *volumeName)) {
                    spec.kind = DiskSpec::Kind::DynamicVolume;
                    spec.path = GuidName(info.guid);
                    return true;
                }
            }
        }
        return false;
    }
    case DiskSpec::Kind::DynamicVolume: {
        std::optional<ldm::Guid> guid = ldm::ParseDynamicVolumeGuid(spec.path);
        std::vector<ldm::DynamicVolumeInfo> dynamic;
        std::wstring failedPath;
        if (!guid && ldm::ListDynamicVolumes(ldmImages, dynamic, failedPath) == ERROR_SUCCESS) {
            std::optional<std::wstring> volumeName;
            if (ldmImages.empty() && spec.path.find_first_of(L":\\") != std::wstring::npos) {
                volumeName = disk::ResolveVolumeName(spec.path);
            }
            size_t matches = 0;
            for (const ldm::DynamicVolumeInfo& info : dynamic) {
                const std::wstring name(info.name.begin(), info.name.end());
                if (volumeName ? EqualsIgnoreCase(info.windowsVolumeName, *volumeName) : EqualsIgnoreCase(name, spec.path)) {
                    guid = info.guid;
                    ++matches;
                }
            }
            if (matches != 1) {
                guid.reset();
            }
        }
        if (!guid) {
            return false;
        }
        spec.path = GuidName(*guid);
        return true;
    }
    }
    return false;
}

bool IsStableSptiName(const std::wstring& device)
{
    return device.size() > 7 && EqualsIgnoreCase(std::wstring_view(device).substr(0, 7), L"serial:");
}

bool UseStableSptiName(std::wstring& device)
{
    if (IsStableSptiName(device)) {
        return true;
    }
    const std::vector<disk::ScsiDeviceInfo> devices = disk::ListScsiDevices();
    for (const disk::ScsiDeviceInfo& info : devices) {
        if (EqualsIgnoreCase(info.path, device)) {
            const std::vector<std::wstring> names = disk::StableScsiSelectors(info, devices);
            if (!names.empty()) {
                device = names.front();
                return true;
            }
        }
    }
    return false;
}

} // namespace tiscsi::config
