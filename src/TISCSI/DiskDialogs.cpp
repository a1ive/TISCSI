// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "DiskDialogs.h"

#include "Common/I18n.h"
#include "Common/Text.h"
#include "Core/disk/DeviceDisk.h"
#include "Core/disk/DiskImage.h"
#include "Core/disk/IsoImage.h"
#include "Core/disk/RamDisk.h"
#include "Core/disk/Vhd.h"
#include "Core/ldm/DynamicVolumes.h"
#include "UiUtil.h"
#include "resource.h"

#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <thread>
#include <vector>

namespace tiscsi {
namespace {

constexpr uint64_t kMiB = 1024 * 1024;
constexpr UINT WM_APP_PROGRESS = WM_APP + 10;
constexpr UINT WM_APP_DONE = WM_APP + 11;

class DialogBase {
public:
    INT_PTR Show(UINT templateId, HWND owner)
    {
        return i18n::DialogBoxParamL(templateId, owner, Proc, reinterpret_cast<LPARAM>(this));
    }

protected:
    virtual ~DialogBase() = default;
    virtual void OnInit() {}
    virtual bool OnOk() = 0; // true closes the dialog with IDOK
    virtual bool OnCancel() { return true; }
    virtual INT_PTR OnMessage(UINT, WPARAM, LPARAM) { return FALSE; }
    virtual void OnCommand(UINT) {}

    HWND m_dialog = nullptr;

private:
    static INT_PTR CALLBACK Proc(HWND dialog, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        DialogBase* self;
        if (msg == WM_INITDIALOG) {
            self = reinterpret_cast<DialogBase*>(lParam);
            self->m_dialog = dialog;
            SetWindowLongPtrW(dialog, DWLP_USER, lParam);
            self->OnInit();
            return TRUE;
        }
        self = reinterpret_cast<DialogBase*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (!self) {
            return FALSE;
        }
        switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IDOK:
                if (self->OnOk()) {
                    EndDialog(dialog, IDOK);
                }
                return TRUE;
            case IDCANCEL:
                if (self->OnCancel()) {
                    EndDialog(dialog, IDCANCEL);
                }
                return TRUE;
            default:
                self->OnCommand(LOWORD(wParam));
                return TRUE;
            }
        case WM_NOTIFY: {
            // Double-clicking a list row means OK.
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->code == NM_DBLCLK && header->idFrom == IDC_DEVICE_LIST &&
                ui::SelectedListRow(header->hwndFrom) >= 0) {
                PostMessageW(dialog, WM_COMMAND, IDOK, 0);
                return TRUE;
            }
            break;
        }
        }
        return self->OnMessage(msg, wParam, lParam);
    }
};

void SetupSizeSpin(HWND dialog, uint32_t minimum, uint32_t maximum, uint32_t initial)
{
    SendDlgItemMessageW(dialog, IDC_SIZE_SPIN, UDM_SETRANGE32, minimum, maximum);
    SendDlgItemMessageW(dialog, IDC_SIZE_SPIN, UDM_SETPOS32, 0, initial);
}

// ---- Existing disk image ---------------------------------------------------

class ImageDialog final : public DialogBase {
public:
    explicit ImageDialog(DiskChoice& choice) : m_choice(choice) {}

private:
    void OnCommand(UINT id) override
    {
        if (id == IDC_BROWSE) {
            if (auto path = ui::BrowseForFile(m_dialog, IDS_FILTER_IMAGES, false, nullptr)) {
                SetDlgItemTextW(m_dialog, IDC_PATH, path->c_str());
                if (disk::HasExtension(*path, L".iso")) {
                    CheckDlgButton(m_dialog, IDC_OPTICAL, BST_CHECKED);
                    UpdateReadOnly();
                }
            }
        } else if (id == IDC_OPTICAL) {
            UpdateReadOnly();
        }
    }

    // A disc is always read-only.
    void UpdateReadOnly()
    {
        const bool optical = IsDlgButtonChecked(m_dialog, IDC_OPTICAL) == BST_CHECKED;
        if (optical) {
            CheckDlgButton(m_dialog, IDC_READ_ONLY, BST_CHECKED);
        }
        EnableWindow(GetDlgItem(m_dialog, IDC_READ_ONLY), !optical);
    }

