// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive

#include "Core/iscsi/Session.h"

#include "Core/iscsi/IscsiName.h"

#include <algorithm>
#include <format>

namespace tiscsi::iscsi {

Session::Session(std::string initiatorName_, uint64_t isid_, uint16_t tsih_)
    : initiatorName(std::move(initiatorName_)), isid(isid_), tsih(tsih_)
{
}

uint32_t Session::NextTransferTag()
{
    // There are no protocol requirements on the value, except that 0xFFFFFFFF is reserved.
    uint32_t tag;
    do {
        tag = m_nextTransferTag.fetch_add(1);
    } while (tag == kReservedTag);
    return tag;
}

bool Session::IsPrecedingCommandPending(uint32_t cmdSN) const
{
    return std::any_of(commandsInTransfer.begin(), commandsInTransfer.end(),
                       [cmdSN](uint32_t pending) { return IsCmdSNPreceding(pending, cmdSN); });
}

std::vector<std::shared_ptr<ScsiCommand>> Session::TakeDelayedCommandsReadyForExecution()
{
    std::vector<std::shared_ptr<ScsiCommand>> result;
    if (commandsInTransfer.empty()) {
        result.swap(delayedCommands);
        return result;
    }
    // Commands preceding the earliest command still in transfer may run.
    uint32_t earliest = commandsInTransfer.front();
    for (uint32_t cmdSN : commandsInTransfer) {
        if (IsCmdSNPreceding(cmdSN, earliest)) {
            earliest = cmdSN;
        }
    }
    auto ready = std::stable_partition(delayedCommands.begin(), delayedCommands.end(),
                                       [earliest](const auto& c) { return IsCmdSNPreceding(c->cmdSN, earliest); });
    result.assign(delayedCommands.begin(), ready);
    delayedCommands.erase(delayedCommands.begin(), ready);
    return result;
}

std::string Session::Identifier() const { return std::format("{},ISID={:x},TSIH={:x}", initiatorName, isid, tsih); }

std::shared_ptr<Session> SessionManager::StartSession(const std::string& initiatorName, uint64_t isid)
{
    std::lock_guard lock(m_mutex);
    auto session = std::make_shared<Session>(initiatorName, isid, NextTsih());
    m_sessions.push_back(session);
    return session;
}

std::shared_ptr<Session> SessionManager::FindSession(const std::string& initiatorName, uint64_t isid,
                                                     uint16_t tsih) const
{
    std::lock_guard lock(m_mutex);
    for (const auto& session : m_sessions) {
        if (EqualsIgnoreCase(session->initiatorName, initiatorName) && session->isid == isid &&
            session->tsih == tsih) {
            return session;
        }
    }
    return nullptr;
}

std::shared_ptr<Session> SessionManager::FindSession(const std::string& initiatorName, uint64_t isid,
                                                     const std::string& targetName) const
{
    std::lock_guard lock(m_mutex);
    for (const auto& session : m_sessions) {
        if (EqualsIgnoreCase(session->initiatorName, initiatorName) && session->isid == isid && session->target &&
            EqualsIgnoreCase(session->target->Name(), targetName)) {
            return session;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<Session>> SessionManager::FindTargetSessions(const std::string& targetName) const
{
    std::lock_guard lock(m_mutex);
    std::vector<std::shared_ptr<Session>> result;
    for (const auto& session : m_sessions) {
        if (session->target && EqualsIgnoreCase(session->target->Name(), targetName)) {
            result.push_back(session);
        }
    }
    return result;
}

bool SessionManager::IsTargetInUse(const std::string& targetName) const
{
    std::lock_guard lock(m_mutex);
    return std::any_of(m_sessions.begin(), m_sessions.end(), [&](const auto& session) {
        return session->target && EqualsIgnoreCase(session->target->Name(), targetName);
    });
}

void SessionManager::SetTarget(Session& session, std::shared_ptr<IscsiTarget> target)
{
    std::lock_guard lock(m_mutex);
    session.target = std::move(target);
}

void SessionManager::RemoveSession(const std::shared_ptr<Session>& session, SessionTerminationReason reason)
{
    std::shared_ptr<IscsiTarget> target;
    {
        std::lock_guard lock(m_mutex);
        auto it = std::find(m_sessions.begin(), m_sessions.end(), session);
        if (it == m_sessions.end()) {
            return;
        }
        target = session->target;
        m_sessions.erase(it);
    }
    // Notify outside the lock: handlers may call back into the server.
    if (target) {
        target->NotifySessionTermination(session->initiatorName, session->isid, reason);
    }
}

uint16_t SessionManager::NextTsih()
{
    // TSIH 0 is reserved for "new session" in login requests.
    const uint16_t tsih = m_nextTsih++;
    if (m_nextTsih == 0) {
        m_nextTsih = 1;
    }
    return tsih;
}

} // namespace tiscsi::iscsi
