// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/ISCSI.Server/ISCSIServer*.cs).
//
// Threading: one accept thread; per connection a receive thread (parses and
// processes PDUs) and a sender thread (drains the send queue); one worker
// thread per target executes SCSI commands; one housekeeping thread sends
// keep-alives and expires sessions that lost their last connection.

#pragma once

#include "Core/iscsi/Connection.h"
#include "Core/iscsi/IscsiTarget.h"
#include "Core/iscsi/Session.h"
#include "Core/util/Log.h"

#include <winsock2.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tiscsi::iscsi {

class IscsiServer {
public:
    static constexpr uint16_t kDefaultPort = 3260;
    static constexpr std::chrono::seconds kDefaultKeepAliveInterval{5 * 60};

    explicit IscsiServer(util::Logger logger);
    ~IscsiServer(); // stops the server
    IscsiServer(const IscsiServer&) = delete;
    IscsiServer& operator=(const IscsiServer&) = delete;

    // Returns false if a target with the same (case-insensitive) name exists.
    bool AddTarget(std::shared_ptr<IscsiTarget> target);
    // Fails (returns false) while an initiator is logged in to the target.
    bool RemoveTarget(const std::string& targetName);
    bool IsTargetInUse(const std::string& targetName) const;
    std::vector<std::shared_ptr<IscsiTarget>> Targets() const;

    // `address` is an IPv4 address in network byte order (INADDR_ANY for all
    // interfaces). Returns 0 or a Winsock error code.
    int Start(uint32_t address, uint16_t port = kDefaultPort,
              std::optional<std::chrono::seconds> keepAliveInterval = kDefaultKeepAliveInterval);
    void Stop();
    bool IsRunning() const { return m_listening; }

    // Terminates every connection and session of the target.
    void ResetTarget(const std::string& targetName);

private:
    using ConnectionPtr = std::shared_ptr<Connection>;
    using SessionPtr = std::shared_ptr<Session>;

    // IscsiServer.cpp: network and threads
    void AcceptLoop();
    void ReceiveLoop(ConnectionPtr connection);
    void SendLoop(ConnectionPtr connection);
    void HousekeepingLoop(std::optional<std::chrono::seconds> keepAliveInterval);
    bool ReceivePdu(const ConnectionPtr& connection, Pdu& pdu);
    void HandleConnectionTermination(const ConnectionPtr& connection);

    // IscsiServerProcessing.cpp: full feature phase
    void ProcessPdu(Pdu&& pdu, const ConnectionPtr& connection);
    bool ValidateCommandNumbering(const Pdu& pdu, const ConnectionPtr& connection);
    void ProcessScsiCommand(ScsiCommand&& command, const ConnectionPtr& connection);
    void ProcessDataOut(ScsiDataOut&& dataOut, const Pdu& header, const ConnectionPtr& connection);
    void ExecuteCommands(const std::vector<std::shared_ptr<ScsiCommand>>& commands, const ConnectionPtr& connection);
    TextResponse GetTextResponse(const TextRequest& request, const ConnectionPtr& connection);
    Pdu GetLogoutResponse(const LogoutRequest& request, const Pdu& header, const ConnectionPtr& connection);
    void SendReject(const ConnectionPtr& connection, RejectReason reason, const Pdu& rejected);

    // IscsiServerLogin.cpp
    LoginResponse GetLoginResponse(const LoginRequest& request, const ConnectionPtr& connection);
    LoginStatus SetUpSession(const LoginRequest& request, const util::KeyValueList& parameters,
                             const ConnectionPtr& connection);
    LoginStatus SetUpNormalSession(const LoginRequest& request, const std::string& targetName,
                                   const ConnectionPtr& connection);
    LoginResponse GetFinalLoginResponse(const LoginRequest& request, const util::KeyValueList& parameters,
                                        const ConnectionPtr& connection);

    // Caller must hold m_targetsMutex.
    std::shared_ptr<IscsiTarget> FindTargetLocked(const std::string& targetName) const;
    void TerminateSession(const SessionPtr& session, SessionTerminationReason reason);

    util::Logger m_log;
    bool m_winsockStarted = false;

    // Guards the target list; also held across login authorization and target
    // removal so a target cannot disappear in the middle of a login.
    mutable std::mutex m_targetsMutex;
    std::vector<std::shared_ptr<IscsiTarget>> m_targets;

    SessionManager m_sessionManager;
    ConnectionManager m_connectionManager;

    std::atomic<bool> m_listening{false};
    SOCKET m_listener = INVALID_SOCKET;
    std::thread m_acceptThread;

    std::thread m_housekeepingThread;
    std::mutex m_housekeepingMutex;
    std::condition_variable m_housekeepingWake;
    bool m_stopHousekeeping = false;
    struct PendingExpiration {
        SessionPtr session;
        std::chrono::steady_clock::time_point deadline;
    };
    std::vector<PendingExpiration> m_pendingExpirations; // guarded by m_housekeepingMutex

    // Every accepted connection until its receive thread exits.
    std::mutex m_liveMutex;
    std::condition_variable m_liveChanged;
    std::vector<ConnectionPtr> m_liveConnections;
};

} // namespace tiscsi::iscsi
