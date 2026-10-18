// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Configuration files: the command line's server options as "key = value"
// lines, in a [server] section and one [target <name>] section per target:
//
//   # TISCSI configuration
//   [server]
//   listen = 0.0.0.0
//   port = 3260
//   wait = 60
//
//   [target iqn.2026-10.example:pc01]
//   image = pc01.vhd
//   disk-ro = serial:WD-WCC4N0123456
//
// Keys are the long option names without "--" (skip-unavailable takes yes or
// no); each disk key adds the next LUN of its target. A target section may
// also restrict who logs in: allow-initiator (repeatable), chap-user,
// chap-secret, mutual-chap-user and mutual-chap-secret. Lines starting with #
// or ; are comments. Double quotes around a value keep spaces at its ends.
// Relative paths are relative to the file's folder.
//
// Files are written as UTF-8 with a BOM. UTF-8 without a BOM, UTF-16 with a
// BOM and the ANSI code page (Notepad's default on older Windows) are read.

#pragma once

#include "Core/config/ServerConfig.h"

#include <windows.h>

#include <string>
#include <string_view>

namespace tiscsi::config {

// CHAP secrets: RFC 3720 asks for at least 96 bits; the Microsoft iSCSI
// Initiator wants 12 to 16 characters.
constexpr size_t kMinChapSecret = 12;
constexpr size_t kMaxChapSecret = 255;

struct ConfigError {
    enum class Code {
        None,
        UnknownOption,     // `option`
        MissingValue,      // `option`
        InvalidSize,       // `value` of `option`, also for the following
        InvalidPort,
        InvalidAddress,
        InvalidLevel,
        InvalidWait,
        InvalidSwitch,
        InvalidDisk,
        InvalidDeviceName,
        InvalidTargetName, // `value`
        DuplicateTarget,   // `value`
        SptiMixed,         // `value`: the target
        NotInTarget,       // `option`: a disk outside a [target] section
        NotInServer,       // `option`: a server setting in a [target] section
        InvalidLine,       // `value`: the line
        UnknownSection,    // `value`: the section header
        NoDisks,
        TargetWithoutDisks, // `value`: the target
        LdmUnused,
        InvalidInitiatorName, // `value`
        InvalidSecret,      // `option`: a CHAP secret shorter than kMinChapSecret or too long
        ChapIncomplete,     // `value`: the target, with a CHAP user or mutual setting but no secret
        ChapSameSecret,     // `value`: the target, whose mutual secret is its CHAP secret
    };

    Code code = Code::None;
    std::wstring option;
    std::wstring value;
    unsigned line = 0; // 1-based line of a configuration file; 0 otherwise

    explicit operator bool() const { return code != Code::None; }
};

// Applies options one at a time, in the order the command line or a file
// gives them, checking each value.
class ConfigBuilder {
public:
    // Command line (`file` false): disks given before the first target go to
    // a target with the default name, and server options may come anywhere.
    // File: disks belong in [target] sections, server settings outside them.
    // Relative paths are resolved against `baseDirectory` if it is not empty.
    ConfigBuilder(ServerConfig& config, bool file, std::wstring baseDirectory = {});

    ConfigError BeginServer();
    ConfigError BeginTarget(std::wstring_view name);
    // `key` is an option name without "--".
    ConfigError Apply(std::wstring_view key, std::wstring_view value);

    static bool IsServerKey(std::wstring_view key);
    static bool IsTargetKey(std::wstring_view key);

private:
    enum class Section { None, Server, Target };

    ConfigError CurrentTarget(std::wstring_view key, TargetSpec*& target);
    ConfigError AddDisk(std::wstring_view key, DiskSpec disk);
    std::wstring Path(std::wstring_view value) const;

    ServerConfig& m_config;
    bool m_file;
    std::wstring m_baseDirectory;
    Section m_section = Section::None;
};

// Checks that need the whole configuration: every target serves something,
// ldm-disk is used, and, if `requireTargets`, there is a target.
ConfigError Validate(const ServerConfig& config, bool requireTargets);

// Parses configuration text into `config` (default-constructed).
ConfigError ParseConfig(std::wstring_view text, const std::wstring& baseDirectory, ServerConfig& config);
// Reads a file into `config`. Returns the Win32 error if it cannot be read;
// otherwise ERROR_SUCCESS, with `error` set for a problem in the file.
DWORD LoadConfigFile(const std::wstring& path, ServerConfig& config, ConfigError& error);

// The configuration as file text (CRLF line endings, no BOM). Paths inside
// `baseDirectory` are written relative to it.
std::wstring FormatConfig(const ServerConfig& config, const std::wstring& baseDirectory);
DWORD SaveConfigFile(const std::wstring& path, const ServerConfig& config);

} // namespace tiscsi::config
