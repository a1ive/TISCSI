// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "App.h"

#include "Core/iscsi/IscsiName.h"

#include <algorithm>

namespace tiscsi {
namespace {

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return text;
}

} // namespace

App::App(std::shared_ptr<const util::LogSink> sink) : m_sink(sink), m_server(util::Logger("iSCSI Server", sink)) {}

App::~App() { m_server.Stop(); }

bool App::HasTargetNamed(const std::string& name) const
{
    return std::any_of(m_targets.begin(), m_targets.end(),
                       [&](const TargetEntry& entry) { return iscsi::EqualsIgnoreCase(entry.target->Name(), name); });
}

bool App::AddTarget(std::shared_ptr<iscsi::IscsiTarget> target, size_t lunCount, config::TargetSpec spec)
{
    // Count sessions the way iSCSI Console did: a session starts when the
    // initiator is authorized and ends with the termination notification.
    const std::string name = target->Name();
    target->onAuthorizationRequest = [this, name](const std::string&, uint64_t, const std::string&) {
        OnSessionDelta(name, +1);
        return true;
    };
    target->onSessionTermination = [this, name](const std::string&, uint64_t, iscsi::SessionTerminationReason) {
        OnSessionDelta(name, -1);
    };
    if (!m_server.AddTarget(target)) {
        return false;
    }
    m_targets.push_back({std::move(target), lunCount, std::move(spec)});
    return true;
}

bool App::RemoveTarget(size_t index)
{
    if (index >= m_targets.size() || !m_server.RemoveTarget(m_targets[index].target->Name())) {
        return false;
    }
    m_targets.erase(m_targets.begin() + static_cast<ptrdiff_t>(index));
    return true;
}

int App::SessionCount(const std::string& targetName) const
{
    std::lock_guard lock(m_sessionsMutex);
    auto it = m_sessions.find(Lower(targetName));
    return it != m_sessions.end() ? it->second : 0;
}

int App::TotalSessions() const
{
    std::lock_guard lock(m_sessionsMutex);
    int total = 0;
    for (const auto& [name, count] : m_sessions) {
        total += count;
    }
    return total;
}

void App::OnSessionDelta(const std::string& targetName, int delta)
{
    // Runs on server threads.
    {
        std::lock_guard lock(m_sessionsMutex);
        int& count = m_sessions[Lower(targetName)];
        count = std::max(0, count + delta);
    }
    if (HWND window = m_notifyWindow) {
        PostMessageW(window, WM_APP_SESSIONS_CHANGED, 0, 0);
    }
}

} // namespace tiscsi
