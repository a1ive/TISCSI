// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/ISCSI.Server/ConnectionState.cs,
// ConnectionParameters.cs and ConnectionManager.cs). Internal to IscsiServer.

#pragma once

#include "Core/iscsi/Parameters.h"
#include "Core/iscsi/Pdu.h"
#include "Core/iscsi/Session.h"
#include "Core/util/BlockingQueue.h"
#include "Core/util/CountdownLatch.h"

#include <winsock2.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tiscsi::iscsi {

// A write command waiting for its Data-Out PDUs.
struct TransferEntry {
    std::shared_ptr<ScsiCommand> command;
    uint32_t transferTag = 0;
    uint32_t nextR2TSN = 0;
    uint32_t nextOffset = 0;
    uint32_t totalR2Ts = 0; // R2Ts that will be sent during this transfer
};

class Connection {
public:
    Connection(SOCKET socket, std::string initiatorAddress);
    ~Connection(); // closes the socket
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    const std::string& InitiatorAddress() const { return m_initiatorAddress; }

    std::shared_ptr<Session> GetSession() const;
    void SetSession(std::shared_ptr<Session> session);

    enum class IoStatus { Ok, PeerClosed, Aborted, Error };
    // Blocking transfers that return early once Abort() is called. Receive is
    // used by the receive thread only, Send by the sender thread only.
    IoStatus Receive(uint8_t* buffer, size_t length, int& error);
    IoStatus Send(const std::vector<uint8_t>& bytes, int& error);

    // Makes pending and future Receive/Send calls fail. Safe from any thread.
    // (shutdown() alone does not wake a blocked recv() on Windows.)
    void Abort();
    // Sends FIN after queued data (graceful close of our side).
    void ShutdownSend();
    // Called by the receive thread once the sender thread has exited.
    void CloseSocket();

    std::string Identifier() const;

    // Set during login, before the connection becomes visible to other threads.
    uint16_t cid = 0;
    bool isFullFeaturePhase = false;
    uint32_t initiatorMaxRecvDataSegmentLength = defaults::kMaxRecvDataSegmentLength;
    const uint32_t targetMaxRecvDataSegmentLength = declared::kMaxRecvDataSegmentLength;

    uint32_t statSN = 0; // sender thread only; any initial value will do

    util::BlockingQueue<Pdu> sendQueue;
    util::CountdownLatch runningCommands;

    // Text and login key=value sequences spanning several PDUs (receive thread).
    std::string AddTextToSequence(uint32_t initiatorTaskTag, std::string_view text);
    void RemoveTextSequence(uint32_t initiatorTaskTag);

    // Pending write transfers (receive thread).
    TransferEntry& AddTransfer(uint32_t initiatorTaskTag, uint32_t transferTag, TransferEntry entry);
    TransferEntry* GetTransfer(uint32_t transferTag);
    TransferEntry* GetTransferByTaskTag(uint32_t initiatorTaskTag);
    void RemoveTransfer(uint32_t initiatorTaskTag, uint32_t transferTag);

private:
    // Waits for an overlapped operation or Abort(); cancels the operation on abort.
    IoStatus WaitForIo(WSAOVERLAPPED& overlapped, DWORD& transferred, int& error);

    const SOCKET m_socket;
    std::mutex m_socketMutex;
    bool m_socketClosed = false;
    HANDLE m_abortEvent;   // manual reset
    WSAEVENT m_receiveEvent;
    WSAEVENT m_sendEvent;
    const std::string m_initiatorAddress;
    mutable std::mutex m_sessionMutex;
    std::shared_ptr<Session> m_session;

    std::unordered_map<uint32_t, std::string> m_textSequences;
    std::unordered_map<uint32_t, uint32_t> m_taskTagToTransferTag;
    std::unordered_map<uint32_t, TransferEntry> m_transfers;
};

// Connections that completed login (full feature phase).
class ConnectionManager {
public:
    void AddConnection(std::shared_ptr<Connection> connection);
    bool RemoveConnection(const std::shared_ptr<Connection>& connection);
    // Waits for in-flight commands, stops sending and shuts the socket down.
    void ReleaseConnection(const std::shared_ptr<Connection>& connection);
    std::shared_ptr<Connection> FindConnection(const std::shared_ptr<Session>& session, uint16_t cid) const;
    std::vector<std::shared_ptr<Connection>> GetSessionConnections(const std::shared_ptr<Session>& session) const;
    // TCP only notices a dead peer when sending, so send a NOP-In to every
    // connection; a failed send triggers the connection termination logic.
    void SendKeepAlive();

private:
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<Connection>> m_connections;
};

} // namespace tiscsi::iscsi
