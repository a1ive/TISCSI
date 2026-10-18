// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Console.h"

#include <windows.h>

#include <mutex>

namespace tiscsi::cli {
namespace {

std::mutex g_outputMutex;
OutputSink g_sink;

void Write(DWORD stdHandle, std::wstring_view text)
{
    std::lock_guard lock(g_outputMutex);
    if (g_sink) {
        g_sink(stdHandle == STD_ERROR_HANDLE, text);
        return;
    }
    HANDLE handle = GetStdHandle(stdHandle);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD mode;
    DWORD written;
    if (GetConsoleMode(handle, &mode)) {
        WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    } else {
        const std::string utf8 = WideToUtf8(text);
        WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

} // namespace

void WriteOut(std::wstring_view text) { Write(STD_OUTPUT_HANDLE, text); }

void WriteErr(std::wstring_view text) { Write(STD_ERROR_HANDLE, text); }

void SetOutputSink(OutputSink sink)
{
    std::lock_guard lock(g_outputMutex);
    g_sink = std::move(sink);
}

} // namespace tiscsi::cli
