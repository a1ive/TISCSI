// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Server lifetime, targets, sockets and threads.

#include "Core/iscsi/IscsiServer.h"

#include "Core/iscsi/IscsiName.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <format>

#pragma comment(lib, "ws2_32.lib")

namespace tiscsi::iscsi {

using util::Severity;

namespace {

std::string FormatEndpoint(const sockaddr_in& address)
{
    return std::format("{}:{}", inet_ntoa(address.sin_addr), ntohs(address.sin_port));
}

} // namespace

IscsiServer::IscsiServer(util::Logger logger) : m_log(std::move(logger))
{
    WSADATA wsa;
    m_winsockStarted = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

IscsiServer::~IscsiServer()
{
    Stop();
    if (m_winsockStarted) {
        WSACleanup();
    }
}

bool IscsiServer::AddTarget(std::shared_ptr<IscsiTarget> target)
{
    std::lock_guard lock(m_targetsMutex);
    if (FindTargetLocked(target->Name())) {
        return false;
    }
    m_targets.push_back(std::move(target));
    return true;
}

bool IscsiServer::RemoveTarget(const std::string& targetName)
{
    // Holding the target lock across the in-use check prevents a login from
    // succeeding between the check and the removal.
    std::lock_guard lock(m_targetsMutex);
    if (m_sessionManager.IsTargetInUse(targetName)) {
        return false;
    }
    auto it = std::find_if(m_targets.begin(), m_targets.end(),
                           [&](const auto& target) { return EqualsIgnoreCase(target->Name(), targetName); });
    if (it == m_targets.end()) {
        return false;
    }
    m_targets.erase(it);
    return true;
}

bool IscsiServer::IsTargetInUse(const std::string& targetName) const
{
    return m_sessionManager.IsTargetInUse(targetName);
}

std::vector<std::shared_ptr<IscsiTarget>> IscsiServer::Targets() const
{
    std::lock_guard lock(m_targetsMutex);
    return m_targets;
}

std::shared_ptr<IscsiTarget> IscsiServer::FindTargetLocked(const std::string& targetName) const
{
    for (const auto& target : m_targets) {
        if (EqualsIgnoreCase(target->Name(), targetName)) {
            return target;
        }
    }
    return nullptr;
}

int IscsiServer::Start(uint32_t address, uint16_t port, std::optional<std::chrono::seconds> keepAliveInterval)
{
    if (m_listening) {
        return 0;
    }
    if (!m_winsockStarted) {
        return WSANOTINITIALISED;
    }
    m_log.Write(Severity::Information, "Starting Server");
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return WSAGetLastError();
    }
    // Do not let another process bind the same port on top of us.
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in endpoint = {};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    endpoint.sin_addr.s_addr = address;
    if (bind(listener, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR ||
        listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        closesocket(listener);
        return error;
    }

    m_listener = listener;
    m_listening = true;
    m_stopHousekeeping = false;
    m_acceptThread = std::thread([this] { AcceptLoop(); });
    m_housekeepingThread = std::thread([this, keepAliveInterval] { HousekeepingLoop(keepAliveInterval); });
    return 0;
}

void IscsiServer::Stop()
{
    if (!m_listening.exchange(false)) {
        return;
    }
    m_log.Write(Severity::Information, "Stopping Server");

    // Closing the listener makes accept() fail and the accept thread exit.
    closesocket(m_listener);
    m_listener = INVALID_SOCKET;
    m_acceptThread.join();

    {
        std::lock_guard lock(m_housekeepingMutex);
        m_stopHousekeeping = true;
    }
    m_housekeepingWake.notify_all();
    m_housekeepingThread.join();

    for (const auto& target : Targets()) {
        ResetTarget(target->Name());
    }

    // Connections without a target (discovery, logins in progress).
    {
        std::unique_lock lock(m_liveMutex);
        for (const auto& connection : m_liveConnections) {
            connection->sendQueue.Stop();
            connection->Abort();
        }
        m_liveChanged.wait(lock, [this] { return m_liveConnections.empty(); });
    }

    // Sessions that were waiting to expire.
    std::vector<PendingExpiration> pending;
    {
        std::lock_guard lock(m_housekeepingMutex);
        pending.swap(m_pendingExpirations);
    }
    for (const auto& entry : pending) {
        m_sessionManager.RemoveSession(entry.session, SessionTerminationReason::ConnectionFailure);
    }
}

void IscsiServer::ResetTarget(const std::string& targetName)
{
    for (const auto& session : m_sessionManager.FindTargetSessions(targetName)) {
        TerminateSession(session, SessionTerminationReason::TargetReset);
    }
}

void IscsiServer::TerminateSession(const SessionPtr& session, SessionTerminationReason reason)
{
    for (const auto& connection : m_connectionManager.GetSessionConnections(session)) {
        m_connectionManager.ReleaseConnection(connection);
    }
    m_sessionManager.RemoveSession(session, reason);
}

void IscsiServer::AcceptLoop()
{
    for (;;) {
        sockaddr_in address = {};
        int addressLength = sizeof(address);
        SOCKET client = accept(m_listener, reinterpret_cast<sockaddr*>(&address), &addressLength);
        if (client == INVALID_SOCKET) {
            if (!m_listening) {
                return;
            }
            m_log.Log(Severity::Debug, "accept failed, error {}", WSAGetLastError());
            Sleep(100); // e.g. out of resources; do not spin
            continue;
        }

        BOOL noDelay = TRUE; // disable the Nagle algorithm
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

        auto connection = std::make_shared<Connection>(client, FormatEndpoint(address));
        m_log.Log(Severity::Information, "New connection has been accepted from {}", connection->InitiatorAddress());
        {
            std::lock_guard lock(m_liveMutex);
            m_liveConnections.push_back(connection);
        }
        // Stop() waits for the thread through m_liveConnections.
        std::thread([this, connection] { ReceiveLoop(connection); }).detach();
    }
}

void IscsiServer::ReceiveLoop(ConnectionPtr connection)
{
    std::thread sender([this, connection] { SendLoop(connection); });

    Pdu pdu;
    while (ReceivePdu(connection, pdu)) {
        ProcessPdu(std::move(pdu), connection);
        pdu = Pdu{};
    }

    HandleConnectionTermination(connection);
    connection->sendQueue.Stop();
    sender.join();
    connection->CloseSocket(); // before Stop() can return and Winsock be cleaned up

    std::lock_guard lock(m_liveMutex);
    std::erase(m_liveConnections, connection);
    // Notify while holding the lock: once it is released Stop() may return
    // and destroy the server. Nothing below may touch `this`.
    m_liveChanged.notify_all();
}

bool IscsiServer::ReceivePdu(const ConnectionPtr& connection, Pdu& pdu)
{
    int error = 0;
    switch (connection->Receive(pdu.bhs.data(), pdu.bhs.size(), error)) {
    case Connection::IoStatus::Ok:
        break;
    case Connection::IoStatus::PeerClosed:
        m_log.Log(Severity::Verbose, "[{}] The initiator has closed the connection", connection->Identifier());
        return false;
    case Connection::IoStatus::Aborted:
        m_log.Log(Severity::Verbose, "[{}] The connection has been closed", connection->Identifier());
        return false;
    default:
        m_log.Log(Severity::Debug, "[{}] Receive failed, error {}", connection->Identifier(), error);
        return false;
    }
    const size_t dataLength = DataSegmentLength(pdu.bhs.data());
    // DataSegmentLength must not exceed the MaxRecvDataSegmentLength we declared.
    if (dataLength > connection->targetMaxRecvDataSegmentLength) {
        m_log.Log(Severity::Error, "[{}] Initiator error: PDU data segment of {} bytes exceeds {}",
                  connection->Identifier(), dataLength, connection->targetMaxRecvDataSegmentLength);
        return false;
    }
    const size_t ahsLength = TotalAhsLength(pdu.bhs.data());
    std::vector<uint8_t> rest(RemainingLength(pdu.bhs.data()));
    if (connection->Receive(rest.data(), rest.size(), error) != Connection::IoStatus::Ok) {
        m_log.Log(Severity::Debug, "[{}] Connection lost in the middle of a PDU", connection->Identifier());
        return false;
    }
    pdu.ahs.assign(rest.begin(), rest.begin() + ahsLength);
    pdu.data.assign(rest.begin() + ahsLength, rest.begin() + ahsLength + dataLength);
    return true;
}

void IscsiServer::SendLoop(ConnectionPtr connection)
{
    while (std::optional<Pdu> pdu = connection->sendQueue.Dequeue()) {
        const OpCode opcode = pdu->Opcode();
        const bool dataInWithStatus = opcode == OpCode::ScsiDataIn && (pdu->bhs[1] & 0x01) != 0;
        // StatSN is only meaningful in a Data-In PDU if the S bit is set.
        if (opcode != OpCode::ScsiDataIn || dataInWithStatus) {
            SetStatSN(*pdu, connection->statSN);
        }
        if (const SessionPtr session = connection->GetSession()) {
            const uint32_t expCmdSN = session->expCmdSN;
            SetCommandWindow(*pdu, expCmdSN, expCmdSN + session->commandQueueSize);
        }
        if ((opcode == OpCode::NopIn && pdu->InitiatorTaskTag() != kReservedTag) || opcode == OpCode::ScsiResponse ||
            opcode == OpCode::LoginResponse || opcode == OpCode::TextResponse || dataInWithStatus ||
            opcode == OpCode::Reject) {
            ++connection->statSN;
        }

        const std::vector<uint8_t> bytes = pdu->Serialize();
        int error = 0;
        if (connection->Send(bytes, error) != Connection::IoStatus::Ok) {
            m_log.Log(Severity::Verbose, "[{}] Failed to send response to initiator. Operation: 0x{:02X}, Size: {}, Error: {}",
                      connection->Identifier(), static_cast<uint8_t>(opcode), bytes.size(), error);
            connection->Abort(); // make the receive thread notice
            return;
        }
        m_log.Log(Severity::Verbose, "[{}] Sent response to initiator, Operation: 0x{:02X}, Size: {}",
                  connection->Identifier(), static_cast<uint8_t>(opcode), bytes.size());

        // After a Logout Response, or a Login Response with a non-zero status
        // class, the connection is closed (data already sent is still delivered).
        const bool loginFailed =
            opcode == OpCode::LoginResponse && util::ReadBE16(&pdu->bhs[36]) != static_cast<uint16_t>(LoginStatus::Success);
        if (opcode == OpCode::LogoutResponse || loginFailed) {
            connection->ShutdownSend();
            connection->Abort();
            return;
        }
    }
}

void IscsiServer::HandleConnectionTermination(const ConnectionPtr& connection)
{
    m_connectionManager.ReleaseConnection(connection);
    const SessionPtr session = connection->GetSession();
    if (session && m_connectionManager.GetSessionConnections(session).empty()) {
        // Session timeout: the last connection's Time2Wait + Time2Retain expired.
        const int timeout = std::max(0, session->defaultTime2Wait + session->defaultTime2Retain);
        std::lock_guard lock(m_housekeepingMutex);
        m_pendingExpirations.push_back({session, std::chrono::steady_clock::now() + std::chrono::seconds(timeout)});
    }
}

void IscsiServer::HousekeepingLoop(std::optional<std::chrono::seconds> keepAliveInterval)
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point nextKeepAlive = Clock::now() + keepAliveInterval.value_or(std::chrono::seconds(0));

    std::unique_lock lock(m_housekeepingMutex);
    while (!m_housekeepingWake.wait_for(lock, std::chrono::seconds(1), [this] { return m_stopHousekeeping; })) {
        const Clock::time_point now = Clock::now();
        std::vector<SessionPtr> expired;
        std::erase_if(m_pendingExpirations, [&](const PendingExpiration& entry) {
            if (entry.deadline > now) {
                return false;
            }
            expired.push_back(entry.session);
            return true;
        });
        lock.unlock();

        for (const SessionPtr& session : expired) {
            // The initiator may have reconnected in the meantime.
            if (m_connectionManager.GetSessionConnections(session).empty()) {
                m_sessionManager.RemoveSession(session, SessionTerminationReason::ConnectionFailure);
            }
        }
        if (keepAliveInterval && now >= nextKeepAlive) {
            m_connectionManager.SendKeepAlive();
            nextKeepAlive = now + *keepAliveInterval;
        }

        lock.lock();
    }
}

} // namespace tiscsi::iscsi