    bool OnOk() override
    {
        const std::wstring path = ui::GetText(m_dialog, IDC_PATH);
        if (path.empty()) {
            ui::ShowError(m_dialog, i18n::Str(IDS_ERR_NO_FILE));
            return false;
        }
        const bool readOnly = IsDlgButtonChecked(m_dialog, IDC_READ_ONLY) == BST_CHECKED;
        const bool optical = IsDlgButtonChecked(m_dialog, IDC_OPTICAL) == BST_CHECKED;
        disk::OpenError error;
        m_choice.disk = optical ? disk::IsoImage::Open(path, error) : disk::OpenDiskImage(path, readOnly, error);
        if (!m_choice.disk) {
            ui::ShowError(m_dialog, ui::ExplainOpenError(error, false));
            return false;
        }
        m_choice.description = path;
        return true;
    }

    DiskChoice& m_choice;
};

// ---- New VHD ---------------------------------------------------------------

class CreateVhdDialog final : public DialogBase {
public:
    static constexpr uint32_t kMaxSizeMb = static_cast<uint32_t>(disk::kMaxVhdSize / kMiB);

    explicit CreateVhdDialog(DiskChoice& choice) : m_choice(choice) {}
    ~CreateVhdDialog() override
    {
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

private:
    void OnInit() override
    {
        SetupSizeSpin(m_dialog, 1, kMaxSizeMb, 1024);
        SendDlgItemMessageW(m_dialog, IDC_PROGRESS, PBM_SETRANGE32, 0, 100);
    }

    void OnCommand(UINT id) override
    {
        if (id == IDC_BROWSE && !m_working) {
            if (auto path = ui::BrowseForFile(m_dialog, IDS_FILTER_VHD, true, L"vhd")) {
                SetDlgItemTextW(m_dialog, IDC_PATH, path->c_str());
            }
        }
    }

    bool OnOk() override
    {
        if (m_working) {
            return false;
        }
        m_path = ui::GetText(m_dialog, IDC_PATH);
        if (m_path.empty()) {
            ui::ShowError(m_dialog, i18n::Str(IDS_ERR_NO_FILE));
            return false;
        }
        const std::optional<uint64_t> sizeMb = ui::GetNumber(m_dialog, IDC_SIZE_MB, 1, kMaxSizeMb);
        if (!sizeMb) {
            ui::ShowError(m_dialog, i18n::Format(IDS_ERR_INVALID_SIZE, 1u, kMaxSizeMb));
            return false;
        }
        const bool dynamic = IsDlgButtonChecked(m_dialog, IDC_DYNAMIC) == BST_CHECKED;
        SetWorking(true);
        // A fixed VHD is zero-filled, which takes a while: do it on a worker
        // thread that reports progress and can be cancelled.
        m_cancel = false;
        m_worker = std::thread([this, size = *sizeMb * kMiB, dynamic, path = m_path, dialog = m_dialog] {
            DWORD error;
            if (dynamic) {
                error = disk::CreateDynamicVhd(path, size);
            } else {
                int lastPercent = -1;
                error = disk::CreateFixedVhd(path, size, [&](uint64_t done, uint64_t total) {
                    const int percent = static_cast<int>(done * 100 / total);
                    if (percent != lastPercent) {
                        lastPercent = percent;
                        PostMessageW(dialog, WM_APP_PROGRESS, static_cast<WPARAM>(percent), 0);
                    }
                    return !m_cancel.load();
                });
            }
            PostMessageW(dialog, WM_APP_DONE, error, 0);
        });
        return false;
    }

    bool OnCancel() override
    {
        if (m_working) {
            m_cancel = true; // closes once the worker has stopped
            return false;
        }
        return true;
    }

    INT_PTR OnMessage(UINT msg, WPARAM wParam, LPARAM) override
    {
        switch (msg) {
        case WM_APP_PROGRESS:
            SendDlgItemMessageW(m_dialog, IDC_PROGRESS, PBM_SETPOS, wParam, 0);
            return TRUE;
        case WM_APP_DONE:
            OnDone(static_cast<DWORD>(wParam));
            return TRUE;
        case WM_CLOSE:
            PostMessageW(m_dialog, WM_COMMAND, IDCANCEL, 0);
            return TRUE;
        }
        return FALSE;
    }

    void OnDone(DWORD error)
    {
        m_worker.join();
        SetWorking(false);
        if (error == ERROR_CANCELLED) {
            EndDialog(m_dialog, IDCANCEL);
            return;
        }
        if (error != ERROR_SUCCESS) {
            ui::ShowError(m_dialog, i18n::Format(IDS_ERR_CREATE, m_path, text::ErrorMessage(error)));
            SendDlgItemMessageW(m_dialog, IDC_PROGRESS, PBM_SETPOS, 0, 0);
            return;
        }
        disk::OpenError openError;
        m_choice.disk = disk::OpenDiskImage(m_path, false, openError);
        if (!m_choice.disk) {
            ui::ShowError(m_dialog, ui::ExplainOpenError(openError, false));
            return;
        }
        m_choice.description = m_path;
        EndDialog(m_dialog, IDOK);
    }

    void SetWorking(bool working)
    {
        m_working = working;
        for (int id : {IDC_PATH, IDC_BROWSE, IDC_SIZE_MB, IDC_SIZE_SPIN, IDC_DYNAMIC, IDOK}) {
            EnableWindow(GetDlgItem(m_dialog, id), !working);
        }
    }

    DiskChoice& m_choice;
    std::wstring m_path;
    std::thread m_worker;
    std::atomic<bool> m_cancel{false};
    bool m_working = false;
};

// ---- RAM disk ----------------------------------------------------------------

class RamDiskDialog final : public DialogBase {
public:
    // What a 32-bit process can realistically allocate in one piece.
    static constexpr uint32_t kMaxSizeMb = sizeof(void*) == 4 ? 1536 : 1024 * 1024;

