// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Opening and saving configuration files (Core/config/ConfigFile.h) in the
// GUI. Errors are shown in message boxes.

#pragma once

#include "App.h"
#include "Core/config/ServerConfig.h"

#include <windows.h>

#include <string>

namespace tiscsi {

// Reads and checks the file; false (after showing why) if it cannot be used.
bool ReadConfiguration(HWND owner, const std::wstring& path, config::ServerConfig& config);

// Replaces the app's targets by the configuration's, which must not have
// initiators logged in. Targets whose disks cannot be opened are left out and
// listed in one message. The listening address and port go to the app's
// fields; the other settings to App::settings.
void ApplyConfiguration(HWND owner, App& app, const config::ServerConfig& config);

// Writes the app's targets with `listen` (empty for all interfaces) and
// `port`. Asks first if some devices have no stable name. False if not saved.
bool WriteConfiguration(HWND owner, const App& app, const std::wstring& listen, uint16_t port,
                        const std::wstring& path);

} // namespace tiscsi
