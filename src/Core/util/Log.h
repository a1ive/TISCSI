// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Diagnostic logging. Log messages are English by design (they are meant for
// troubleshooting and bug reports, not for the localized UI).

#pragma once

#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace tiscsi::util {

// Same levels as iSCSI Console's ISCSI.Logging.Severity.
enum class Severity {
    Critical = 1,
    Error = 2,
    Warning = 3,
    Information = 4,
    Verbose = 5,
    Debug = 6,
    Trace = 7,
};

std::string_view SeverityName(Severity severity);

// Log messages are UTF-8; use this for paths and other wide strings.
std::string ToUtf8(std::wstring_view text);

// Receives every log entry; may be called concurrently from any thread.
using LogSink = std::function<void(Severity severity, std::string_view source, std::string_view message)>;

// A named log source forwarding to a shared sink. Copyable and cheap.
class Logger {
public:
    Logger() = default;
    Logger(std::string source, std::shared_ptr<const LogSink> sink)
        : m_source(std::move(source)), m_sink(std::move(sink))
    {
    }

    void SetSink(std::shared_ptr<const LogSink> sink) { m_sink = std::move(sink); }
    const std::shared_ptr<const LogSink>& Sink() const { return m_sink; }

    void Write(Severity severity, std::string_view message) const
    {
        if (m_sink && *m_sink) {
            (*m_sink)(severity, m_source, message);
        }
    }

    template <typename... Args>
    void Log(Severity severity, std::format_string<Args...> fmt, Args&&... args) const
    {
        if (m_sink && *m_sink) {
            (*m_sink)(severity, m_source, std::format(fmt, std::forward<Args>(args)...));
        }
    }

private:
    std::string m_source;
    std::shared_ptr<const LogSink> m_sink;
};

} // namespace tiscsi::util
