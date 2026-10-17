// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Small helpers for dialogs: list views, message boxes, file dialogs.

#pragma once

#include "Core/disk/DiskImage.h"

#include <windows.h>

#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

namespace tiscsi::ui {

std::wstring GetText(HWND dialog, int controlId);

// Report-style list view with full-row selection; column widths in dialog units.
void InitListView(HWND dialog, int controlId, std::initializer_list<std::pair<UINT, int>> columns);
int AddListRow(HWND list, std::initializer_list<std::wstring> cells, LPARAM param = 0);
void SetListCell(HWND list, int row, int column, const std::wstring& text);
int SelectedListRow(HWND list); // -1 if none
LPARAM ListRowParam(HWND list, int row);

void ShowError(HWND owner, const std::wstring& text);
void ShowInfo(HWND owner, const std::wstring& text);
bool Confirm(HWND owner, const std::wstring& text);

// Explains why a disk could not be opened (with an administrator hint when
// access was denied). `device`: a physical disk or volume rather than a file.
std::wstring ExplainOpenError(const disk::OpenError& error, bool device);

// Open / Save As dialog; `filterId` names a string of '|'-separated pairs.
std::optional<std::wstring> BrowseForFile(HWND owner, UINT filterId, bool save, const wchar_t* defaultExtension);

// Range-checked integer from an edit control (MB values in the size fields).
std::optional<uint64_t> GetNumber(HWND dialog, int controlId, uint64_t minimum, uint64_t maximum);

} // namespace tiscsi::ui
