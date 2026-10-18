// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Options.h"

#include "Common/I18n.h"
#include "Console.h"
#include "Core/disk/Vhd.h"
#include "Service.h"
#include "resource.h"

#include <algorithm>

namespace tiscsi::cli {
namespace {

using config::ConfigError;

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

// Value of the first `option`, or empty.
std::wstring FindOption(int argc, wchar_t** argv, std::wstring_view option)
{
    for (int i = 1; i < argc; ++i) {
        const Argument arg = SplitArgument(argv[i]);
        if (arg.name == option) {
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

bool IsCliOptionWithValue(std::wstring_view name)
{
    for (std::wstring_view option :
         {L"--target", L"--lang", L"--create-vhd", L"--size", L"--config", L"--save-config", L"--service-name"}) {
        if (name == option) {
            return true;
        }
    }
    return false;
}

std::optional<std::wstring> LoadConfig(const std::wstring& path, Options& options)
{
    ConfigError error;
    if (const DWORD code = config::LoadConfigFile(path, options.server, error)) {
        return i18n::Format(IDS_CLI_ERR_CONFIG_READ, path, ErrorMessage(code));
    }
    if (error) {
        return error.line ? i18n::Format(IDS_CLI_ERR_CONFIG_LINE, path, error.line, ConfigErrorMessage(error))
                          : i18n::Format(IDS_CLI_ERR_CONFIG_FILE, path, ConfigErrorMessage(error));
    }
    return std::nullopt;
}

} // namespace

std::wstring FindLanguageOption(int argc, wchar_t** argv) { return FindOption(argc, argv, L"--lang"); }

std::wstring ConfigErrorMessage(const ConfigError& error)
{
    using Code = ConfigError::Code;
    switch (error.code) {
    case Code::None:
        break;
    case Code::UnknownOption:
        return i18n::Format(IDS_CLI_ERR_UNKNOWN_OPTION, error.option);
    case Code::MissingValue:
        return i18n::Format(IDS_CLI_ERR_MISSING_VALUE, error.option);
    case Code::InvalidSize:
        return i18n::Format(IDS_CLI_ERR_INVALID_SIZE, error.value);
    case Code::InvalidPort:
        return i18n::Format(IDS_CLI_ERR_INVALID_PORT, error.value);
    case Code::InvalidAddress:
        return i18n::Format(IDS_CLI_ERR_INVALID_ADDRESS, error.value);
    case Code::InvalidLevel:
        return i18n::Format(IDS_CLI_ERR_INVALID_LEVEL, error.value);
    case Code::InvalidWait:
        return i18n::Format(IDS_CLI_ERR_INVALID_WAIT, error.value, config::kMaxWaitSeconds);
    case Code::InvalidSwitch:
        return i18n::Format(IDS_CLI_ERR_INVALID_SWITCH, error.option, error.value);
    case Code::InvalidDisk:
        return i18n::Format(IDS_CLI_ERR_INVALID_DISK, error.value);
    case Code::InvalidDeviceName:
        return i18n::Format(IDS_CLI_ERR_INVALID_DEVICE_NAME, error.value);
    case Code::InvalidTargetName:
        return i18n::Format(IDS_CLI_ERR_INVALID_TARGET_NAME, error.value);
    case Code::DuplicateTarget:
        return i18n::Format(IDS_CLI_ERR_DUPLICATE_TARGET, error.value);
    case Code::SptiMixed:
        return i18n::Format(IDS_CLI_ERR_SPTI_MIXED, error.value);
    case Code::NotInTarget:
        return i18n::Format(IDS_CLI_ERR_CONFIG_NOT_IN_TARGET, error.option);
    case Code::NotInServer:
        return i18n::Format(IDS_CLI_ERR_CONFIG_NOT_IN_SERVER, error.option);
    case Code::InvalidLine:
        return i18n::Format(IDS_CLI_ERR_CONFIG_INVALID_LINE, error.value);
    case Code::UnknownSection:
        return i18n::Format(IDS_CLI_ERR_CONFIG_UNKNOWN_SECTION, error.value);
    case Code::NoDisks:
        return i18n::Str(IDS_CLI_ERR_NO_DISKS);
    case Code::TargetWithoutDisks:
        return i18n::Format(IDS_CLI_ERR_TARGET_WITHOUT_DISKS, error.value);
    case Code::LdmUnused:
        return i18n::Str(IDS_CLI_ERR_LDM_UNUSED);
    case Code::InvalidInitiatorName:
        return i18n::Format(IDS_CLI_ERR_INVALID_INITIATOR_NAME, error.value);
    case Code::InvalidSecret:
        return i18n::Format(IDS_CLI_ERR_INVALID_SECRET, error.option, config::kMinChapSecret, config::kMaxChapSecret);
    case Code::ChapIncomplete:
        return i18n::Format(IDS_CLI_ERR_CHAP_INCOMPLETE, error.value);
    case Code::ChapSameSecret:
        return i18n::Format(IDS_CLI_ERR_CHAP_SAME_SECRET, error.value);
    }
    return {};
}

std::optional<std::wstring> ParseOptions(int argc, wchar_t** argv, Options& options)
{
    // The file comes first; server options on the command line override it.
    options.configFile = FindOption(argc, argv, L"--config");
    if (!options.configFile.empty()) {
        if (auto error = LoadConfig(options.configFile, options)) {
            return error;
        }
    }
    config::ConfigBuilder builder(options.server, false);

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
        if (arg.name == L"--skip-unavailable") {
            options.server.skipUnavailable = true;
            options.serverOptionsGiven = true;
            continue;
        }
        if (arg.name == L"--install-service" || arg.name == L"--uninstall-service" || arg.name == L"--service") {
            options.service = arg.name == L"--install-service"     ? Options::ServiceAction::Install
                              : arg.name == L"--uninstall-service" ? Options::ServiceAction::Uninstall
                                                                   : Options::ServiceAction::Run;
            continue;
        }

        const bool cliOption = IsCliOptionWithValue(arg.name);
        const std::wstring_view key = arg.name.starts_with(L"--") ? arg.name.substr(2) : std::wstring_view();
        if (!cliOption && !config::ConfigBuilder::IsServerKey(key) && !config::ConfigBuilder::IsTargetKey(key)) {
            return i18n::Format(IDS_CLI_ERR_UNKNOWN_OPTION, arg.name);
        }
        std::optional<std::wstring> value;
        if (arg.inlineValue) {
            value = std::wstring(*arg.inlineValue);
        } else if (i + 1 < argc) {
            value = std::wstring(argv[++i]);
        }
        if (!value || value->empty()) {
            return i18n::Format(IDS_CLI_ERR_MISSING_VALUE, arg.name);
        }
        if (!options.configFile.empty() && (arg.name == L"--target" || config::ConfigBuilder::IsTargetKey(key))) {
            return i18n::Str(IDS_CLI_ERR_CONFIG_MIXED);
        }

        ConfigError error;
        if (arg.name == L"--target") {
            error = builder.BeginTarget(*value);
        } else if (arg.name == L"--create-vhd") {
            options.createVhdPath = *value;
        } else if (arg.name == L"--size") {
            const std::optional<uint64_t> size = config::ParseSize(*value);
            if (!size || *size == 0 || *size % 512 != 0 || *size > disk::kMaxVhdSize) {
                return i18n::Format(IDS_CLI_ERR_VHD_SIZE, *value);
            }
            options.createSize = *size;
        } else if (arg.name == L"--save-config") {
            options.saveConfigFile = *value;
        } else if (arg.name == L"--service-name") {
            if (!IsValidServiceName(*value)) {
                return i18n::Format(IDS_CLI_ERR_INVALID_SERVICE_NAME, *value);
            }
            options.serviceName = *value;
        } else if (!cliOption) {
            error = builder.Apply(key, *value);
            options.serverOptionsGiven = options.serverOptionsGiven || config::ConfigBuilder::IsServerKey(key);
        }
        // --lang and --config were handled before.
        if (error) {
            if (!error.option.empty()) {
                error.option = arg.name;
            }
            return ConfigErrorMessage(error);
        }
    }

    if (options.serviceName.empty()) {
        options.serviceName = kDefaultServiceName;
    }
    if (options.help || options.list || options.service == Options::ServiceAction::Uninstall) {
        return std::nullopt;
    }
    if (options.service != Options::ServiceAction::None) {
        // The service serves the file and nothing else.
        if (options.configFile.empty()) {
            return i18n::Str(IDS_CLI_ERR_SERVICE_NEEDS_CONFIG);
        }
        if (options.service == Options::ServiceAction::Install &&
            (options.serverOptionsGiven || !options.saveConfigFile.empty() || !options.createVhdPath.empty())) {
            return i18n::Str(IDS_CLI_ERR_SERVICE_OPTIONS);
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
    if (const ConfigError error = config::Validate(options.server, !creating)) {
        return ConfigErrorMessage(error);
    }
    return std::nullopt;
}

} // namespace tiscsi::cli