    explicit RamDiskDialog(DiskChoice& choice) : m_choice(choice) {}

private:
    void OnInit() override { SetupSizeSpin(m_dialog, 1, kMaxSizeMb, 512); }

    bool OnOk() override
    {
        const std::optional<uint64_t> sizeMb = ui::GetNumber(m_dialog, IDC_SIZE_MB, 1, kMaxSizeMb);
        if (!sizeMb) {
            ui::ShowError(m_dialog, i18n::Format(IDS_ERR_INVALID_SIZE, 1u, kMaxSizeMb));
            return false;
        }
        m_choice.disk = disk::RamDisk::Create(*sizeMb * kMiB);
        if (!m_choice.disk) {
            ui::ShowError(m_dialog, i18n::Format(IDS_ERR_RAM_ALLOC, text::FormatSize(*sizeMb * kMiB)));
            return false;
        }
        m_choice.description = i18n::Str(IDS_DESC_RAM_DISK);
        return true;
    }

    DiskChoice& m_choice;
};

// ---- Physical disk -----------------------------------------------------------

std::wstring VolumeDisplayName(const disk::VolumeInfo& volume)
{
    std::wstring name;
    for (const std::wstring& mountPoint : volume.mountPoints) {
        name += (name.empty() ? L"" : L", ") + mountPoint;
    }
    return name.empty() ? volume.volumeName : name;
}

class PhysicalDiskDialog final : public DialogBase {
public:
    explicit PhysicalDiskDialog(DiskChoice& choice) : m_choice(choice) {}

private:
    void OnInit() override
    {
        ui::InitListView(m_dialog, IDC_DEVICE_LIST,
                         {{IDS_COL_DISK, 30}, {IDS_COL_DESCRIPTION, 144}, {IDS_COL_SERIAL, 70}, {IDS_COL_SIZE, 50}});
        HWND list = GetDlgItem(m_dialog, IDC_DEVICE_LIST);
        m_disks = disk::ListPhysicalDisks();
        for (size_t i = 0; i < m_disks.size(); ++i) {
            const disk::PhysicalDiskInfo& info = m_disks[i];
            std::wstring description = Model(info);
            if (info.isDynamic) {
                description += i18n::Str(IDS_DYNAMIC_SUFFIX);
            }
            ui::AddListRow(list, {std::to_wstring(info.index), description, text::Utf8ToWide(info.serialNumber),
                                  text::FormatSize(info.size)},
                           static_cast<LPARAM>(i));
        }
    }

    static std::wstring Model(const disk::PhysicalDiskInfo& info)
    {
        return text::Utf8ToWide(info.vendor.empty() ? info.product : info.vendor + " " + info.product);
    }

