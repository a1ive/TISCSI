// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/config/ConfigFile.h"

#include "Core/disk/DeviceSelector.h"
#include "Core/disk/File.h"
#include "Core/iscsi/IscsiName.h"

#include <algorithm>
#include <cwctype>
#include <format>
#include <vector>

namespace tiscsi::config {
namespace {

using Code = ConfigError::Code;

constexpr uint64_t kMaxFileSize = 1024 * 1024;

ConfigError Error(Code code, std::wstring_view option = {}, std::wstring_view value = {})
{
    ConfigError error;
    error.code = code;
    error.option = option;
    error.value = value;
    return error;
}

std::wstring FromUtf8(const std::string& text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
               return std::towlower(x) == std::towlower(y);
           });
}

std::wstring_view Trim(std::wstring_view text)
{
    const size_t first = text.find_first_not_of(L" \t");
    if (first == std::wstring_view::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(L" \t");
    return text.substr(first, last - first + 1);
}

std::wstring Lower(std::wstring_view text)
{
    std::wstring lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return lower;
}

std::optional<uint64_t> ParseNumber(std::wstring_view text, uint64_t maximum)
{
    if (text.empty() || text.size() > 10) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<uint64_t>(c - L'0');
    }
    return value <= maximum ? std::optional<uint64_t>(value) : std::nullopt;
}

std::optional<bool> ParseSwitch(std::wstring_view text)
{
    const std::wstring lower = Lower(text);
    if (lower == L"yes" || lower == L"true" || lower == L"on" || lower == L"1") {
        return true;
    }
    if (lower == L"no" || lower == L"false" || lower == L"off" || lower == L"0") {
        return false;
    }
    return std::nullopt;
}

std::wstring Decode(const std::vector<uint8_t>& bytes)
{
    auto convert = [](UINT codePage, DWORD flags, const char* data, int length) -> std::optional<std::wstring> {
        if (length == 0) {
            return std::wstring();
        }
        const int count = MultiByteToWideChar(codePage, flags, data, length, nullptr, 0);
        if (count == 0) {
            return std::nullopt;
        }
        std::wstring text(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(codePage, flags, data, length, text.data(), count);
        return text;
    };
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    const int length = static_cast<int>(bytes.size());
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        std::wstring text((bytes.size() - 2) / 2, L'\0');
        std::copy_n(bytes.data() + 2, text.size() * 2, reinterpret_cast<uint8_t*>(text.data()));
        return text;
    }
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        return convert(CP_UTF8, 0, data + 3, length - 3).value_or(std::wstring());
    }
    if (auto text = convert(CP_UTF8, MB_ERR_INVALID_CHARS, data, length)) {
        return *text;
    }
    return convert(CP_ACP, 0, data, length).value_or(std::wstring());
}

// `path` relative to `baseDirectory` if it lies inside it.
std::wstring RelativePath(const std::wstring& path, const std::wstring& baseDirectory)
{
    if (baseDirectory.empty()) {
        return path;
    }
    const std::wstring full = disk::FullPath(path);
    if (full.size() > baseDirectory.size() &&
        EqualsIgnoreCase(std::wstring_view(full).substr(0, baseDirectory.size()), baseDirectory)) {
        return full.substr(baseDirectory.size());
    }
    return full;
}

void AddLine(std::wstring& text, std::wstring_view key, std::wstring_view value)
{
    const bool quote = Trim(value).size() != value.size() || (value.size() >= 2 && value.front() == L'"');
    text += std::format(L"{} = {}\r\n", key, quote ? L"\"" + std::wstring(value) + L"\"" : std::wstring(value));
}

std::wstring_view DiskKey(const DiskSpec& disk)
{
    switch (disk.kind) {
    case DiskSpec::Kind::Image:
        return disk.optical ? L"cdrom" : disk.readOnly ? L"image-ro" : L"image";
    case DiskSpec::Kind::RamDisk:
        return L"ram";
    case DiskSpec::Kind::PhysicalDisk:
        return disk.readOnly ? L"disk-ro" : L"disk";
    case DiskSpec::Kind::Volume:
        return disk.readOnly ? L"volume-ro" : L"volume";
    case DiskSpec::Kind::DynamicVolume:
        return disk.readOnly ? L"dynamic-volume-ro" : L"dynamic-volume";
    }
    return L"image";
}

} // namespace

