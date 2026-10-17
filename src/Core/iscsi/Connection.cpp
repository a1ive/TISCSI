// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive

#include "Core/iscsi/Connection.h"

#include <algorithm>
#include <format>

namespace tiscsi::iscsi {

Connection::Connection(SOCKET socket, std::string initiatorAddress)
    : m_socket(socket), m_initiatorAddress(std::move(initiatorAddress)),
      m_abortEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)), m_receiveEvent(WSACreateEvent()),
      m_sendEvent(WSACreateEvent())
{
}

Connection::~Connection()
{
    CloseSocket();
    WSACloseEvent(m_sendEvent);
    WSACloseEvent(m_receiveEvent);
    CloseHandle(m_abortEvent);
}

Connection::IoStatus Connection::WaitForIo(WSAOVERLAPPED& overlapped, DWORD& transferred, int& error)
{
    HANDLE handles[] = {overlapped.hEvent, m_abortEvent};
    DWORD flags = 0;
    if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) {
        // CancelIo only cancels I/O issued by this thread, which is exactly
        // the operation we own. Wait for it to finish before the buffer and
        // OVERLAPPED go away.
        CancelIo(reinterpret_cast<HANDLE>(m_socket));
        WSAGetOverlappedResult(m_socket, &overlapped, &transferred, TRUE, &flags);
        return IoStatus::Aborted;
    }
    if (!WSAGetOverlappedResult(m_socket, &overlapped, &transferred, FALSE, &flags)) {
        error = WSAGetLastError();
        return IoStatus::Error;
    }
    return IoStatus::Ok;
}

Connection::IoStatus Connection::Receive(uint8_t* buffer, size_t length, int& error)
{
    while (length > 0) {
        if (WaitForSingleObject(m_abortEvent, 0) == WAIT_OBJECT_0) {
            return IoStatus::Aborted;
        }
        WSABUF wsaBuffer = {static_cast<ULONG>(std::min<size_t>(length, 1 << 20)), reinterpret_cast<char*>(buffer)};
        WSAOVERLAPPED overlapped = {};
        overlapped.hEvent = m_receiveEvent;
        WSAResetEvent(m_receiveEvent);
        DWORD received = 0;
        DWORD flags = 0;
        if (WSARecv(m_socket, &wsaBuffer, 1, &received, &flags, &overlapped, nullptr) == SOCKET_ERROR) {
            error = WSAGetLastError();
            if (error != WSA_IO_PENDING) {
                return IoStatus::Error;
            }
            if (const IoStatus status = WaitForIo(overlapped, received, error); status != IoStatus::Ok) {
                return status;
            }
        }
        if (received == 0) {
            return IoStatus::PeerClosed;
        }
        buffer += received;
        length -= received;
    }
    return IoStatus::Ok;
}

Connection::IoStatus Connection::Send(const std::vector<uint8_t>& bytes, int& error)
{
    const uint8_t* p = bytes.data();
    size_t remaining = bytes.size();
    while (remaining > 0) {
        if (WaitForSingleObject(m_abortEvent, 0) == WAIT_OBJECT_0) {
            return IoStatus::Aborted;
        }
        WSABUF wsaBuffer = {static_cast<ULONG>(std::min<size_t>(remaining, 1 << 20)),
                            const_cast<char*>(reinterpret_cast<const char*>(p))};
        WSAOVERLAPPED overlapped = {};
        overlapped.hEvent = m_sendEvent;
        WSAResetEvent(m_sendEvent);
        DWORD sent = 0;
        if (WSASend(m_socket, &wsaBuffer, 1, &sent, 0, &overlapped, nullptr) == SOCKET_ERROR) {
            error = WSAGetLastError();
            if (error != WSA_IO_PENDING) {
                return IoStatus::Error;
            }
            if (const IoStatus status = WaitForIo(overlapped, sent, error); status != IoStatus::Ok) {
                return status;
            }
        }
        p += sent;
        remaining -= sent;
    }
    return IoStatus::Ok;
}

void Connection::Abort() { SetEvent(m_abortEvent); }

