// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "AddTargetDialog.h"

#include "Common/I18n.h"
#include "Common/Text.h"
#include "Core/disk/Device.h"
#include "Core/iscsi/IscsiName.h"
#include "DiskDialogs.h"
#include "UiUtil.h"
#include "resource.h"

#include <commctrl.h>

#include <format>

namespace tiscsi {

bool AddTargetDialog::Run(HWND owner)
{
    return i18n::DialogBoxParamL(IDD_ADD_TARGET, owner, DialogProc, reinterpret_cast<LPARAM>(this)) == IDOK;
}

INT_PTR CALLBACK AddTargetDialog::DialogProc(HWND dialog, UINT msg, WPARAM wParam, LPARAM lParam)
{
    AddTargetDialog* self;
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<AddTargetDialog*>(lParam);
        self->m_dialog = dialog;
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
    } else {
        self = reinterpret_cast<AddTargetDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : FALSE;
}

INT_PTR AddTargetDialog::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
        OnInit();
        return TRUE;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header->idFrom == IDC_DISKS && header->code == LVN_ITEMCHANGED) {
            UpdateUi();
        }
        return TRUE;
    }
    case WM_COMMAND: {
        DiskChoice choice;
        bool chosen = false;
        switch (LOWORD(wParam)) {
        case IDC_CREATE_VHD:
            chosen = CreateVirtualDisk(m_dialog, choice);
            break;
        case IDC_CREATE_RAMDISK:
            chosen = CreateRamDisk(m_dialog, choice);
            break;
        case IDC_ADD_IMAGE:
            chosen = ChooseDiskImage(m_dialog, choice);
            break;
        case IDC_ADD_PHYSICAL:
            chosen = ChoosePhysicalDisk(m_dialog, choice);
            break;
        case IDC_ADD_VOLUME:
            chosen = ChooseVolume(m_dialog, choice);
            break;
        case IDC_REMOVE_DISK:
            RemoveDisk();
            return TRUE;
        case IDOK:
            if (Accept()) {
                EndDialog(m_dialog, IDOK);
            }
            return TRUE;
        case IDCANCEL:
            EndDialog(m_dialog, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
        }
        if (chosen) {
            AddDisk(std::move(choice));
        }
        return TRUE;
    }
    }
    return FALSE;
}

void AddTargetDialog::OnInit()
{
    const std::string name = std::format("{}:target{}", iscsi::kDefaultTargetNamePrefix, m_app.nextTargetNumber);
    SetDlgItemTextW(m_dialog, IDC_TARGET_NAME, text::Utf8ToWide(name).c_str());
    ui::InitListView(m_dialog, IDC_DISKS, {{IDS_COL_DESCRIPTION, 120}, {IDS_COL_SIZE, 46}});
    // Physical disks and volumes need administrator rights.
    const bool elevated = disk::IsProcessElevated();
    EnableWindow(GetDlgItem(m_dialog, IDC_ADD_PHYSICAL), elevated);
    EnableWindow(GetDlgItem(m_dialog, IDC_ADD_VOLUME), elevated);
    UpdateUi();
}

void AddTargetDialog::AddDisk(DiskChoice choice)
{
    if (choice.disk->IsReadOnly()) {
        choice.description += i18n::Str(IDS_READ_ONLY_SUFFIX);
    }
    ui::AddListRow(GetDlgItem(m_dialog, IDC_DISKS), {choice.description, text::FormatSize(choice.disk->Size())});
    m_disks.push_back(std::move(choice.disk));
    m_specs.push_back(std::move(choice.spec));
    UpdateUi();
}

void AddTargetDialog::RemoveDisk()
{
    HWND list = GetDlgItem(m_dialog, IDC_DISKS);
    const int selected = ui::SelectedListRow(list);
    if (selected < 0 || static_cast<size_t>(selected) >= m_disks.size()) {
        return;
    }
    ListView_DeleteItem(list, selected);
    m_disks.erase(m_disks.begin() + selected); // closes the disk and releases its locks
    m_specs.erase(m_specs.begin() + selected);
    UpdateUi();
}

bool AddTargetDialog::Accept()
{
    const std::string name = text::WideToUtf8(ui::GetText(m_dialog, IDC_TARGET_NAME));
    if (!iscsi::IsValidIscsiName(name)) {
        ui::ShowError(m_dialog, i18n::Str(IDS_ERR_INVALID_TARGET_NAME));
        return false;
    }
    if (m_app.HasTargetNamed(name)) {
        ui::ShowError(m_dialog, i18n::Format(IDS_ERR_TARGET_EXISTS, text::Utf8ToWide(name)));
        return false;
    }
    if (m_disks.empty()) {
        ui::ShowError(m_dialog, i18n::Str(IDS_ERR_NO_DISKS));
        return false;
    }
    const size_t lunCount = m_disks.size();
    auto target = std::make_shared<iscsi::IscsiTarget>(name, std::move(m_disks), m_app.ScsiLogger());
    if (!m_app.AddTarget(std::move(target), lunCount, config::TargetSpec{name, std::move(m_specs), {}})) {
        ui::ShowError(m_dialog, i18n::Format(IDS_ERR_TARGET_EXISTS, text::Utf8ToWide(name)));
        return true; // the disks now belong to the discarded target; nothing left to edit
    }
    ++m_app.nextTargetNumber;
    return true;
}

void AddTargetDialog::UpdateUi()
{
    const int selected = ui::SelectedListRow(GetDlgItem(m_dialog, IDC_DISKS));
    EnableWindow(GetDlgItem(m_dialog, IDC_REMOVE_DISK), selected >= 0);
}

} // namespace tiscsi
