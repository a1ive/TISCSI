// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Options.h"

#include "Common/I18n.h"
#include "Console.h"
#include "Core/disk/DeviceSelector.h"
#include "Core/disk/Vhd.h"
#include "Core/iscsi/IscsiName.h"
#include "resource.h"

#include <winsock2.h>

#include <algorithm>
#include <charconv>
#include <cwctype>
#include <format>

namespace tiscsi::cli {
namespace {

using config::DiskSpec;
using config::TargetSpec;

struct Argument {
    std::wstring_view name;  // "--image"
    std::optional<std::wstring_view> inlineValue; // from "--image=path"
};

Argument SplitArgument(std::wstring_view arg)
{
    const size_t equals = arg.find(L'=');
    if (arg.starts_with(L"--") && equals != std::wstring_view::npos) {
        return {arg.substr(0, equals), arg.substr(equals + 1)};
    }
    return {arg, std::nullopt};
}

std::optional<util::Severity> ParseVerbosity(std::wstring_view text)
{
    if (text == L"error") {
        return util::Severity::Error;
    }
    if (text == L"warning") {
        return util::Severity::Warning;
    }
    if (text == L"info") {
        return util::Severity::Information;
    }
    if (text == L"verbose") {
        return util::Severity::Verbose;
    }
    if (text == L"debug") {
        return util::Severity::Debug;
    }
    return std::nullopt;
}

std::optional<uint32_t> ParseIpv4(std::wstring_view text)
{
    // inet_addr is available on XP (inet_pton is not); reject its quirks
    // (e.g. "1") by requiring four dotted parts.
    if (std::count(text.begin(), text.end(), L'.') != 3) {
        return std::nullopt;
    }
    const std::string ascii = WideToUtf8(text);
    const unsigned long address = inet_addr(ascii.c_str());
    if (address == INADDR_NONE && ascii != "255.255.255.255") {
        return std::nullopt;
    }
    return static_cast<uint32_t>(address);
}

TargetSpec& CurrentTarget(Options& options)
{
    if (options.server.targets.empty()) {
        options.server.targets.push_back({std::format("{}:target1", iscsi::kDefaultTargetNamePrefix), {}, {}});
    }
    return options.server.targets.back();
}

// A pass-through target serves one device and nothing else.
std::optional<std::wstring> AddDisk(Options& options, DiskSpec disk)
{
    TargetSpec& target = CurrentTarget(options);
    if (!target.sptiDevice.empty()) {
        return i18n::Format(IDS_CLI_ERR_SPTI_MIXED, Utf8ToWide(target.name));
    }
    target.disks.push_back(std::move(disk));
    return std::nullopt;
}

} // namespace

std::wstring FindLanguageOption(int argc, wchar_t** argv)
{
    for (int i = 1; i < argc; ++i) {
        const Argument arg = SplitArgument(argv[i]);
        if (arg.name == L"--lang") {
            if (arg.inlineValue) {
                return std::wstring(*arg.inlineValue);
            }
            if (i + 1 < argc) {
                return argv[i + 1];
            }
        }
    }
    return {};
}

std::optional<std::wstring> ParseOptions(int argc, wchar_t** argv, Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const Argument arg = SplitArgument(argv[i]);
        if (arg.name == L"--help" || arg.name == L"-h" || arg.name == L"/?") {
            options.help = true;
            continue;
        }
        if (arg.name == L"--dynamic") {
            options.createDynamic = true;
            continue;
        }
        if (arg.name == L"--list") {
            options.list = true;
            continue;
        }

        auto takeValue = [&]() -> std::optional<std::wstring> {
            if (arg.inlineValue) {
                return std::wstring(*arg.inlineValue);
            }
            if (i + 1 < argc) {
                return std::wstring(argv[++i]);
            }
            return std::nullopt;
        };
        const bool knownWithValue = arg.name == L"--target" || arg.name == L"--image" || arg.name == L"--image-ro" ||
                                    arg.name == L"--cdrom" || arg.name == L"--ram" || arg.name == L"--listen" || arg.name == L"--port" ||
                                    arg.name == L"--log" || arg.name == L"--verbosity" || arg.name == L"--lang" ||
                                    arg.name == L"--create-vhd" || arg.name == L"--size" ||
                                    arg.name == L"--disk" || arg.name == L"--disk-ro" || arg.name == L"--volume" ||
                                    arg.name == L"--volume-ro" || arg.name == L"--spti" ||
                                    arg.name == L"--dynamic-volume" || arg.name == L"--dynamic-volume-ro" ||
                                    arg.name == L"--ldm-disk";
        if (!knownWithValue) {
            return i18n::Format(IDS_CLI_ERR_UNKNOWN_OPTION, arg.name);
        }
        const std::optional<std::wstring> value = takeValue();
        if (!value || value->empty()) {
            return i18n::Format(IDS_CLI_ERR_MISSING_VALUE, arg.name);
        }

        if (arg.name == L"--target") {
            const std::string name = WideToUtf8(*value);
            if (!iscsi::IsValidIscsiName(name)) {
                return i18n::Format(IDS_CLI_ERR_INVALID_TARGET_NAME, *value);
            }
            for (const TargetSpec& target : options.server.targets) {
                if (iscsi::EqualsIgnoreCase(target.name, name)) {
                    return i18n::Format(IDS_CLI_ERR_DUPLICATE_TARGET, *value);
                }
            }
            options.server.targets.push_back({name, {}, {}});
        } else if (arg.name == L"--image" || arg.name == L"--image-ro" || arg.name == L"--cdrom") {
            DiskSpec disk;
            disk.kind = DiskSpec::Kind::Image;
            disk.path = *value;
            disk.optical = arg.name == L"--cdrom";
            disk.readOnly = arg.name != L"--image";
            if (auto error = AddDisk(options, std::move(disk))) {
                return error;
            }
        } else if (arg.name == L"--ram") {
            const std::optional<uint64_t> size = ParseSize(*value);
            if (!size || *size < 512) {
                return i18n::Format(IDS_CLI_ERR_INVALID_SIZE, *value);
            }
            DiskSpec disk;
            disk.kind = DiskSpec::Kind::RamDisk;
            disk.size = *size;
            if (auto error = AddDisk(options, std::move(disk))) {
                return error;
            }
        } else if (arg.name == L"--disk" || arg.name == L"--disk-ro") {
            if (!disk::IsValidDiskSelector(*value)) {
                return i18n::Format(IDS_CLI_ERR_INVALID_DISK, *value);
            }
            DiskSpec disk;
            disk.kind = DiskSpec::Kind::PhysicalDisk;
            disk.path = *value;
            disk.readOnly = arg.name == L"--disk-ro";
            if (auto error = AddDisk(options, std::move(disk))) {
                return error;
            }
        } else if (arg.name == L"--volume" || arg.name == L"--volume-ro") {
            if (!disk::IsValidVolumeSelector(*value)) {
                return i18n::Format(IDS_CLI_ERR_INVALID_DEVICE_NAME, *value);
            }
            DiskSpec disk;
            disk.kind = DiskSpec::Kind::Volume;
            disk.path = *value;
            disk.readOnly = arg.name == L"--volume-ro";
            if (auto error = AddDisk(options, std::move(disk))) {
                return error;
            }
        } else if (arg.name == L"--dynamic-volume" || arg.name == L"--dynamic-volume-ro") {
            DiskSpec disk;
            disk.kind = DiskSpec::Kind::DynamicVolume;
            disk.path = *value;
            disk.readOnly = arg.name == L"--dynamic-volume-ro";
            if (auto error = AddDisk(options, std::move(disk))) {
                return error;
            }
        } else if (arg.name == L"--ldm-disk") {
            options.server.ldmImages.push_back(*value);
        } else if (arg.name == L"--spti") {
            if (!disk::IsValidScsiSelector(*value)) {
                return i18n::Format(IDS_CLI_ERR_INVALID_DEVICE_NAME, *value);
            }
            TargetSpec& target = CurrentTarget(options);
            if (!target.disks.empty() || !target.sptiDevice.empty()) {
                return i18n::Format(IDS_CLI_ERR_SPTI_MIXED, Utf8ToWide(target.name));
            }
            target.sptiDevice = *value;
        } else if (arg.name == L"--listen") {
            const std::optional<uint32_t> address = ParseIpv4(*value);
            if (!address) {
                return i18n::Format(IDS_CLI_ERR_INVALID_ADDRESS, *value);
            }
            options.server.address = *address;
            options.server.addressText = *value;
        } else if (arg.name == L"--port") {
            unsigned port = 0;
            const std::string text = WideToUtf8(*value);
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), port);
            if (ec != std::errc() || end != text.data() + text.size() || port == 0 || port > 65535) {
                return i18n::Format(IDS_CLI_ERR_INVALID_PORT, *value);
            }
            options.server.port = static_cast<uint16_t>(port);
        } else if (arg.name == L"--create-vhd") {
            options.createVhdPath = *value;
        } else if (arg.name == L"--size") {
            const std::optional<uint64_t> size = ParseSize(*value);
            if (!size || *size == 0 || *size % 512 != 0 || *size > disk::kMaxVhdSize) {
                return i18n::Format(IDS_CLI_ERR_VHD_SIZE, *value);
            }
            options.createSize = *size;
        } else if (arg.name == L"--log") {
            options.server.logFile = *value;
        } else if (arg.name == L"--verbosity") {
            const std::optional<util::Severity> level = ParseVerbosity(*value);
            if (!level) {
                return i18n::Format(IDS_CLI_ERR_INVALID_LEVEL, *value);
            }
            options.server.verbosity = *level;
        }
        // --lang was handled by FindLanguageOption.
    }

    if (options.help || options.list) {
        return std::nullopt;
    }
    if (!options.server.ldmImages.empty()) {
        const bool servesDynamicVolume =
            std::any_of(options.server.targets.begin(), options.server.targets.end(), [](const TargetSpec& target) {
                return std::any_of(target.disks.begin(), target.disks.end(),
                                   [](const DiskSpec& disk) { return disk.kind == DiskSpec::Kind::DynamicVolume; });
            });
        if (!servesDynamicVolume) {
            return i18n::Str(IDS_CLI_ERR_LDM_UNUSED);
        }
    }
    const bool creating = !options.createVhdPath.empty();
    if (creating && options.createSize == 0) {
        return i18n::Str(IDS_CLI_ERR_CREATE_NEEDS_SIZE);
    }
    if (!creating && (options.createSize != 0 || options.createDynamic)) {
        return i18n::Str(IDS_CLI_ERR_SIZE_WITHOUT_CREATE);
    }
    // Creating an image alone is a complete command; serving needs disks.
    if (options.server.targets.empty() && !creating) {
        return i18n::Str(IDS_CLI_ERR_NO_DISKS);
    }
    for (const TargetSpec& target : options.server.targets) {
        if (target.disks.empty() && target.sptiDevice.empty()) {
            return i18n::Format(IDS_CLI_ERR_TARGET_WITHOUT_DISKS, Utf8ToWide(target.name));
        }
    }
    return std::nullopt;
}

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

} // namespace tiscsi::cli
