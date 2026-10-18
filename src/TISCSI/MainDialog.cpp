// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "MainDialog.h"

#include "AccessDialog.h"
#include "AddTargetDialog.h"
#include "AppSettings.h"
#include "ConfigFiles.h"
#include "Common/I18n.h"
#include "Common/Text.h"
#include "Core/disk/Device.h"
#include "UiUtil.h"
#include "resource.h"
#include "version.h"

#include <winsock2.h>
#include <commctrl.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

namespace tiscsi {
namespace {

// Posted to itself to open App::startupConfig once the window is shown.
constexpr UINT WM_APP_OPEN_STARTUP_CONFIG = WM_APP + 2;

// Returns the popup menu that directly contains command `id`.
HMENU FindSubMenuContaining(HMENU menu, UINT id)
{
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        HMENU sub = GetSubMenu(menu, i);
        if (sub && GetMenuState(sub, id, MF_BYCOMMAND) != static_cast<UINT>(-1)) {
            HMENU deeper = FindSubMenuContaining(sub, id);
            return deeper ? deeper : sub;
        }
    }
    return nullptr;
}

// IPv4 addresses of this host (gethostbyname works on every Windows version).
std::vector<std::wstring> LocalAddresses()
{
    std::vector<std::wstring> result;
    char name[256] = {};
    if (gethostname(name, sizeof(name)) != 0) {
        return result;
    }
    const hostent* host = gethostbyname(name);
    if (!host || host->h_addrtype != AF_INET) {
        return result;
    }
    for (char** address = host->h_addr_list; *address; ++address) {
        in_addr in;
        std::memcpy(&in, *address, sizeof(in));
        result.push_back(text::Utf8ToWide(inet_ntoa(in)));
    }
    return result;
}

} // namespace

INT_PTR CALLBACK AboutDialogProc(HWND dialog, UINT msg, WPARAM wParam, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dialog, IDC_ABOUT_VERSION,
                        i18n::Format(IDS_ABOUT_VERSION, std::wstring_view(TISCSI_VERSION_WSTRING)).c_str());
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

INT_PTR MainDialog::Run()
{
    const INT_PTR result = i18n::DialogBoxParamL(IDD_MAIN, nullptr, DialogProc, reinterpret_cast<LPARAM>(this));
    m_app.SetNotifyWindow(nullptr);
    return result;
}

INT_PTR CALLBACK MainDialog::DialogProc(HWND dialog, UINT msg, WPARAM wParam, LPARAM lParam)
{
    MainDialog* self;
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<MainDialog*>(lParam);
        self->m_dialog = dialog;
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
    } else {
        self = reinterpret_cast<MainDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : FALSE;
}

INT_PTR MainDialog::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
        OnInit();
        return TRUE;
    case WM_COMMAND:
        OnCommand(LOWORD(wParam));
        return TRUE;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header->idFrom == IDC_TARGETS && header->code == LVN_ITEMCHANGED) {
            UpdateUi();
        }
        return TRUE;
    }
    case WM_APP_SESSIONS_CHANGED:
        RefreshTargets();
        return TRUE;
    case WM_APP_OPEN_STARTUP_CONFIG:
        if (!m_app.startupConfig.empty()) {
            const std::wstring path = std::exchange(m_app.startupConfig, {});
            OpenConfiguration(path, true);
        }
        return TRUE;
    case WM_CLOSE:
        if (ConfirmExit()) {
            EndDialog(m_dialog, IDOK);
        }
        return TRUE;
    case WM_DESTROY:
        if (m_iconLarge) {
            DestroyIcon(m_iconLarge);
        }
        if (m_iconSmall) {
            DestroyIcon(m_iconSmall);
        }
        return FALSE;
    }
    return FALSE;
}

