// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive

#include "Core/util/KeyValueText.h"

namespace tiscsi::util {

KeyValueList KeyValueList::ParseNullDelimited(std::string_view text)
{
    KeyValueList result;
    while (!text.empty()) {
        const size_t end = text.find('\0');
        const std::string_view entry = text.substr(0, end);
        // Split at the first '=' only; values may contain '='.
        const size_t separator = entry.find('=');
        if (separator != std::string_view::npos) {
            result.Add(std::string(entry.substr(0, separator)), std::string(entry.substr(separator + 1)));
        }
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
    return result;
}

std::string KeyValueList::ToNullDelimited() const
{
    std::string result;
    for (const auto& [key, value] : m_entries) {
        result += key;
        result += '=';
        result += value;
        result += '\0';
    }
    return result;
}

std::string KeyValueList::ToDisplayString() const
{
    std::string result;
    for (const auto& [key, value] : m_entries) {
        if (!result.empty()) {
            result += ", ";
        }
        result += key;
        result += '=';
        result += value;
    }
    return result;
}

std::optional<std::string_view> KeyValueList::Find(std::string_view key) const
{
    for (const auto& [k, v] : m_entries) {
        if (k == key) {
            return v;
        }
    }
    return std::nullopt;
}

} // namespace tiscsi::util
