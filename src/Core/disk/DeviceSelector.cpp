// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/disk/DeviceSelector.h"

#include <algorithm>
#include <cwctype>
#include <format>

namespace tiscsi::disk {
namespace {

struct Selector {
    enum class Kind { Number, Serial, Gpt, Mbr, Other };
    Kind kind = Kind::Other;
    uint32_t number = 0;
    std::wstring serial;
    GUID guid = {};
    uint32_t signature = 0;
    std::optional<uint64_t> offset;
};

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
               return std::towupper(x) == std::towupper(y);
           });
}

bool StartsWithIgnoreCase(std::wstring_view text, std::wstring_view prefix)
{
    return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

int HexDigit(wchar_t c)
{
    if (c >= L'0' && c <= L'9') {
        return c - L'0';
    }
    if (c >= L'a' && c <= L'f') {
        return c - L'a' + 10;
    }
    if (c >= L'A' && c <= L'F') {
        return c - L'A' + 10;
    }
    return -1;
}

std::optional<uint64_t> ParseHex(std::wstring_view text, size_t maxDigits)
{
    if (text.empty() || text.size() > maxDigits) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (wchar_t c : text) {
        const int digit = HexDigit(c);
        if (digit < 0) {
            return std::nullopt;
        }
        value = value << 4 | static_cast<uint64_t>(digit);
    }
    return value;
}

std::optional<uint64_t> ParseDecimal(std::wstring_view text)
{
    if (text.empty() || text.size() > 19) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<uint64_t>(c - L'0');
    }
    return value;
}

// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", optionally in braces.
std::optional<GUID> ParseGuid(std::wstring_view text)
{
    if (text.size() == 38 && text.front() == L'{' && text.back() == L'}') {
        text = text.substr(1, 36);
    }
    if (text.size() != 36 || text[8] != L'-' || text[13] != L'-' || text[18] != L'-' || text[23] != L'-') {
        return std::nullopt;
    }
    const auto data1 = ParseHex(text.substr(0, 8), 8);
    const auto data2 = ParseHex(text.substr(9, 4), 4);
    const auto data3 = ParseHex(text.substr(14, 4), 4);
    if (!data1 || !data2 || !data3) {
        return std::nullopt;
    }
    GUID guid = {static_cast<unsigned long>(*data1), static_cast<unsigned short>(*data2),
                 static_cast<unsigned short>(*data3), {}};
    for (size_t i = 0; i < 8; ++i) {
        const size_t position = i < 2 ? 19 + i * 2 : 24 + (i - 2) * 2;
        const auto byte = ParseHex(text.substr(position, 2), 2);
        if (!byte) {
            return std::nullopt;
        }
        guid.Data4[i] = static_cast<unsigned char>(*byte);
    }
    return guid;
}

