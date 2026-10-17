// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Appends log entries to a UTF-8 text file (format of iSCSI Console's log).

#pragma once

#include "Core/util/Log.h"

#include <windows.h>

#include <mutex>
#include <string>
#include <string_view>

namespace tiscsi {

class LogFile {
public:
    LogFile() = default;
    ~LogFile();
    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;

    // Returns ERROR_SUCCESS or a Win32 error.
    DWORD Open(const std::wstring& path);
    bool IsOpen() const { return m_file != INVALID_HANDLE_VALUE; }

    // "2026-10-17 13:37:28 Warning      [source] message"
    static std::string FormatLine(util::Severity severity, std::string_view source, std::string_view message);
    void Write(util::Severity severity, std::string_view source, std::string_view message);

private:
    HANDLE m_file = INVALID_HANDLE_VALUE;
    std::mutex m_mutex;
};

} // namespace tiscsi
