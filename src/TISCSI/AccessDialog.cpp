// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "AccessDialog.h"

#include "Common/I18n.h"
#include "Common/Text.h"
#include "Core/config/ConfigFile.h"
#include "Core/iscsi/IscsiName.h"
#include "UiUtil.h"
#include "resource.h"

#include <string_view>
#include <utility>

namespace tiscsi {
namespace {

std::wstring_view Trim(std::wstring_view text)
{
    constexpr std::wstring_view kSpace = L" \t\r\n";
    const size_t first = text.find_first_not_of(kSpace);
    if (first == std::wstring_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

class AccessDialog {
public:
    explicit AccessDialog(iscsi::TargetAccess& access) : m_access(access) {}

    static INT_PTR CALLBACK DialogProc(HWND dialog, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        AccessDialog* self;
        if (msg == WM_INITDIALOG) {
            self = reinterpret_cast<AccessDialog*>(lParam);
            self->m_dialog = dialog;
            SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        } else {
            self = reinterpret_cast<AccessDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
        }
        return self ? self->HandleMessage(msg, wParam) : FALSE;
    }

private:
    INT_PTR HandleMessage(UINT msg, WPARAM wParam)
    {
        switch (msg) {
        case WM_INITDIALOG:
            OnInit();
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IDOK:
                if (Accept()) {
                    EndDialog(m_dialog, IDOK);
                }
                return TRUE;
            case IDCANCEL:
                EndDialog(m_dialog, IDCANCEL);
                return TRUE;
            }
            break;
        }
        return FALSE;
    }

    void OnInit()
    {
        std::wstring initiators;
        for (const std::string& name : m_access.allowedInitiators) {
            initiators += text::Utf8ToWide(name) + L"\r\n";
        }
        SetDlgItemTextW(m_dialog, IDC_ALLOWED_INITIATORS, initiators.c_str());
        for (const auto& [id, value] : {std::pair{IDC_CHAP_USER, &m_access.chapUser},
                                        {IDC_CHAP_SECRET, &m_access.chapSecret},
                                        {IDC_MUTUAL_USER, &m_access.mutualUser},
                                        {IDC_MUTUAL_SECRET, &m_access.mutualSecret}}) {
            SetDlgItemTextW(m_dialog, id, text::Utf8ToWide(*value).c_str());
        }
        SendDlgItemMessageW(m_dialog, IDC_CHAP_SECRET, EM_LIMITTEXT, config::kMaxChapSecret, 0);
        SendDlgItemMessageW(m_dialog, IDC_MUTUAL_SECRET, EM_LIMITTEXT, config::kMaxChapSecret, 0);
    }

    // The same checks as config::Validate and the chap-secret setting.
    bool Accept()
    {
        iscsi::TargetAccess access;
        const std::wstring initiators = ui::GetText(m_dialog, IDC_ALLOWED_INITIATORS);
        for (size_t start = 0; start <= initiators.size();) {
            size_t end = initiators.find(L'\n', start);
            if (end == std::wstring::npos) {
                end = initiators.size();
            }
            const std::wstring_view line = Trim(std::wstring_view(initiators).substr(start, end - start));
            start = end + 1;
            if (line.empty()) {
                continue;
            }
            const std::string name = text::WideToUtf8(line);
            if (!iscsi::IsValidIscsiName(name)) {
                return Fail(IDC_ALLOWED_INITIATORS, i18n::Format(IDS_ERR_CONFIG_INITIATOR_NAME, line));
            }
            access.allowedInitiators.push_back(name);
        }
        access.chapUser = text::WideToUtf8(Trim(ui::GetText(m_dialog, IDC_CHAP_USER)));
        access.mutualUser = text::WideToUtf8(Trim(ui::GetText(m_dialog, IDC_MUTUAL_USER)));
        // Secrets are taken as typed: spaces may be part of them.
        access.chapSecret = text::WideToUtf8(ui::GetText(m_dialog, IDC_CHAP_SECRET));
        access.mutualSecret = text::WideToUtf8(ui::GetText(m_dialog, IDC_MUTUAL_SECRET));
        for (const auto& [id, secret] :
             {std::pair{IDC_CHAP_SECRET, &access.chapSecret}, {IDC_MUTUAL_SECRET, &access.mutualSecret}}) {
            if (!secret->empty() && (secret->size() < config::kMinChapSecret || secret->size() > config::kMaxChapSecret)) {
                return Fail(id, i18n::Format(IDS_ERR_CHAP_SECRET_LENGTH, config::kMinChapSecret, config::kMaxChapSecret));
            }
        }
        if (access.chapSecret.empty() &&
            (!access.chapUser.empty() || !access.mutualUser.empty() || !access.mutualSecret.empty())) {
            return Fail(IDC_CHAP_SECRET, i18n::Str(IDS_ERR_CHAP_NO_SECRET));
        }
        if (!access.mutualUser.empty() && access.mutualSecret.empty()) {
            return Fail(IDC_MUTUAL_SECRET, i18n::Str(IDS_ERR_MUTUAL_NO_SECRET));
        }
        // RFC 3720 11.1.4: the two directions must use different secrets.
        if (!access.mutualSecret.empty() && access.mutualSecret == access.chapSecret) {
            return Fail(IDC_MUTUAL_SECRET, i18n::Str(IDS_ERR_MUTUAL_SAME_SECRET));
        }
        m_access = std::move(access);
        return true;
    }

    bool Fail(int controlId, const std::wstring& message)
    {
        ui::ShowError(m_dialog, message);
        SendMessageW(m_dialog, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(m_dialog, controlId)), TRUE);
        return false;
    }

    iscsi::TargetAccess& m_access;
    HWND m_dialog = nullptr;
};

} // namespace

bool EditTargetAccess(HWND owner, iscsi::TargetAccess& access)
{
    AccessDialog dialog(access);
    return i18n::DialogBoxParamL(IDD_TARGET_ACCESS, owner, AccessDialog::DialogProc,
                                 reinterpret_cast<LPARAM>(&dialog)) == IDOK;
}

std::wstring AccessSummary(const iscsi::TargetAccess& access)
{
    std::wstring chap;
    if (access.RequiresChap()) {
        chap = i18n::Str(access.mutualSecret.empty() ? IDS_ACCESS_CHAP : IDS_ACCESS_MUTUAL_CHAP);
    }
    if (access.allowedInitiators.empty()) {
        return chap.empty() ? i18n::Str(IDS_ACCESS_ANY) : chap;
    }
    const std::wstring initiators = i18n::Str(IDS_ACCESS_INITIATORS);
    return chap.empty() ? initiators : i18n::Format(IDS_ACCESS_BOTH, initiators, chap);
}

} // namespace tiscsi
