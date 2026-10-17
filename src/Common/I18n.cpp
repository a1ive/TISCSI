// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Common/I18n.h"

#include <array>

namespace tiscsi::i18n {
namespace {

constexpr LANGID kEnglishUS = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);

// To add a language: add an entry here and a lang\<tag>.rc2 file included by
// the application's .rc.
constexpr std::array kLanguages{
    Language{kEnglishUS, L"en-US", L"English"},
    Language{MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN), L"ja-JP", L"日本語"},
    Language{MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED), L"zh-CN", L"简体中文"},
    Language{MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL), L"zh-TW", L"正體中文"},
    Language{MAKELANGID(LANG_KOREAN, SUBLANG_KOREAN), L"ko-KR", L"한국어"},
};

HMODULE g_module = nullptr;
const Language* g_current = &kLanguages[0];

bool IsTraditionalChinese(LANGID id)
{
    switch (SUBLANGID(id)) {
    case SUBLANG_CHINESE_TRADITIONAL:
    case SUBLANG_CHINESE_HONGKONG:
    case SUBLANG_CHINESE_MACAU:
        return true;
    default:
        return false;
    }
}

const Language* MatchLangId(LANGID id)
{
    for (const Language& lang : kLanguages) {
        if (lang.langId == id) {
            return &lang;
        }
    }
    // Same primary language, different region (e.g. en-GB -> en-US). Chinese
    // must also agree on the script, or zh-TW users would get Simplified.
    for (const Language& lang : kLanguages) {
        if (PRIMARYLANGID(lang.langId) != PRIMARYLANGID(id)) {
            continue;
        }
        if (PRIMARYLANGID(id) == LANG_CHINESE && IsTraditionalChinese(id) != IsTraditionalChinese(lang.langId)) {
            continue;
        }
        return &lang;
    }
    return nullptr;
}

const Language* MatchTag(std::wstring_view tag)
{
    for (const Language& lang : kLanguages) {
        const std::wstring_view candidate = lang.tag;
        if (candidate.size() == tag.size() &&
            CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, candidate.data(), static_cast<int>(candidate.size()),
                           tag.data(), static_cast<int>(tag.size())) == CSTR_EQUAL) {
            return &lang;
        }
    }
    return nullptr;
}

const void* FindResourceData(LPCWSTR type, LPCWSTR name, LANGID lang, DWORD* size = nullptr)
{
    HRSRC info = FindResourceExW(g_module, type, name, lang);
    if (!info) {
        return nullptr;
    }
    HGLOBAL handle = LoadResource(g_module, info);
    if (!handle) {
        return nullptr;
    }
    if (size) {
        *size = SizeofResource(g_module, info);
    }
    return LockResource(handle);
}

bool Found(const void* p) { return p != nullptr; }
bool Found(std::wstring_view s) { return !s.empty(); }

// Current language first, then en-US.
template <typename Fn>
auto WithFallback(Fn&& lookup) -> decltype(lookup(LANGID{}))
{
    auto result = lookup(g_current->langId);
    if (!Found(result) && g_current->langId != kEnglishUS) {
        result = lookup(kEnglishUS);
    }
    return result;
}

std::wstring_view FindString(UINT id, LANGID lang)
{
    // Strings are stored in bundles of 16; each entry is a WORD length
    // followed by that many WCHARs (no terminator). Length 0 = absent.
    DWORD size = 0;
    auto* data = static_cast<const WCHAR*>(
        FindResourceData(RT_STRING, MAKEINTRESOURCEW((id >> 4) + 1), lang, &size));
    if (!data) {
        return {};
    }
    const WCHAR* const end = data + size / sizeof(WCHAR);
    for (UINT i = 0; i < (id & 15); ++i) {
        if (data >= end) {
            return {};
        }
        data += 1 + *data;
    }
    if (data >= end || *data == 0 || data + 1 + *data > end) {
        return {};
    }
    return {data + 1, *data};
}

const DLGTEMPLATE* FindDialog(UINT id)
{
    return WithFallback([id](LANGID lang) {
        return static_cast<const DLGTEMPLATE*>(FindResourceData(RT_DIALOG, MAKEINTRESOURCEW(id), lang));
    });
}

} // namespace

std::span<const Language> SupportedLanguages() { return kLanguages; }

void Initialize(HMODULE module, std::wstring_view tag)
{
    g_module = module;
    const Language* lang = nullptr;
    if (!tag.empty() && tag != L"auto") {
        lang = MatchTag(tag);
    }
    if (!lang) {
        lang = MatchLangId(GetUserDefaultUILanguage());
    }
    g_current = lang ? lang : &kLanguages[0];
}

const Language& Current() { return *g_current; }

std::wstring_view LoadStr(UINT id)
{
    return WithFallback([id](LANGID lang) { return FindString(id, lang); });
}

HMENU LoadMenuL(UINT id)
{
    auto* tmpl = WithFallback([id](LANGID lang) {
        return static_cast<const MENUTEMPLATEW*>(FindResourceData(RT_MENU, MAKEINTRESOURCEW(id), lang));
    });
    return tmpl ? LoadMenuIndirectW(tmpl) : nullptr;
}

INT_PTR DialogBoxParamL(UINT id, HWND parent, DLGPROC proc, LPARAM param)
{
    const DLGTEMPLATE* tmpl = FindDialog(id);
    return tmpl ? DialogBoxIndirectParamW(g_module, tmpl, parent, proc, param) : -1;
}

HWND CreateDialogParamL(UINT id, HWND parent, DLGPROC proc, LPARAM param)
{
    const DLGTEMPLATE* tmpl = FindDialog(id);
    return tmpl ? CreateDialogIndirectParamW(g_module, tmpl, parent, proc, param) : nullptr;
}

} // namespace tiscsi::i18n