std::shared_ptr<Session> Connection::GetSession() const
{
    std::lock_guard lock(m_sessionMutex);
    return m_session;
}

void Connection::SetSession(std::shared_ptr<Session> session)
{
    std::lock_guard lock(m_sessionMutex);
    m_session = std::move(session);
}

void Connection::ShutdownSend()
{
    // The lock keeps us from touching a handle value that was closed and reused.
    std::lock_guard lock(m_socketMutex);
    if (!m_socketClosed) {
        shutdown(m_socket, SD_SEND);
    }
}

void Connection::CloseSocket()
{
    std::lock_guard lock(m_socketMutex);
    if (!m_socketClosed) {
        m_socketClosed = true;
        closesocket(m_socket);
    }
}

std::string Connection::Identifier() const
{
    const std::shared_ptr<Session> session = GetSession();
    if (session) {
        return std::format("{},CID={:x}", session->Identifier(), cid);
    }
    return std::format("ISID=0,TSIH=0,CID={:x}", cid);
}

std::string Connection::AddTextToSequence(uint32_t initiatorTaskTag, std::string_view text)
{
    std::string& sequence = m_textSequences[initiatorTaskTag];
    sequence += text;
    return sequence;
}

void Connection::RemoveTextSequence(uint32_t initiatorTaskTag) { m_textSequences.erase(initiatorTaskTag); }

TransferEntry& Connection::AddTransfer(uint32_t initiatorTaskTag, uint32_t transferTag, TransferEntry entry)
{
    m_taskTagToTransferTag[initiatorTaskTag] = transferTag;
    return m_transfers[transferTag] = std::move(entry);
}

TransferEntry* Connection::GetTransfer(uint32_t transferTag)
{
    auto it = m_transfers.find(transferTag);
    return it != m_transfers.end() ? &it->second : nullptr;
}

TransferEntry* Connection::GetTransferByTaskTag(uint32_t initiatorTaskTag)
{
    auto it = m_taskTagToTransferTag.find(initiatorTaskTag);
    return it != m_taskTagToTransferTag.end() ? GetTransfer(it->second) : nullptr;
}

void Connection::RemoveTransfer(uint32_t initiatorTaskTag, uint32_t transferTag)
{
    m_taskTagToTransferTag.erase(initiatorTaskTag);
    m_transfers.erase(transferTag);
}

void ConnectionManager::AddConnection(std::shared_ptr<Connection> connection)
{
    std::lock_guard lock(m_mutex);
    m_connections.push_back(std::move(connection));
}

bool ConnectionManager::RemoveConnection(const std::shared_ptr<Connection>& connection)
{
    std::lock_guard lock(m_mutex);
    auto it = std::find(m_connections.begin(), m_connections.end(), connection);
    if (it == m_connections.end()) {
        return false;
    }
    m_connections.erase(it);
    return true;
}

void ConnectionManager::ReleaseConnection(const std::shared_ptr<Connection>& connection)
{
    connection->runningCommands.WaitUntilZero();
    connection->sendQueue.Stop();
    connection->Abort();
    RemoveConnection(connection);
}

std::shared_ptr<Connection> ConnectionManager::FindConnection(const std::shared_ptr<Session>& session,
                                                              uint16_t cid) const
{
    std::lock_guard lock(m_mutex);
    for (const auto& connection : m_connections) {
        if (connection->GetSession() == session && connection->cid == cid) {
            return connection;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<Connection>>
ConnectionManager::GetSessionConnections(const std::shared_ptr<Session>& session) const
{
    std::lock_guard lock(m_mutex);
    std::vector<std::shared_ptr<Connection>> result;
    for (const auto& connection : m_connections) {
        if (connection->GetSession() == session) {
            result.push_back(connection);
        }
    }
    return result;
}

void ConnectionManager::SendKeepAlive()
{
    std::lock_guard lock(m_mutex);
    for (const auto& connection : m_connections) {
        // Unsolicited NOP-In: ITT and TTT both reserved, no reply expected.
        connection->sendQueue.Enqueue(NopIn{}.ToPdu());
    }
}

} // namespace tiscsi::iscsi
