// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Common/Text.h"

#include "Common/I18n.h"

#include <windows.h>

#include <format>
#include <iterator>

namespace tiscsi::text {

std::wstring Utf8ToWide(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string WideToUtf8(std::wstring_view text)
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

std::wstring ErrorMessage(unsigned long error)
{
    // Prefer the UI language; system messages exist only for installed languages.
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD length = FormatMessageW(flags, nullptr, error, i18n::Current().langId, reinterpret_cast<wchar_t*>(&buffer), 0,
                                  nullptr);
    if (length == 0) {
        length = FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    }
    std::wstring message = length ? std::wstring(buffer, length) : std::wstring();
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' ')) {
        message.pop_back();
    }
    return message.empty() ? std::format(L"error {}", error) : std::format(L"{} ({})", message, error);
}

std::wstring FormatSize(uint64_t bytes)
{
    static constexpr const wchar_t* kUnits[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    size_t unit = 0;
    double value = static_cast<double>(bytes);
    while (value >= 1024 && unit + 1 < std::size(kUnits)) {
        value /= 1024;
        ++unit;
    }
    if (value == static_cast<double>(static_cast<uint64_t>(value))) {
        return std::format(L"{} {}", static_cast<uint64_t>(value), kUnits[unit]);
    }
    return std::format(L"{:.1f} {}", value, kUnits[unit]);
}

} // namespace tiscsi::text
