// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// UI language selection and language-aware resource loading.
//
// All translations are embedded in the executable as per-language resources
// (STRINGTABLE / DIALOGEX / MENU tagged with LANGUAGE). Lookups go through
// FindResourceExW with an explicit LANGID, so the selected language does not
// depend on thread locale behaviour, which differs between XP and Vista+.
// A resource missing from the current language falls back to en-US.

#pragma once

#include <windows.h>

#include <format>
#include <span>
#include <string>
#include <string_view>

namespace tiscsi::i18n {

struct Language {
    LANGID langId;
    const wchar_t* tag;        // BCP 47 tag used in settings, e.g. L"ja-JP"
    const wchar_t* nativeName; // Shown in the language menu, in its own language
};

// Languages that have embedded resources, en-US first.
std::span<const Language> SupportedLanguages();

// Selects the UI language. An empty tag or L"auto" follows the user's
// Windows UI language; an unknown tag behaves like L"auto".
// `module` is the module that carries the localized resources.
void Initialize(HMODULE module, std::wstring_view tag = {});

const Language& Current();

// Returns the string as a view into the resource section (not null-terminated),
// or an empty view if the id exists in no language.
std::wstring_view LoadStr(UINT id);

inline std::wstring Str(UINT id) { return std::wstring(LoadStr(id)); }

// std::format with a localized format string, e.g. L"{0} of {1}". Translators
// may reorder positional arguments. A malformed translation yields the raw
// format string instead of throwing.
template <typename... Args>
std::wstring Format(UINT id, const Args&... args)
{
    const std::wstring_view fmt = LoadStr(id);
    try {
        return std::vformat(fmt, std::make_wformat_args(args...));
    } catch (const std::format_error&) {
        return std::wstring(fmt);
    }
}

HMENU LoadMenuL(UINT id);
INT_PTR DialogBoxParamL(UINT id, HWND parent, DLGPROC proc, LPARAM param = 0);
HWND CreateDialogParamL(UINT id, HWND parent, DLGPROC proc, LPARAM param = 0);

} // namespace tiscsi::i18n
