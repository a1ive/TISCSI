// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Application state that outlives the main dialog (which is recreated when
// the UI language changes): the server, its targets and session counts.

#pragma once

#include "Core/config/ServerConfig.h"
#include "Core/iscsi/IscsiServer.h"
#include "Core/util/Log.h"

#include <windows.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tiscsi {

// Posted to the notify window when session counts change.
constexpr UINT WM_APP_SESSIONS_CHANGED = WM_APP + 1;

class App {
public:
    explicit App(std::shared_ptr<const util::LogSink> sink);
    ~App(); // stops the server

    iscsi::IscsiServer& Server() { return m_server; }
    util::Logger ScsiLogger() const { return util::Logger("Virtual SCSI Target", m_sink); }

    util::Logger SptiLogger() const { return util::Logger("SPTI Target", m_sink); }

    struct TargetEntry {
        std::shared_ptr<iscsi::IscsiTarget> target;
        size_t lunCount = 0;
        config::TargetSpec spec; // what a saved configuration names
    };
    const std::vector<TargetEntry>& Targets() const { return m_targets; }
    bool HasTargetNamed(const std::string& name) const;
    // Registers the target with the server; false if the name is taken.
    bool AddTarget(std::shared_ptr<iscsi::IscsiTarget> target, size_t lunCount, config::TargetSpec spec);
    // False while initiators are logged in. Disks are released with the target.
    bool RemoveTarget(size_t index);

    int SessionCount(const std::string& targetName) const;
    int TotalSessions() const;

    void SetNotifyWindow(HWND window) { m_notifyWindow = window; }

    // Kept here so they survive recreating the main dialog.
    bool running = false;
    int addressIndex = 0;
    std::wstring addressText;
    uint16_t port = iscsi::IscsiServer::kDefaultPort;
    unsigned nextTargetNumber = 1;
    // The last opened configuration's settings the GUI does not use itself
    // (log, verbosity, wait, ldm-disk, ...), kept so that saving keeps them.
    // `targets` stays empty.
    config::ServerConfig settings;
    // --config: opened, and the server started, once the window is shown.
    std::wstring startupConfig;

private:
    void OnSessionDelta(const std::string& targetName, int delta);

    std::shared_ptr<const util::LogSink> m_sink;
    std::vector<TargetEntry> m_targets;
    mutable std::mutex m_sessionsMutex;
    std::map<std::string, int> m_sessions; // by lower-cased target name
    std::atomic<HWND> m_notifyWindow{nullptr};
    // Declared last: destroyed (stopped) first, while the rest is still valid.
    iscsi::IscsiServer m_server;
};

} // namespace tiscsi
