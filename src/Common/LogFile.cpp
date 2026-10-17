// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Common/LogFile.h"

#include <format>

namespace tiscsi {

LogFile::~LogFile()
{
    if (m_file != INVALID_HANDLE_VALUE) {
        CloseHandle(m_file);
    }
}

DWORD LogFile::Open(const std::wstring& path)
{
    // Write-through, no buffering: a full file system cache must not stall
    // the I/O path that is logging (same choice as iSCSI Console).
    m_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    return m_file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
}

std::string LogFile::FormatLine(util::Severity severity, std::string_view source, std::string_view message)
{
    SYSTEMTIME now;
    GetLocalTime(&now);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} {:<12} [{}] {}", now.wYear, now.wMonth, now.wDay, now.wHour,
                       now.wMinute, now.wSecond, util::SeverityName(severity), source, message);
}

void LogFile::Write(util::Severity severity, std::string_view source, std::string_view message)
{
    if (m_file == INVALID_HANDLE_VALUE) {
        return;
    }
    const std::string line = FormatLine(severity, source, message) + "\r\n";
    std::lock_guard lock(m_mutex);
    DWORD written;
    WriteFile(m_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

} // namespace tiscsi
