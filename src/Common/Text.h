// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Text helpers shared by the front ends.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tiscsi::text {

std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);

// System message for a Win32 / Winsock error code in the UI language when
// available, e.g. "Access is denied. (5)".
std::wstring ErrorMessage(unsigned long error);

// "512 MiB", "1.5 GiB"
std::wstring FormatSize(uint64_t bytes);

} // namespace tiscsi::text
