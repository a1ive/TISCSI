// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "AppSettings.h"

#include <windows.h>

namespace tiscsi::settings {
namespace {

constexpr wchar_t kSectionUi[] = L"UI";
constexpr wchar_t kKeyLanguage[] = L"Language";

const std::wstring& IniPath()
{
    static const std::wstring path = [] {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;) {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            // XP does not set ERROR_INSUFFICIENT_BUFFER, it only returns the full size.
            if (length < buffer.size()) {
                buffer.resize(length);
                break;
            }
            buffer.resize(buffer.size() * 2);
        }
        buffer.resize(buffer.find_last_of(L'\\') + 1);
        return buffer + L"TISCSI.ini";
    }();
    return path;
}

} // namespace

std::wstring LoadLanguage()
{
    wchar_t value[64] = {};
    GetPrivateProfileStringW(kSectionUi, kKeyLanguage, L"auto", value, static_cast<DWORD>(std::size(value)),
                             IniPath().c_str());
    return value;
}

void SaveLanguage(std::wstring_view tag)
{
    // Silently fails when the directory is read-only (e.g. CD or WinPE media).
    WritePrivateProfileStringW(kSectionUi, kKeyLanguage, std::wstring(tag).c_str(), IniPath().c_str());
}

} // namespace tiscsi::settings
