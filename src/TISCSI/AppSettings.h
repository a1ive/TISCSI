// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Portable settings stored in TISCSI.ini next to the executable.

#pragma once

#include <string>
#include <string_view>

namespace tiscsi::settings {

// UI language tag ("auto", "en-US", "ja-JP", ...). Defaults to "auto".
std::wstring LoadLanguage();
void SaveLanguage(std::wstring_view tag);

} // namespace tiscsi::settings
