// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "UiUtil.h"

#include "Common/I18n.h"
#include "Common/Text.h"
#include "Core/disk/Device.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <charconv>
#include <vector>

namespace tiscsi::ui {

std::wstring GetText(HWND dialog, int controlId)
{
    HWND control = GetDlgItem(dialog, controlId);
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    text.resize(GetWindowTextW(control, text.data(), static_cast<int>(text.size())));
    return text;
}

void InitListView(HWND dialog, int controlId, std::initializer_list<std::pair<UINT, int>> columns)
{
    HWND list = GetDlgItem(dialog, controlId);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    int index = 0;
    for (const auto& [stringId, width] : columns) {
        RECT rc = {0, 0, width, 0};
        MapDialogRect(dialog, &rc);
        std::wstring title = i18n::Str(stringId);
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = title.data();
        column.cx = rc.right;
        ListView_InsertColumn(list, index++, &column);
    }
}

int AddListRow(HWND list, std::initializer_list<std::wstring> cells, LPARAM param)
{
    int row = -1;
    int column = 0;
    for (const std::wstring& cell : cells) {
        std::wstring text = cell;
        if (column == 0) {
            LVITEMW item = {};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = ListView_GetItemCount(list);
            item.pszText = text.data();
            item.lParam = param;
            row = ListView_InsertItem(list, &item);
        } else {
            ListView_SetItemText(list, row, column, text.data());
        }
        ++column;
    }
    return row;
}

void SetListCell(HWND list, int row, int column, const std::wstring& text)
{
    std::wstring copy = text;
    ListView_SetItemText(list, row, column, copy.data());
}

int SelectedListRow(HWND list) { return ListView_GetNextItem(list, -1, LVNI_SELECTED); }

LPARAM ListRowParam(HWND list, int row)
{
    LVITEMW item = {};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    ListView_GetItem(list, &item);
    return item.lParam;
}

void ShowError(HWND owner, const std::wstring& text)
{
    MessageBoxW(owner, text.c_str(), i18n::Str(IDS_APP_TITLE).c_str(), MB_OK | MB_ICONERROR);
}

void ShowInfo(HWND owner, const std::wstring& text)
{
    MessageBoxW(owner, text.c_str(), i18n::Str(IDS_APP_TITLE).c_str(), MB_OK | MB_ICONINFORMATION);
}

bool Confirm(HWND owner, const std::wstring& text)
{
    return MessageBoxW(owner, text.c_str(), i18n::Str(IDS_APP_TITLE).c_str(), MB_YESNO | MB_ICONWARNING |
                       MB_DEFBUTTON2) == IDYES;
}

std::wstring ExplainOpenError(const disk::OpenError& error, bool device)
{
    std::wstring message;
    switch (error.problem) {
    case disk::OpenError::Problem::DynamicDiskWrite:
        return i18n::Format(IDS_ERR_DYNAMIC_DISK, error.path);
    case disk::OpenError::Problem::VolumeLocked:
        message = i18n::Format(IDS_ERR_VOLUME_LOCKED, error.path, text::ErrorMessage(error.code));
        break;
    case disk::OpenError::Problem::AlreadyInUse:
        return i18n::Format(IDS_ERR_ALREADY_SERVED, error.path);
    case disk::OpenError::Problem::WriteProtected:
        return i18n::Format(IDS_ERR_WRITE_PROTECTED, error.path);
    case disk::OpenError::Problem::NoSuchVolume:
        return i18n::Format(IDS_ERR_NO_SUCH_VOLUME, error.path);
    case disk::OpenError::Problem::AmbiguousVolume:
        return i18n::Format(IDS_ERR_AMBIGUOUS_VOLUME, error.path);
    case disk::OpenError::Problem::VolumeFailed:
        return i18n::Format(IDS_ERR_VOLUME_FAILED, error.path);
    case disk::OpenError::Problem::DegradedWrite:
        return i18n::Format(IDS_ERR_DEGRADED_WRITE, error.path);
    case disk::OpenError::Problem::None:
        // error.path may name a parent or extent file rather than the image itself.
        message = i18n::Format(device ? IDS_ERR_OPEN_DEVICE : IDS_ERR_OPEN_IMAGE, error.path,
                            text::ErrorMessage(error.code));
        break;
    }
    if (error.code == ERROR_ACCESS_DENIED && !disk::IsProcessElevated()) {
        message += L"\n\n" + i18n::Str(IDS_HINT_ADMIN);
    }
    return message;
}

std::optional<std::wstring> BrowseForFile(HWND owner, UINT filterId, bool save, const wchar_t* defaultExtension)
{
    std::wstring filter = i18n::Str(filterId);
    std::replace(filter.begin(), filter.end(), L'|', L'\0');
    filter.push_back(L'\0');
    std::vector<wchar_t> path(32768, L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = path.data();
    ofn.nMaxFile = static_cast<DWORD>(path.size());
    ofn.lpstrDefExt = defaultExtension;
    ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) {
        return std::nullopt;
    }
    return std::wstring(path.data());
}

std::optional<uint64_t> GetNumber(HWND dialog, int controlId, uint64_t minimum, uint64_t maximum)
{
    const std::string text = text::WideToUtf8(GetText(dialog, controlId));
    uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || value < minimum || value > maximum) {
        return std::nullopt;
    }
    return value;
}

} // namespace tiscsi::ui