void MainDialog::OnInit()
{
    HINSTANCE instance = GetModuleHandleW(nullptr);
    m_iconLarge = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    m_iconSmall = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    SendMessageW(m_dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(m_iconLarge));
    SendMessageW(m_dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m_iconSmall));
    SetWindowTextW(m_dialog, i18n::Str(IDS_APP_TITLE).c_str());

    // The template's client area holds the controls; the menu (set at run
    // time) and the status bar come on top of it, so size the window for all
    // three. The menu may wrap at large font sizes, hence the second pass.
    RECT client;
    GetClientRect(m_dialog, &client);
    BuildMenu();
    m_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, m_dialog, nullptr,
                               instance, nullptr);
    SendMessageW(m_status, WM_SIZE, 0, 0); // the status bar sizes itself; measure it afterwards
    RECT statusRect;
    GetWindowRect(m_status, &statusRect);
    const int targetHeight = client.bottom + (statusRect.bottom - statusRect.top);
    for (int pass = 0; pass < 2; ++pass) {
        RECT now;
        GetClientRect(m_dialog, &now);
        RECT window;
        GetWindowRect(m_dialog, &window);
        SetWindowPos(m_dialog, nullptr, 0, 0, window.right - window.left,
                     window.bottom - window.top + (targetHeight - now.bottom), SWP_NOMOVE | SWP_NOZORDER);
    }
    SendMessageW(m_status, WM_SIZE, 0, 0);

    FillAddresses();
    SetDlgItemInt(m_dialog, IDC_PORT, m_app.port, FALSE);
    SendDlgItemMessageW(m_dialog, IDC_PORT, EM_LIMITTEXT, 5, 0);
    ui::InitListView(m_dialog, IDC_TARGETS,
                     {{IDS_COL_TARGET, 150}, {IDS_COL_LUNS, 28}, {IDS_COL_SESSIONS, 36}, {IDS_COL_ACCESS, 50}});
    // Number columns as wide as their (translated) headers.
    HWND list = GetDlgItem(m_dialog, IDC_TARGETS);
    ListView_SetColumnWidth(list, 1, LVSCW_AUTOSIZE_USEHEADER);
    ListView_SetColumnWidth(list, 2, LVSCW_AUTOSIZE_USEHEADER);
    m_app.SetNotifyWindow(m_dialog);
    RefreshTargets();
    if (!m_app.startupConfig.empty()) {
        PostMessageW(m_dialog, WM_APP_OPEN_STARTUP_CONFIG, 0, 0);
    }
}

void MainDialog::BuildMenu()
{
    HMENU menu = i18n::LoadMenuL(IDR_MAINMENU);
    if (HMENU languageMenu = menu ? FindSubMenuContaining(menu, IDM_LANG_AUTO) : nullptr) {
        UINT checked = IDM_LANG_AUTO;
        const auto languages = i18n::SupportedLanguages();
        for (size_t i = 0; i < languages.size(); ++i) {
            const UINT id = IDM_LANG_FIRST + static_cast<UINT>(i);
            AppendMenuW(languageMenu, MF_STRING, id, languages[i].nativeName);
            if (m_languageSetting == languages[i].tag) {
                checked = id;
            }
        }
        CheckMenuRadioItem(languageMenu, IDM_LANG_AUTO, IDM_LANG_FIRST + static_cast<UINT>(languages.size()) - 1,
                           checked, MF_BYCOMMAND);
    }
    SetMenu(m_dialog, menu);
}

void MainDialog::FillAddresses()
{
    HWND combo = GetDlgItem(m_dialog, IDC_ADDRESS);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::Str(IDS_ANY_ADDRESS).c_str()));
    for (const std::wstring& address : LocalAddresses()) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(address.c_str()));
    }
    // Keep the previous choice across dialog recreation. An address from a
    // configuration file may not be on this computer (yet): add it.
    int index = 0;
    if (m_app.addressIndex > 0) {
        LRESULT found =
            SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(m_app.addressText.c_str()));
        if (found == CB_ERR) {
            found = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m_app.addressText.c_str()));
        }
        index = found == CB_ERR ? 0 : static_cast<int>(found);
    }
    SendMessageW(combo, CB_SETCURSEL, index, 0);
}