    bool OnOk() override
    {
        const int row = ui::SelectedListRow(GetDlgItem(m_dialog, IDC_DEVICE_LIST));
        if (row < 0) {
            ui::ShowError(m_dialog, i18n::Str(IDS_ERR_NO_SELECTION));
            return false;
        }
        const disk::PhysicalDiskInfo& info = m_disks[static_cast<size_t>(ui::ListRowParam(GetDlgItem(m_dialog, IDC_DEVICE_LIST), row))];
        const bool readOnly = IsDlgButtonChecked(m_dialog, IDC_READ_ONLY) == BST_CHECKED;
        if (!readOnly && !info.isDynamic) {
            // Say which volumes will disappear while the disk is served.
            std::wstring volumes;
            for (const disk::VolumeInfo& volume : disk::ListVolumes()) {
                if (std::find(volume.diskNumbers.begin(), volume.diskNumbers.end(), info.index) !=
                    volume.diskNumbers.end()) {
                    volumes += L"  " + VolumeDisplayName(volume) + L"\n";
                }
            }
            if (!volumes.empty() && !ui::Confirm(m_dialog, i18n::Format(IDS_CONFIRM_LOCK_DISK, info.index, volumes))) {
                return false;
            }
        }
        disk::OpenError error;
        m_choice.disk = disk::PhysicalDisk::Open(info.index, readOnly, error);
        if (!m_choice.disk) {
            ui::ShowError(m_dialog, ui::ExplainOpenError(error, true));
            return false;
        }
        m_choice.description = i18n::Format(IDS_DESC_PHYSICAL_DISK, info.index);
        const std::wstring model = Model(info);
        if (!model.empty()) {
            m_choice.description += L" (" + model + L")";
        }
        return true;
    }

    DiskChoice& m_choice;
    std::vector<disk::PhysicalDiskInfo> m_disks;
};

// ---- Volume --------------------------------------------------------------------

std::wstring DynamicVolumeType(const ldm::DynamicVolumeInfo& volume)
{
    UINT layout = IDS_LAYOUT_SIMPLE;
    switch (volume.layout) {
    case ldm::VolumeLayout::Simple:
        layout = IDS_LAYOUT_SIMPLE;
        break;
    case ldm::VolumeLayout::Spanned:
        layout = IDS_LAYOUT_SPANNED;
        break;
    case ldm::VolumeLayout::Striped:
        layout = IDS_LAYOUT_STRIPED;
        break;
    case ldm::VolumeLayout::Mirrored:
        layout = IDS_LAYOUT_MIRRORED;
        break;
    case ldm::VolumeLayout::Raid5:
        layout = IDS_LAYOUT_RAID5;
        break;
    }
    std::wstring type = i18n::Str(layout);
    if (volume.health == ldm::VolumeHealth::FailedRedundancy) {
        type += i18n::Str(IDS_HEALTH_FAILED_REDUNDANCY);
    } else if (volume.health == ldm::VolumeHealth::Failed) {
        type += i18n::Str(IDS_HEALTH_FAILED);
    }
    return type;
}

bool SameVolumeName(const std::wstring& a, const std::wstring& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) { return towlower(x) == towlower(y); });
}

// Windows volumes, plus dynamic volumes Windows has not mounted (from the LDM
// database, as iSCSI Console lists them). Dynamic volumes Windows has are
// shown once, as Windows volumes with their layout.
class VolumeDialog final : public DialogBase {
public:
    explicit VolumeDialog(DiskChoice& choice) : m_choice(choice) {}

private:
    struct Row {
        bool dynamic;   // true: m_dynamic[index], not mounted by Windows
        size_t index;
    };

    void OnInit() override
    {
        ui::InitListView(m_dialog, IDC_DEVICE_LIST,
                         {{IDS_COL_VOLUME, 96}, {IDS_COL_LABEL, 54}, {IDS_COL_FILE_SYSTEM, 50}, {IDS_COL_TYPE, 64},
                          {IDS_COL_SIZE, 50}, {IDS_COL_DISK, 30}});
        HWND list = GetDlgItem(m_dialog, IDC_DEVICE_LIST);
        m_volumes = disk::ListVolumes();
        std::wstring failedPath;
        if (ldm::ListDynamicVolumes({}, m_dynamic, failedPath) != ERROR_SUCCESS) {
            m_dynamic.clear(); // the Windows volumes are still useful
        }
        std::vector<uint32_t> dynamicDisks;
        for (const disk::PhysicalDiskInfo& info : disk::ListPhysicalDisks()) {
            if (info.isDynamic) {
                dynamicDisks.push_back(info.index);
            }
        }

        for (size_t i = 0; i < m_volumes.size(); ++i) {
            const disk::VolumeInfo& info = m_volumes[i];
            std::wstring disks;
            for (uint32_t number : info.diskNumbers) {
                disks += (disks.empty() ? L"" : L", ") + std::to_wstring(number);
            }
            std::wstring type;
            const auto dynamic = std::find_if(m_dynamic.begin(), m_dynamic.end(), [&info](const auto& volume) {
                return SameVolumeName(volume.windowsVolumeName, info.volumeName);
            });
            if (dynamic != m_dynamic.end()) {
                type = DynamicVolumeType(*dynamic);
            } else if (std::any_of(info.diskNumbers.begin(), info.diskNumbers.end(), [&](uint32_t number) {
                           return std::find(dynamicDisks.begin(), dynamicDisks.end(), number) != dynamicDisks.end();
                       })) {
                type = i18n::Str(IDS_TYPE_DYNAMIC);
            } else if (!info.diskNumbers.empty()) {
                type = i18n::Str(IDS_TYPE_BASIC);
            }
            AddRow(list, {VolumeDisplayName(info), info.label, info.fileSystem, type, text::FormatSize(info.size), disks},
                   {false, i});
        }
        for (size_t i = 0; i < m_dynamic.size(); ++i) {
            const ldm::DynamicVolumeInfo& info = m_dynamic[i];
            if (!info.windowsVolumeName.empty()) {
                continue;
            }
            std::wstring disks;
            for (const std::wstring& disk : info.disks) {
                disks += (disks.empty() ? L"" : L", ") + disk;
            }
            AddRow(list, {DynamicDisplayName(info), L"", L"", DynamicVolumeType(info), text::FormatSize(info.size), disks},
                   {true, i});
        }
    }

