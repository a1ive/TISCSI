// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSIConsole/Helpers/ISCSINameHelper.cs).

#pragma once

#include <string_view>

namespace tiscsi::iscsi {

// Default target name prefix used by iSCSI Console; kept so existing initiator
// configurations (e.g. iPXE sanhook scripts) keep working.
constexpr std::string_view kDefaultTargetNamePrefix = "iqn.1991-05.com.microsoft";

// "iqn.yyyy-mm.reversed.domain[:subqualifier]" or "eui.<16 hex digits>".
bool IsValidIscsiName(std::string_view name);
bool IsValidIqn(std::string_view name);
bool IsValidEui(std::string_view name);

// ASCII case-insensitive comparison; iSCSI names are compared case-insensitively.
bool EqualsIgnoreCase(std::string_view a, std::string_view b);

} // namespace tiscsi::iscsi
