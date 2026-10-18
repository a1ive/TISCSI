// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Main window (iSCSI Console's MainForm): listening address and port,
// start / stop, and the list of targets.

#pragma once

#include "App.h"

#include <windows.h>

#include <string>

namespace tiscsi {

class MainDialog {
public:
    // Returned by Run() when the language changed and the dialog must be
    // recreated from the other language's template.
    static constexpr INT_PTR kRecreate = 2;

    MainDialog(App& app, std::wstring& languageSetting) : m_app(app), m_languageSetting(languageSetting) {}
    INT_PTR Run();

private:
    static INT_PTR CALLBACK DialogProc(HWND dialog, UINT msg, WPARAM wParam, LPARAM lParam);
    INT_PTR HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnInit();
    void OnCommand(UINT id);
    void BuildMenu();
    void FillAddresses();
    void RefreshTargets();
    void UpdateUi();
    void StartOrStop();
    void AddTarget();
    void RemoveTarget();
    // Replaces the targets and address; restarts the server if it was
    // running, or starts it if `start`.
    void OpenConfiguration(const std::wstring& path, bool start);
    void SaveConfiguration();
    bool ConfirmExit();

    App& m_app;
    std::wstring& m_languageSetting;
    HWND m_dialog = nullptr;
    HWND m_status = nullptr;
    HICON m_iconLarge = nullptr;
    HICON m_iconSmall = nullptr;
};

INT_PTR CALLBACK AboutDialogProc(HWND dialog, UINT msg, WPARAM wParam, LPARAM lParam);

} // namespace tiscsi
