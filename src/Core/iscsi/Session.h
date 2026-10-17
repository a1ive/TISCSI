// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/ISCSI.Server/ISCSISession.cs and
// SessionManager.cs). Internal to IscsiServer.

#pragma once

#include "Core/iscsi/IscsiTarget.h"
#include "Core/iscsi/Parameters.h"
#include "Core/iscsi/Pdu.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tiscsi::iscsi {

// RFC 1982 serial number arithmetic: true if `a` comes before `b`.
inline bool IsCmdSNPreceding(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }

class Session {
public:
    Session(std::string initiatorName, uint64_t isid, uint16_t tsih);

    // Negotiated operational parameters (written during login).
    int maxConnections = defaults::kMaxConnections;
    bool initialR2T = defaults::kInitialR2T;
    bool immediateData = defaults::kImmediateData;
    int maxBurstLength = defaults::kMaxBurstLength;
    int firstBurstLength = defaults::kFirstBurstLength;
    int defaultTime2Wait = defaults::kDefaultTime2Wait;
    int defaultTime2Retain = defaults::kDefaultTime2Retain;
    int maxOutstandingR2T = defaults::kMaxOutstandingR2T;
    bool dataPduInOrder = defaults::kDataPduInOrder;
    bool dataSequenceInOrder = defaults::kDataSequenceInOrder;
    int errorRecoveryLevel = defaults::kErrorRecoveryLevel;
    uint32_t commandQueueSize = kDefaultCommandQueueSize;

    const std::string initiatorName;
    const uint64_t isid; // initiator session ID
    const uint16_t tsih; // target session identifying handle

    bool isDiscovery = false;
    bool isFullFeaturePhase = false;
    bool commandNumberingStarted = false;
    std::atomic<uint32_t> expCmdSN{0}; // also read by the sender thread

    // Set once during login via SessionManager::SetTarget; other threads read
    // it only through SessionManager.
    std::shared_ptr<IscsiTarget> target;

    // Commands waiting for Data-Out, and commands held back so execution
    // follows CmdSN order. Only touched by the session's receive thread
    // (one connection per session).
    std::vector<uint32_t> commandsInTransfer;
    std::vector<std::shared_ptr<ScsiCommand>> delayedCommands;

    uint32_t NextTransferTag();
    bool IsPrecedingCommandPending(uint32_t cmdSN) const;
    std::vector<std::shared_ptr<ScsiCommand>> TakeDelayedCommandsReadyForExecution();

    std::string Identifier() const;

private:
    std::atomic<uint32_t> m_nextTransferTag{0};
};

class SessionManager {
public:
    std::shared_ptr<Session> StartSession(const std::string& initiatorName, uint64_t isid);
    std::shared_ptr<Session> FindSession(const std::string& initiatorName, uint64_t isid, uint16_t tsih) const;
    std::shared_ptr<Session> FindSession(const std::string& initiatorName, uint64_t isid,
                                         const std::string& targetName) const;
    std::vector<std::shared_ptr<Session>> FindTargetSessions(const std::string& targetName) const;
    bool IsTargetInUse(const std::string& targetName) const;
    void SetTarget(Session& session, std::shared_ptr<IscsiTarget> target);
    // No-op if the session was already removed. Notifies the target.
    void RemoveSession(const std::shared_ptr<Session>& session, SessionTerminationReason reason);

private:
    uint16_t NextTsih();

    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<Session>> m_sessions;
    uint16_t m_nextTsih = 1;
};

} // namespace tiscsi::iscsi
