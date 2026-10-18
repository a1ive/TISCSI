// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// "Add iSCSI Target" (iSCSI Console's AddTargetForm): a target name and the
// disks it serves.

#pragma once

#include "App.h"
#include "Core/disk/Disk.h"
#include "DiskDialogs.h"

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace tiscsi {

class AddTargetDialog {
public:
    explicit AddTargetDialog(App& app) : m_app(app) {}
    // True if a target was added to the app.
    bool Run(HWND owner);

private:
    static INT_PTR CALLBACK DialogProc(HWND dialog, UINT msg, WPARAM wParam, LPARAM lParam);
    INT_PTR HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnInit();
    void AddDisk(DiskChoice choice);
    void RemoveDisk();
    bool Accept();
    void UpdateUi();

    App& m_app;
    HWND m_dialog = nullptr;
    std::vector<std::unique_ptr<disk::Disk>> m_disks; // released if the dialog is cancelled
    std::vector<config::DiskSpec> m_specs;          // one per disk
};

} // namespace tiscsi
