// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "ConfigFiles.h"

#include "Common/I18n.h"
#include "Common/Text.h"
#include "Core/config/ConfigFile.h"
#include "Core/scsi/SptiTarget.h"
#include "UiUtil.h"
#include "resource.h"

#include <format>

namespace tiscsi {
namespace {

std::wstring ConfigErrorText(const config::ConfigError& error)
{
    using Code = config::ConfigError::Code;
    switch (error.code) {
    case Code::None:
    case Code::NoDisks:
        break;
    case Code::UnknownOption:
        return i18n::Format(IDS_ERR_CONFIG_UNKNOWN, error.option);
    case Code::MissingValue:
    case Code::InvalidSize:
    case Code::InvalidPort:
    case Code::InvalidAddress:
    case Code::InvalidLevel:
    case Code::InvalidWait:
    case Code::InvalidSwitch:
    case Code::InvalidDisk:
    case Code::InvalidDeviceName:
        return i18n::Format(IDS_ERR_CONFIG_VALUE, error.option, error.value);
    case Code::InvalidTargetName:
        return i18n::Format(IDS_ERR_CONFIG_TARGET_NAME, error.value);
    case Code::DuplicateTarget:
        return i18n::Format(IDS_ERR_TARGET_EXISTS, error.value);
    case Code::SptiMixed:
        return i18n::Format(IDS_ERR_CONFIG_SPTI_MIXED, error.value);
    case Code::NotInTarget:
        return i18n::Format(IDS_ERR_CONFIG_NOT_IN_TARGET, error.option);
    case Code::NotInServer:
        return i18n::Format(IDS_ERR_CONFIG_NOT_IN_SERVER, error.option);
    case Code::InvalidLine:
        return i18n::Format(IDS_ERR_CONFIG_INVALID_LINE, error.value);
    case Code::UnknownSection:
        return i18n::Format(IDS_ERR_CONFIG_UNKNOWN_SECTION, error.value);
    case Code::TargetWithoutDisks:
        return i18n::Format(IDS_ERR_CONFIG_EMPTY_TARGET, error.value);
    case Code::LdmUnused:
        return i18n::Str(IDS_ERR_CONFIG_LDM_UNUSED);
    }
    return {};
}

// Opens a target with all of its disks; otherwise returns why not.
std::wstring OpenTarget(App& app, const config::TargetSpec& spec, const std::vector<std::wstring>& ldmImages)
{
    std::shared_ptr<iscsi::IscsiTarget> target;
    size_t lunCount = 0;
    if (!spec.sptiDevice.empty()) {
        std::wstring path;
        disk::OpenError error;
        if (!config::ResolveSptiDevice(spec.sptiDevice, path, error)) {
            return ui::ExplainOpenError(error, true);
        }
        DWORD code = 0;
        auto spti = scsi::SptiTarget::Open(path, app.SptiLogger(), code);
        if (!spti) {
            error.Set(code, path);
            return ui::ExplainOpenError(error, true);
        }
        lunCount = spti->LunCount();
        target = std::make_shared<iscsi::IscsiTarget>(spec.name, std::move(spti));
    } else {
        std::vector<std::unique_ptr<disk::Disk>> disks;
        for (const config::DiskSpec& diskSpec : spec.disks) {
            disk::OpenError error;
            std::unique_ptr<disk::Disk> disk = config::OpenDisk(diskSpec, ldmImages, error);
            if (!disk) {
                return diskSpec.kind == config::DiskSpec::Kind::RamDisk
                           ? i18n::Format(IDS_ERR_RAM_ALLOC, text::FormatSize(diskSpec.size))
                           : ui::ExplainOpenError(error, diskSpec.kind != config::DiskSpec::Kind::Image);
            }
            disks.push_back(std::move(disk));
        }
        lunCount = disks.size();
        target = std::make_shared<iscsi::IscsiTarget>(spec.name, std::move(disks), app.ScsiLogger());
    }
    if (!app.AddTarget(std::move(target), lunCount, spec)) {
        return i18n::Format(IDS_ERR_TARGET_EXISTS, text::Utf8ToWide(spec.name));
    }
    return {};
}

} // namespace

bool ReadConfiguration(HWND owner, const std::wstring& path, config::ServerConfig& config)
{
    config::ConfigError error;
    if (const DWORD code = config::LoadConfigFile(path, config, error)) {
        ui::ShowError(owner, i18n::Format(IDS_ERR_CONFIG_READ, path, text::ErrorMessage(code)));
        return false;
    }
    if (error) {
        ui::ShowError(owner, error.line ? i18n::Format(IDS_ERR_CONFIG_LINE, path, error.line, ConfigErrorText(error))
                                        : i18n::Format(IDS_ERR_CONFIG_FILE, path, ConfigErrorText(error)));
        return false;
    }
    return true;
}

void ApplyConfiguration(HWND owner, App& app, const config::ServerConfig& config)
{
    while (!app.Targets().empty() && app.RemoveTarget(app.Targets().size() - 1)) {
    }
    std::wstring failures;
    for (const config::TargetSpec& spec : config.targets) {
        const std::wstring reason = OpenTarget(app, spec, config.ldmImages);
        if (!reason.empty()) {
            failures += std::format(L"{}{}:\n{}", failures.empty() ? L"" : L"\n\n", text::Utf8ToWide(spec.name), reason);
        }
    }
    app.addressIndex = config.address == 0 ? 0 : 1;
    app.addressText = config.addressText;
    app.port = config.port;
    app.settings = config;
    app.settings.targets.clear();
    if (!failures.empty()) {
        ui::ShowError(owner, i18n::Format(IDS_ERR_CONFIG_TARGETS, failures));
    }
}

bool WriteConfiguration(HWND owner, const App& app, const std::wstring& listen, uint16_t port,
                        const std::wstring& path)
{
    config::ServerConfig config = app.settings;
    config.addressText = listen.empty() ? L"0.0.0.0" : listen;
    config.port = port;
    std::wstring unstable;
    for (const App::TargetEntry& entry : app.Targets()) {
        config.targets.push_back(entry.spec);
        if (!entry.spec.sptiDevice.empty() && !config::IsStableSptiName(entry.spec.sptiDevice)) {
            unstable += L"  " + entry.spec.sptiDevice + L"\n";
        }
        for (const config::DiskSpec& disk : entry.spec.disks) {
            if (config::HasStableName(disk)) {
                continue;
            }
            UINT description = IDS_DESC_VOLUME;
            if (disk.kind == config::DiskSpec::Kind::PhysicalDisk) {
                description = IDS_DESC_PHYSICAL_DISK;
            } else if (disk.kind == config::DiskSpec::Kind::DynamicVolume) {
                description = IDS_DESC_DYNAMIC_VOLUME;
            }
            unstable += L"  " + i18n::Format(description, disk.path) + L"\n";
        }
    }
    if (!unstable.empty() && !ui::Confirm(owner, i18n::Format(IDS_CONFIRM_UNSTABLE, unstable))) {
        return false;
    }
    if (const DWORD error = config::SaveConfigFile(path, config)) {
        ui::ShowError(owner, i18n::Format(IDS_ERR_CONFIG_SAVE, path, text::ErrorMessage(error)));
        return false;
    }
    return true;
}

} // namespace tiscsi