// ---- ConfigBuilder -------------------------------------------------------------

ConfigBuilder::ConfigBuilder(ServerConfig& config, bool file, std::wstring baseDirectory)
    : m_config(config), m_file(file), m_baseDirectory(std::move(baseDirectory))
{
}

bool ConfigBuilder::IsServerKey(std::wstring_view key)
{
    for (std::wstring_view name : {L"listen", L"port", L"log", L"verbosity", L"wait", L"skip-unavailable", L"ldm-disk"}) {
        if (key == name) {
            return true;
        }
    }
    return false;
}

bool ConfigBuilder::IsTargetKey(std::wstring_view key)
{
    for (std::wstring_view name : {L"image", L"image-ro", L"cdrom", L"ram", L"disk", L"disk-ro", L"volume", L"volume-ro",
                                   L"dynamic-volume", L"dynamic-volume-ro", L"spti"}) {
        if (key == name) {
            return true;
        }
    }
    return false;
}

ConfigError ConfigBuilder::BeginServer()
{
    m_section = Section::Server;
    return {};
}

ConfigError ConfigBuilder::BeginTarget(std::wstring_view name)
{
    const std::string utf8 = util::ToUtf8(name);
    if (!iscsi::IsValidIscsiName(utf8)) {
        return Error(Code::InvalidTargetName, L"target", name);
    }
    for (const TargetSpec& target : m_config.targets) {
        if (iscsi::EqualsIgnoreCase(target.name, utf8)) {
            return Error(Code::DuplicateTarget, L"target", name);
        }
    }
    m_config.targets.push_back({utf8, {}, {}});
    m_section = Section::Target;
    return {};
}

std::wstring ConfigBuilder::Path(std::wstring_view value) const
{
    return m_baseDirectory.empty() ? std::wstring(value) : disk::ResolveRelative(m_baseDirectory, std::wstring(value));
}

ConfigError ConfigBuilder::CurrentTarget(std::wstring_view key, TargetSpec*& target)
{
    if (m_section != Section::Target) {
        if (m_file) {
            return Error(Code::NotInTarget, key);
        }
        if (m_config.targets.empty()) {
            m_config.targets.push_back({std::format("{}:target1", iscsi::kDefaultTargetNamePrefix), {}, {}});
        }
    }
    target = &m_config.targets.back();
    return {};
}

// A pass-through target serves one device and nothing else.
ConfigError ConfigBuilder::AddDisk(std::wstring_view key, DiskSpec disk)
{
    TargetSpec* target = nullptr;
    if (ConfigError error = CurrentTarget(key, target)) {
        return error;
    }
    if (!target->sptiDevice.empty()) {
        return Error(Code::SptiMixed, key, FromUtf8(target->name));
    }
    target->disks.push_back(std::move(disk));
    return {};
}

