// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive

#include "Core/iscsi/IscsiName.h"

#include <algorithm>
#include <charconv>

namespace tiscsi::iscsi {
namespace {

char ToLowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

bool IsAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool IsHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

int ParseInt(std::string_view text)
{
    int value = -1;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc() && end == text.data() + text.size() ? value : -1;
}

bool IsValidReversedDomainName(std::string_view name)
{
    // Split at '.'; each component is non-empty, alphanumeric or '-', and
    // does not start or end with '-'.
    for (;;) {
        const size_t dot = name.find('.');
        const std::string_view component = name.substr(0, dot);
        if (component.empty() || component.front() == '-' || component.back() == '-' ||
            !std::all_of(component.begin(), component.end(), [](char c) { return IsAlnum(c) || c == '-'; })) {
            return false;
        }
        if (dot == std::string_view::npos) {
            return true;
        }
        name.remove_prefix(dot + 1);
    }
}

// RFC 3720: the owner of the domain assigns the rest as desired, but iSCSI
// names contain no whitespace. An empty sub-qualifier is valid.
bool IsValidSubQualifier(std::string_view subQualifier) { return subQualifier.find(' ') == std::string_view::npos; }

} // namespace

bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return ToLowerAscii(x) == ToLowerAscii(y); });
}

bool IsValidIscsiName(std::string_view name)
{
    return StartsWithIgnoreCase(name, "iqn.") ? IsValidIqn(name) : IsValidEui(name);
}

bool IsValidIqn(std::string_view name)
{
    if (!StartsWithIgnoreCase(name, "iqn.") || name.size() <= 12 || name[8] != '-' || name[11] != '.') {
        return false;
    }
    const int year = ParseInt(name.substr(4, 4));
    const int month = ParseInt(name.substr(9, 2));
    if (year < 0 || month < 1 || month > 12) {
        return false;
    }
    const size_t colon = name.find(':');
    if (colon != std::string_view::npos && colon >= 12) {
        return IsValidReversedDomainName(name.substr(12, colon - 12)) && IsValidSubQualifier(name.substr(colon + 1));
    }
    return IsValidReversedDomainName(name.substr(12));
}

bool IsValidEui(std::string_view name)
{
    return StartsWithIgnoreCase(name, "eui.") && name.size() == 20 &&
           std::all_of(name.begin() + 4, name.end(), IsHex);
}

} // namespace tiscsi::iscsi