    static std::wstring DynamicDisplayName(const ldm::DynamicVolumeInfo& info)
    {
        return i18n::Format(IDS_UNMOUNTED_VOLUME, text::Utf8ToWide(info.name), text::Utf8ToWide(info.diskGroupName));
    }

    void AddRow(HWND list, std::initializer_list<std::wstring> cells, Row row)
    {
        m_rows.push_back(row);
        ui::AddListRow(list, cells, static_cast<LPARAM>(m_rows.size() - 1));
    }

    bool OnOk() override
    {
        const int selected = ui::SelectedListRow(GetDlgItem(m_dialog, IDC_DEVICE_LIST));
        if (selected < 0) {
            ui::ShowError(m_dialog, i18n::Str(IDS_ERR_NO_SELECTION));
            return false;
        }
        const Row& row = m_rows[static_cast<size_t>(ui::ListRowParam(GetDlgItem(m_dialog, IDC_DEVICE_LIST), selected))];
        const bool readOnly = IsDlgButtonChecked(m_dialog, IDC_READ_ONLY) == BST_CHECKED;
        disk::OpenError error;
        std::wstring name;
        if (row.dynamic) {
            const ldm::DynamicVolumeInfo& info = m_dynamic[row.index];
            name = DynamicDisplayName(info);
            const std::wstring id = text::Utf8ToWide(info.guid.ToString());
            m_choice.disk = ldm::OpenDynamicVolume(id, readOnly, {}, error);
            if (error.path == id) {
                error.path = name;
            }
            m_choice.description = i18n::Format(IDS_DESC_DYNAMIC_VOLUME, name);
        } else {
            const disk::VolumeInfo& info = m_volumes[row.index];
            name = VolumeDisplayName(info);
            m_choice.disk = disk::VolumeDisk::Open(info.volumeName, readOnly, error);
            if (error.path == info.volumeName) {
                error.path = name;
            }
            m_choice.description = i18n::Format(IDS_DESC_VOLUME, name);
        }
        if (!m_choice.disk) {
            ui::ShowError(m_dialog, ui::ExplainOpenError(error, true));
            return false;
        }
        return true;
    }

    DiskChoice& m_choice;
    std::vector<disk::VolumeInfo> m_volumes;
    std::vector<ldm::DynamicVolumeInfo> m_dynamic;
    std::vector<Row> m_rows;
};

} // namespace

bool ChooseDiskImage(HWND owner, DiskChoice& choice)
{
    ImageDialog dialog(choice);
    return dialog.Show(IDD_SELECT_IMAGE, owner) == IDOK;
}

bool CreateVirtualDisk(HWND owner, DiskChoice& choice)
{
    CreateVhdDialog dialog(choice);
    return dialog.Show(IDD_CREATE_VHD, owner) == IDOK;
}

bool CreateRamDisk(HWND owner, DiskChoice& choice)
{
    RamDiskDialog dialog(choice);
    return dialog.Show(IDD_CREATE_RAMDISK, owner) == IDOK;
}

bool ChoosePhysicalDisk(HWND owner, DiskChoice& choice)
{
    PhysicalDiskDialog dialog(choice);
    return dialog.Show(IDD_SELECT_PHYSICAL, owner) == IDOK;
}

bool ChooseVolume(HWND owner, DiskChoice& choice)
{
    VolumeDialog dialog(choice);
    return dialog.Show(IDD_SELECT_VOLUME, owner) == IDOK;
}

} // namespace tiscsi