std::wstring FormatGuid(const GUID& guid)
{
    return std::format(L"{{{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}}}", guid.Data1,
                       guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                       guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

bool IsNullGuid(const GUID& guid) { return IsEqualGUID(guid, GUID{}) != FALSE; }

// Serial numbers are ASCII.
std::wstring Widen(const std::string& text)
{
    std::wstring wide;
    for (char c : text) {
        wide += static_cast<wchar_t>(static_cast<unsigned char>(c));
    }
    return wide;
}

// Kind::Other for text without a known prefix; std::nullopt for a known
// prefix with a malformed value. MBR names carry an offset if `withOffset`.
std::optional<Selector> Parse(std::wstring_view text, bool withOffset)
{
    Selector selector;
    if (StartsWithIgnoreCase(text, L"serial:")) {
        selector.kind = Selector::Kind::Serial;
        selector.serial = text.substr(7);
        if (selector.serial.empty()) {
            return std::nullopt;
        }
    } else if (StartsWithIgnoreCase(text, L"gpt:")) {
        selector.kind = Selector::Kind::Gpt;
        const std::optional<GUID> guid = ParseGuid(text.substr(4));
        if (!guid || IsNullGuid(*guid)) {
            return std::nullopt;
        }
        selector.guid = *guid;
    } else if (StartsWithIgnoreCase(text, L"mbr:")) {
        selector.kind = Selector::Kind::Mbr;
        std::wstring_view value = text.substr(4);
        const size_t colon = value.find(L':');
        if ((colon != std::wstring_view::npos) != withOffset) {
            return std::nullopt;
        }
        if (withOffset) {
            selector.offset = ParseDecimal(value.substr(colon + 1));
            if (!selector.offset) {
                return std::nullopt;
            }
            value = value.substr(0, colon);
        }
        const std::optional<uint64_t> signature = ParseHex(value, 8);
        if (!signature || *signature == 0) {
            return std::nullopt;
        }
        selector.signature = static_cast<uint32_t>(*signature);
    } else if (const std::optional<uint64_t> number = ParseDecimal(text); number && *number <= UINT32_MAX) {
        selector.kind = Selector::Kind::Number;
        selector.number = static_cast<uint32_t>(*number);
    }
    return selector;
}

bool DiskMatches(const Selector& selector, const PhysicalDiskInfo& disk)
{
    switch (selector.kind) {
    case Selector::Kind::Serial:
        return !disk.serialNumber.empty() && EqualsIgnoreCase(Widen(disk.serialNumber), selector.serial);
    case Selector::Kind::Gpt:
        return IsEqualGUID(disk.gptDiskId, selector.guid) != FALSE;
    case Selector::Kind::Mbr:
        return disk.mbrSignature == selector.signature;
    default:
        return false;
    }
}

// The basic disk and partition a volume occupies, if it is exactly one partition.
struct VolumeLocation {
    const PhysicalDiskInfo* disk = nullptr;
    const PartitionInfo* partition = nullptr;
};

std::optional<VolumeLocation> LocateVolume(const VolumeInfo& volume, const std::vector<PhysicalDiskInfo>& disks)
{
    if (volume.extents.size() != 1) {
        return std::nullopt;
    }
    const VolumeExtent& extent = volume.extents.front();
    const auto disk = std::find_if(disks.begin(), disks.end(),
                                   [&](const PhysicalDiskInfo& info) { return info.index == extent.diskNumber; });
    if (disk == disks.end() || disk->isDynamic) {
        return std::nullopt;
    }
    const auto partition = std::find_if(disk->partitions.begin(), disk->partitions.end(),
                                        [&](const PartitionInfo& info) { return info.offset == extent.offset; });
    if (partition == disk->partitions.end()) {
        return std::nullopt;
    }
    return VolumeLocation{&*disk, &*partition};
}

bool VolumeMatches(const Selector& selector, const VolumeInfo& volume, const std::vector<PhysicalDiskInfo>& disks)
{
    const std::optional<VolumeLocation> location = LocateVolume(volume, disks);
    if (!location) {
        return false;
    }
    if (selector.kind == Selector::Kind::Gpt) {
        return IsEqualGUID(location->partition->gptPartitionId, selector.guid) != FALSE;
    }
    return selector.kind == Selector::Kind::Mbr && location->disk->mbrSignature == selector.signature &&
           location->partition->offset == selector.offset;
}

} // namespace

// ---- Physical disks ----------------------------------------------------------

bool IsValidDiskSelector(std::wstring_view selector)
{
    const std::optional<Selector> parsed = Parse(selector, false);
    return parsed && parsed->kind != Selector::Kind::Other;
}

SelectorMatch ResolveDiskSelector(std::wstring_view selector, const std::vector<PhysicalDiskInfo>& disks,
                                  uint32_t& index)
{
    const std::optional<Selector> parsed = Parse(selector, false);
    if (!parsed || parsed->kind == Selector::Kind::Other) {
        return SelectorMatch::Invalid;
    }
    if (parsed->kind == Selector::Kind::Number) {
        index = parsed->number;
        return SelectorMatch::Found;
    }
    size_t matches = 0;
    for (const PhysicalDiskInfo& disk : disks) {
        if (DiskMatches(*parsed, disk)) {
            index = disk.index;
            ++matches;
        }
    }
    return matches == 0 ? SelectorMatch::NotFound : matches == 1 ? SelectorMatch::Found : SelectorMatch::Ambiguous;
}

std::vector<std::wstring> StableDiskSelectors(const PhysicalDiskInfo& disk, const std::vector<PhysicalDiskInfo>& disks)
{
    std::vector<std::wstring> candidates;
    if (!disk.serialNumber.empty()) {
        candidates.push_back(L"serial:" + Widen(disk.serialNumber));
    }
    if (!IsNullGuid(disk.gptDiskId)) {
        candidates.push_back(L"gpt:" + FormatGuid(disk.gptDiskId));
    }
    if (disk.mbrSignature != 0) {
        candidates.push_back(std::format(L"mbr:{:08X}", disk.mbrSignature));
    }
    std::vector<std::wstring> selectors;
    for (std::wstring& candidate : candidates) {
        uint32_t index = 0;
        if (ResolveDiskSelector(candidate, disks, index) == SelectorMatch::Found && index == disk.index) {
            selectors.push_back(std::move(candidate));
        }
    }
    return selectors;
}

// ---- Volumes -----------------------------------------------------------------

bool IsStableVolumeSelector(std::wstring_view selector)
{
    return StartsWithIgnoreCase(selector, L"gpt:") || StartsWithIgnoreCase(selector, L"mbr:");
}

bool IsValidVolumeSelector(std::wstring_view selector)
{
    return !selector.empty() && (!IsStableVolumeSelector(selector) || Parse(selector, true));
}

SelectorMatch ResolveVolumeSelector(std::wstring_view selector, const std::vector<PhysicalDiskInfo>& disks,
                                    const std::vector<VolumeInfo>& volumes, std::wstring& volumeName)
{
    const std::optional<Selector> parsed = Parse(selector, true);
    if (!parsed || (parsed->kind != Selector::Kind::Gpt && parsed->kind != Selector::Kind::Mbr)) {
        return SelectorMatch::Invalid;
    }
    size_t matches = 0;
    for (const VolumeInfo& volume : volumes) {
        if (VolumeMatches(*parsed, volume, disks)) {
            volumeName = volume.volumeName;
            ++matches;
        }
    }
    return matches == 0 ? SelectorMatch::NotFound : matches == 1 ? SelectorMatch::Found : SelectorMatch::Ambiguous;
}

std::optional<std::wstring> StableVolumeSelector(const VolumeInfo& volume, const std::vector<PhysicalDiskInfo>& disks,
                                                 const std::vector<VolumeInfo>& volumes)
{
    const std::optional<VolumeLocation> location = LocateVolume(volume, disks);
    if (!location) {
        return std::nullopt;
    }
    std::wstring selector;
    if (!IsNullGuid(location->partition->gptPartitionId)) {
        selector = L"gpt:" + FormatGuid(location->partition->gptPartitionId);
    } else if (location->disk->mbrSignature != 0) {
        selector = std::format(L"mbr:{:08X}:{}", location->disk->mbrSignature, location->partition->offset);
    } else {
        return std::nullopt;
    }
    std::wstring found;
    if (ResolveVolumeSelector(selector, disks, volumes, found) != SelectorMatch::Found ||
        found != volume.volumeName) {
        return std::nullopt;
    }
    return selector;
}

// ---- SCSI devices ------------------------------------------------------------

bool IsValidScsiSelector(std::wstring_view selector)
{
    return !selector.empty() && Parse(selector, false).has_value();
}

SelectorMatch ResolveScsiSelector(std::wstring_view selector, const std::vector<ScsiDeviceInfo>& devices,
                                  std::wstring& path)
{
    const std::optional<Selector> parsed = Parse(selector, false);
    if (!parsed || selector.empty()) {
        return SelectorMatch::Invalid;
    }
    if (parsed->kind != Selector::Kind::Serial) {
        path = selector;
        return SelectorMatch::Found;
    }
    size_t matches = 0;
    for (const ScsiDeviceInfo& device : devices) {
        if (!device.serialNumber.empty() && EqualsIgnoreCase(Widen(device.serialNumber), parsed->serial)) {
            path = device.path;
            ++matches;
        }
    }
    return matches == 0 ? SelectorMatch::NotFound : matches == 1 ? SelectorMatch::Found : SelectorMatch::Ambiguous;
}

std::vector<std::wstring> StableScsiSelectors(const ScsiDeviceInfo& device, const std::vector<ScsiDeviceInfo>& devices)
{
    if (device.serialNumber.empty()) {
        return {};
    }
    std::wstring selector = L"serial:" + Widen(device.serialNumber);
    std::wstring path;
    if (ResolveScsiSelector(selector, devices, path) != SelectorMatch::Found || path != device.path) {
        return {};
    }
    return {std::move(selector)};
}

} // namespace tiscsi::disk
