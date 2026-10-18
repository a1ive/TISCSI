// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Command-line options of TISCSICli.

#pragma once

#include "Core/config/ServerConfig.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tiscsi::cli {

struct Options {
    config::ServerConfig server;
    bool help = false;
    bool list = false; // --list: show disks, volumes and SCSI devices

    // --create-vhd <path> --size <size> [--dynamic]
    std::wstring createVhdPath;
    uint64_t createSize = 0;
    bool createDynamic = false;
};

// Value of --lang, looked up before anything is printed.
std::wstring FindLanguageOption(int argc, wchar_t** argv);

// Returns a localized error message, or std::nullopt on success.
std::optional<std::wstring> ParseOptions(int argc, wchar_t** argv, Options& options);

// "512M", "2G", "1048576" (bytes); binary units K, M, G, T with optional "B"/"iB".
std::optional<uint64_t> ParseSize(std::wstring_view text);

} // namespace tiscsi::cli