ConfigError ConfigBuilder::Apply(std::wstring_view key, std::wstring_view value)
{
    const bool serverKey = IsServerKey(key);
    if (!serverKey && !IsTargetKey(key)) {
        return Error(Code::UnknownOption, key);
    }
    if (value.empty()) {
        return Error(Code::MissingValue, key);
    }
    if (serverKey && m_file && m_section == Section::Target) {
        return Error(Code::NotInServer, key);
    }

    if (key == L"listen") {
        const std::optional<uint32_t> address = ParseIpv4(value);
        if (!address) {
            return Error(Code::InvalidAddress, key, value);
        }
        m_config.address = *address;
        m_config.addressText = value;
    } else if (key == L"port") {
        const std::optional<uint64_t> port = ParseNumber(value, 65535);
        if (!port || *port == 0) {
            return Error(Code::InvalidPort, key, value);
        }
        m_config.port = static_cast<uint16_t>(*port);
    } else if (key == L"log") {
        m_config.logFile = Path(value);
    } else if (key == L"verbosity") {
        const std::optional<util::Severity> level = ParseVerbosity(value);
        if (!level) {
            return Error(Code::InvalidLevel, key, value);
        }
        m_config.verbosity = *level;
    } else if (key == L"wait") {
        const std::optional<uint64_t> seconds = ParseNumber(value, kMaxWaitSeconds);
        if (!seconds) {
            return Error(Code::InvalidWait, key, value);
        }
        m_config.waitSeconds = static_cast<uint32_t>(*seconds);
    } else if (key == L"skip-unavailable") {
        const std::optional<bool> skip = ParseSwitch(value);
        if (!skip) {
            return Error(Code::InvalidSwitch, key, value);
        }
        m_config.skipUnavailable = *skip;
    } else if (key == L"ldm-disk") {
        m_config.ldmImages.push_back(Path(value));
    } else if (key == L"image" || key == L"image-ro" || key == L"cdrom") {
        DiskSpec disk;
        disk.kind = DiskSpec::Kind::Image;
        disk.path = Path(value);
        disk.optical = key == L"cdrom";
        disk.readOnly = key != L"image";
        return AddDisk(key, std::move(disk));
    } else if (key == L"ram") {
        const std::optional<uint64_t> size = ParseSize(value);
        if (!size || *size < 512) {
            return Error(Code::InvalidSize, key, value);
        }
        DiskSpec disk;
        disk.kind = DiskSpec::Kind::RamDisk;
        disk.size = *size;
        return AddDisk(key, std::move(disk));
    } else if (key == L"disk" || key == L"disk-ro") {
        if (!disk::IsValidDiskSelector(value)) {
            return Error(Code::InvalidDisk, key, value);
        }
        DiskSpec disk;
        disk.kind = DiskSpec::Kind::PhysicalDisk;
        disk.path = value;
        disk.readOnly = key == L"disk-ro";
        return AddDisk(key, std::move(disk));
    } else if (key == L"volume" || key == L"volume-ro") {
        if (!disk::IsValidVolumeSelector(value)) {
            return Error(Code::InvalidDeviceName, key, value);
        }
        DiskSpec disk;
        disk.kind = DiskSpec::Kind::Volume;
        disk.path = value;
        disk.readOnly = key == L"volume-ro";
        return AddDisk(key, std::move(disk));
    } else if (key == L"dynamic-volume" || key == L"dynamic-volume-ro") {
        DiskSpec disk;
        disk.kind = DiskSpec::Kind::DynamicVolume;
        disk.path = value;
        disk.readOnly = key == L"dynamic-volume-ro";
        return AddDisk(key, std::move(disk));
    } else if (key == L"spti") {
        if (!disk::IsValidScsiSelector(value)) {
            return Error(Code::InvalidDeviceName, key, value);
        }
        TargetSpec* target = nullptr;
        if (ConfigError error = CurrentTarget(key, target)) {
            return error;
        }
        if (!target->disks.empty() || !target->sptiDevice.empty()) {
            return Error(Code::SptiMixed, key, FromUtf8(target->name));
        }
        target->sptiDevice = value;
    }
    return {};
}

// ---- Whole configurations ------------------------------------------------------

ConfigError Validate(const ServerConfig& config, bool requireTargets)
{
    for (const TargetSpec& target : config.targets) {
        if (target.disks.empty() && target.sptiDevice.empty()) {
            return Error(Code::TargetWithoutDisks, {}, FromUtf8(target.name));
        }
    }
    if (!config.ldmImages.empty()) {
        const bool servesDynamicVolume =
            std::any_of(config.targets.begin(), config.targets.end(), [](const TargetSpec& target) {
                return std::any_of(target.disks.begin(), target.disks.end(),
                                   [](const DiskSpec& disk) { return disk.kind == DiskSpec::Kind::DynamicVolume; });
            });
        if (!servesDynamicVolume) {
            return Error(Code::LdmUnused);
        }
    }
    if (requireTargets && config.targets.empty()) {
        return Error(Code::NoDisks);
    }
    return {};
}

