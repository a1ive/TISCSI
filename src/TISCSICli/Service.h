// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Running TISCSICli as a Windows service, so a configuration file is served
// from Windows startup on, before anyone logs on. The service runs
// "TISCSICli --service --config <file>" as LocalSystem.

#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace tiscsi::cli {

constexpr wchar_t kDefaultServiceName[] = L"TISCSI";

// Service names may not contain slashes (the service controller's rule).
bool IsValidServiceName(const std::wstring& name);

// --install-service: installs the service, or updates it if it exists, and
// (re)starts it. `logPath` is where the running service logs. Prints what
// happened; returns the process exit code.
int InstallService(const std::wstring& name, const std::wstring& configFile, const std::wstring& logPath,
                   bool waits);
// --uninstall-service: stops and removes the service.
int UninstallService(const std::wstring& name);

// --service: runs `run` on the service thread once the service controller
// has started the service, and sets `stopEvent` when it is asked to stop.
// The service reports itself running at once (waiting for devices is part
// of running) and stopped with `run`'s result. Returns `run`'s result, or a
// Win32 error if the process was not started by the service controller
// (ERROR_FAILED_SERVICE_CONTROLLER_CONNECT).
DWORD RunAsService(std::function<int()> run, HANDLE stopEvent);

} // namespace tiscsi::cli