void MainDialog::RefreshTargets()
{
    HWND list = GetDlgItem(m_dialog, IDC_TARGETS);
    const int selected = ui::SelectedListRow(list);
    ListView_DeleteAllItems(list);
    for (const App::TargetEntry& entry : m_app.Targets()) {
        const std::string& name = entry.target->Name();
        ui::AddListRow(list, {text::Utf8ToWide(name), std::to_wstring(entry.lunCount),
                              std::to_wstring(m_app.SessionCount(name)), AccessSummary(entry.target->access)});
    }
    if (selected >= 0 && selected < ListView_GetItemCount(list)) {
        ListView_SetItemState(list, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    SizeTargetColumns();
    UpdateUi();
}

void MainDialog::SizeTargetColumns()
{
    // The access column fits its header and texts (cells and headers are
    // drawn with a margin of about 6 pixels on each side); the name gets
    // the rest.
    HWND list = GetDlgItem(m_dialog, IDC_TARGETS);
    int accessWidth = ListView_GetStringWidth(list, i18n::Str(IDS_COL_ACCESS).c_str());
    for (const App::TargetEntry& entry : m_app.Targets()) {
        accessWidth = std::max(accessWidth, ListView_GetStringWidth(list, AccessSummary(entry.target->access).c_str()));
    }
    accessWidth += 14;
    RECT listClient;
    GetClientRect(list, &listClient);
    const int nameWidth = listClient.right - ListView_GetColumnWidth(list, 1) - ListView_GetColumnWidth(list, 2) -
                          accessWidth - GetSystemMetrics(SM_CXVSCROLL);
    ListView_SetColumnWidth(list, 0, std::max(nameWidth, 60));
    ListView_SetColumnWidth(list, 3, accessWidth);
}

void MainDialog::UpdateUi()
{
    SetDlgItemTextW(m_dialog, IDC_START, i18n::Str(m_app.running ? IDS_STOP : IDS_START).c_str());
    EnableWindow(GetDlgItem(m_dialog, IDC_ADDRESS), !m_app.running);
    EnableWindow(GetDlgItem(m_dialog, IDC_PORT), !m_app.running);

    const int selected = ui::SelectedListRow(GetDlgItem(m_dialog, IDC_TARGETS));
    const bool canRemove = selected >= 0 && static_cast<size_t>(selected) < m_app.Targets().size() &&
                           m_app.SessionCount(m_app.Targets()[selected].target->Name()) == 0;
    EnableWindow(GetDlgItem(m_dialog, IDC_REMOVE_TARGET), canRemove);

    std::wstring status;
    if (m_app.running) {
        status = i18n::Format(IDS_STATUS_RUNNING, m_app.addressIndex == 0 ? std::wstring(L"0.0.0.0") : m_app.addressText,
                              m_app.port, m_app.TotalSessions());
    } else if (!disk::IsProcessElevated()) {
        status = i18n::Str(IDS_STATUS_NOT_ADMIN);
    } else {
        status = i18n::Str(IDS_STATUS_STOPPED);
    }
    SetWindowTextW(m_status, status.c_str());
}

void MainDialog::OnCommand(UINT id)
{
    if (id == IDM_LANG_AUTO || (id >= IDM_LANG_FIRST && id <= IDM_LANG_LAST)) {
        const auto languages = i18n::SupportedLanguages();
        const size_t index = id - IDM_LANG_FIRST;
        if (id != IDM_LANG_AUTO && index >= languages.size()) {
            return;
        }
        m_languageSetting = id == IDM_LANG_AUTO ? L"auto" : languages[index].tag;
        settings::SaveLanguage(m_languageSetting);
        i18n::Initialize(GetModuleHandleW(nullptr), m_languageSetting);
        // Dialog templates are per language: rebuild the window. The server
        // and targets live in App and keep running.
        EndDialog(m_dialog, kRecreate);
        return;
    }
    switch (id) {
    case IDC_START:
        StartOrStop();
        break;
    case IDC_ADD_TARGET:
        AddTarget();
        break;
    case IDC_REMOVE_TARGET:
        RemoveTarget();
        break;
    case IDM_FILE_OPEN_CONFIG:
        if (auto path = ui::BrowseForFile(m_dialog, IDS_FILTER_CONFIG, false, nullptr)) {
            OpenConfiguration(*path, false);
        }
        break;
    case IDM_FILE_SAVE_CONFIG:
        SaveConfiguration();
        break;
    case IDM_HELP_ABOUT:
        i18n::DialogBoxParamL(IDD_ABOUT, m_dialog, AboutDialogProc);
        break;
    case IDM_FILE_EXIT:
        SendMessageW(m_dialog, WM_CLOSE, 0, 0);
        break;
    // IDCANCEL (Escape) is ignored: the main window closes only on purpose.
    }
}

void MainDialog::StartOrStop()
{
    if (m_app.running) {
        m_app.Server().Stop();
        m_app.running = false;
        UpdateUi();
        return;
    }
    BOOL valid = FALSE;
    const UINT port = GetDlgItemInt(m_dialog, IDC_PORT, &valid, FALSE);
    if (!valid || port == 0 || port > 65535) {
        ui::ShowError(m_dialog, i18n::Str(IDS_ERR_INVALID_PORT));
        return;
    }
    const int index = static_cast<int>(SendDlgItemMessageW(m_dialog, IDC_ADDRESS, CB_GETCURSEL, 0, 0));
    const std::wstring addressText = ui::GetText(m_dialog, IDC_ADDRESS);
    const uint32_t address = index <= 0 ? INADDR_ANY : inet_addr(text::WideToUtf8(addressText).c_str());
    if (const int error = m_app.Server().Start(address, static_cast<uint16_t>(port))) {
        ui::ShowError(m_dialog, i18n::Format(IDS_ERR_START, text::ErrorMessage(static_cast<unsigned long>(error))));
        return;
    }
    m_app.running = true;
    m_app.addressIndex = index;
    m_app.addressText = addressText;
    m_app.port = static_cast<uint16_t>(port);
    UpdateUi();
}

void MainDialog::AddTarget()
{
    AddTargetDialog dialog(m_app);
    if (dialog.Run(m_dialog)) {
        RefreshTargets();
    }
}

void MainDialog::RemoveTarget()
{
    const int selected = ui::SelectedListRow(GetDlgItem(m_dialog, IDC_TARGETS));
    if (selected < 0 || static_cast<size_t>(selected) >= m_app.Targets().size()) {
        return;
    }
    const std::string name = m_app.Targets()[selected].target->Name();
    if (!m_app.RemoveTarget(static_cast<size_t>(selected))) {
        ui::ShowError(m_dialog, i18n::Format(IDS_ERR_TARGET_IN_USE, text::Utf8ToWide(name)));
        return;
    }
    RefreshTargets();
}

void MainDialog::OpenConfiguration(const std::wstring& path, bool start)
{
    config::ServerConfig config;
    if (!ReadConfiguration(m_dialog, path, config)) {
        return;
    }
    // Replacing the targets would cut off logged-in initiators.
    if (m_app.TotalSessions() > 0) {
        ui::ShowError(m_dialog, i18n::Str(IDS_ERR_CONFIG_SESSIONS));
        return;
    }
    // The address may change: restart the server on the new one.
    const bool run = start || m_app.running;
    if (m_app.running) {
        m_app.Server().Stop();
        m_app.running = false;
    }
    ApplyConfiguration(m_dialog, m_app, config);
    FillAddresses();
    SetDlgItemInt(m_dialog, IDC_PORT, m_app.port, FALSE);
    RefreshTargets();
    if (run) {
        StartOrStop();
    }
}

void MainDialog::SaveConfiguration()
{
    BOOL valid = FALSE;
    const UINT port = GetDlgItemInt(m_dialog, IDC_PORT, &valid, FALSE);
    if (!valid || port == 0 || port > 65535) {
        ui::ShowError(m_dialog, i18n::Str(IDS_ERR_INVALID_PORT));
        return;
    }
    const int index = static_cast<int>(SendDlgItemMessageW(m_dialog, IDC_ADDRESS, CB_GETCURSEL, 0, 0));
    const std::wstring listen = index <= 0 ? std::wstring() : ui::GetText(m_dialog, IDC_ADDRESS);
    if (auto path = ui::BrowseForFile(m_dialog, IDS_FILTER_CONFIG, true, L"conf")) {
        WriteConfiguration(m_dialog, m_app, listen, static_cast<uint16_t>(port), *path);
    }
}

bool MainDialog::ConfirmExit()
{
    return !m_app.running || m_app.TotalSessions() == 0 || ui::Confirm(m_dialog, i18n::Str(IDS_CONFIRM_EXIT));
}

} // namespace tiscsi
