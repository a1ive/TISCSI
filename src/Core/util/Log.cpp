// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/util/Log.h"

#include <windows.h>

namespace tiscsi::util {

std::string ToUtf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::string_view SeverityName(Severity severity)
{
    switch (severity) {
    case Severity::Critical:
        return "Critical";
    case Severity::Error:
        return "Error";
    case Severity::Warning:
        return "Warning";
    case Severity::Information:
        return "Information";
    case Severity::Verbose:
        return "Verbose";
    case Severity::Debug:
        return "Debug";
    case Severity::Trace:
        return "Trace";
    }
    return "Unknown";
}

} // namespace tiscsi::util
