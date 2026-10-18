// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Unicode console output: WriteConsoleW on a console, UTF-8 when redirected.

#pragma once

#include "Common/Text.h"

#include <functional>
#include <string>
#include <string_view>

namespace tiscsi::cli {

void WriteOut(std::wstring_view text);
void WriteErr(std::wstring_view text);

// A service has no console: output goes to `sink` instead (`error` for
// WriteErr). An empty sink restores the console.
using OutputSink = std::function<void(bool error, std::wstring_view text)>;
void SetOutputSink(OutputSink sink);

inline void PrintLine(std::wstring_view text) { WriteOut(std::wstring(text) + L"\n"); }
inline void PrintError(std::wstring_view text) { WriteErr(std::wstring(text) + L"\n"); }

using text::ErrorMessage;
using text::FormatSize;
using text::Utf8ToWide;
using text::WideToUtf8;

} // namespace tiscsi::cli