ConfigError ParseConfig(std::wstring_view text, const std::wstring& baseDirectory, ServerConfig& config)
{
    ConfigBuilder builder(config, true, baseDirectory);
    unsigned number = 0;
    while (!text.empty()) {
        const size_t end = text.find(L'\n');
        std::wstring_view line = text.substr(0, end);
        text = end == std::wstring_view::npos ? std::wstring_view() : text.substr(end + 1);
        ++number;
        line = Trim(line.ends_with(L'\r') ? line.substr(0, line.size() - 1) : line);
        if (line.empty() || line.front() == L'#' || line.front() == L';') {
            continue;
        }

        ConfigError error;
        if (line.front() == L'[') {
            const std::wstring_view header = Trim(line.substr(1, line.size() - (line.back() == L']' ? 2 : 1)));
            const size_t space = header.find_first_of(L" \t");
            const std::wstring section = Lower(header.substr(0, space));
            if (line.back() != L']') {
                error = Error(Code::InvalidLine, {}, line);
            } else if (section == L"server" && space == std::wstring_view::npos) {
                error = builder.BeginServer();
            } else if (section == L"target") {
                error = builder.BeginTarget(space == std::wstring_view::npos ? std::wstring_view()
                                                                             : Trim(header.substr(space)));
            } else {
                error = Error(Code::UnknownSection, {}, line);
            }
        } else {
            const size_t equals = line.find(L'=');
            if (equals == std::wstring_view::npos) {
                error = Error(Code::InvalidLine, {}, line);
            } else {
                std::wstring_view value = Trim(line.substr(equals + 1));
                if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
                    value = value.substr(1, value.size() - 2);
                }
                error = builder.Apply(Lower(Trim(line.substr(0, equals))), value);
            }
        }
        if (error) {
            error.line = number;
            return error;
        }
    }
    return Validate(config, false);
}

DWORD LoadConfigFile(const std::wstring& path, ServerConfig& config, ConfigError& error)
{
    DWORD code = ERROR_SUCCESS;
    const std::unique_ptr<disk::File> file = disk::File::Open(path, disk::File::Access::ReadShared, code);
    if (!file) {
        return code;
    }
    uint64_t size = 0;
    if (!file->GetSize(size)) {
        return GetLastError();
    }
    if (size > kMaxFileSize) {
        return ERROR_FILE_TOO_LARGE;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!bytes.empty() && !file->ReadAt(0, bytes.data(), bytes.size())) {
        return GetLastError();
    }
    error = ParseConfig(Decode(bytes), disk::DirectoryOf(disk::FullPath(path)), config);
    return ERROR_SUCCESS;
}

std::wstring FormatConfig(const ServerConfig& config, const std::wstring& baseDirectory)
{
    std::wstring text = L"# TISCSI configuration (see TISCSICli --help)\r\n\r\n[server]\r\n";
    AddLine(text, L"listen", config.addressText.empty() ? L"0.0.0.0" : config.addressText);
    AddLine(text, L"port", std::to_wstring(config.port));
    if (!config.logFile.empty()) {
        AddLine(text, L"log", RelativePath(config.logFile, baseDirectory));
    }
    if (config.verbosity != util::Severity::Information) {
        AddLine(text, L"verbosity", VerbosityName(config.verbosity));
    }
    if (config.waitSeconds != 0) {
        AddLine(text, L"wait", std::to_wstring(config.waitSeconds));
    }
    if (config.skipUnavailable) {
        AddLine(text, L"skip-unavailable", L"yes");
    }
    for (const std::wstring& image : config.ldmImages) {
        AddLine(text, L"ldm-disk", RelativePath(image, baseDirectory));
    }
    for (const TargetSpec& target : config.targets) {
        text += std::format(L"\r\n[target {}]\r\n", FromUtf8(target.name));
        if (!target.sptiDevice.empty()) {
            AddLine(text, L"spti", target.sptiDevice);
        }
        for (const DiskSpec& disk : target.disks) {
            switch (disk.kind) {
            case DiskSpec::Kind::Image:
                AddLine(text, DiskKey(disk), RelativePath(disk.path, baseDirectory));
                break;
            case DiskSpec::Kind::RamDisk:
                AddLine(text, DiskKey(disk), FormatSize(disk.size));
                break;
            default:
                AddLine(text, DiskKey(disk), disk.path);
                break;
            }
        }
    }
    return text;
}

DWORD SaveConfigFile(const std::wstring& path, const ServerConfig& config)
{
    const std::wstring fullPath = disk::FullPath(path);
    const std::string utf8 = "\xEF\xBB\xBF" + util::ToUtf8(FormatConfig(config, disk::DirectoryOf(fullPath)));
    HANDLE file = CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    const DWORD error = ok && written == utf8.size() ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    return error;
}

} // namespace tiscsi::config
