// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/Utilities/KeyValuePairUtils.cs).
// iSCSI text data: "key=value" entries, each terminated by a NUL byte.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tiscsi::util {

// Ordered list of key/value pairs. Keys are case sensitive (RFC 3720).
class KeyValueList {
public:
    using Entry = std::pair<std::string, std::string>;

    static KeyValueList ParseNullDelimited(std::string_view text);
    std::string ToNullDelimited() const;
    // "key=value, key=value" for logs.
    std::string ToDisplayString() const;

    void Add(std::string key, std::string value) { m_entries.emplace_back(std::move(key), std::move(value)); }
    std::optional<std::string_view> Find(std::string_view key) const;
    bool Contains(std::string_view key) const { return Find(key).has_value(); }

    bool empty() const { return m_entries.empty(); }
    size_t size() const { return m_entries.size(); }
    auto begin() const { return m_entries.begin(); }
    auto end() const { return m_entries.end(); }

private:
    std::vector<Entry> m_entries;
};

} // namespace tiscsi::util
