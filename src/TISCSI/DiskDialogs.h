// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Dialogs that produce one disk for a target (iSCSI Console's
// SelectDiskImageForm, CreateDiskImageForm, CreateRAMDiskForm,
// SelectPhysicalDiskForm and SelectVolumeForm).

#pragma once

#include "Core/config/ServerConfig.h"
#include "Core/disk/Disk.h"

#include <windows.h>

#include <memory>
#include <string>

namespace tiscsi {

struct DiskChoice {
    std::unique_ptr<disk::Disk> disk;
    std::wstring description; // shown in the target's disk list
    // How a saved configuration names the disk: devices by their stable
    // names, taken when the disk is chosen (disk numbers can change later).
    config::DiskSpec spec;
};

// Each returns false if cancelled. On success `choice` holds the opened disk.
bool ChooseDiskImage(HWND owner, DiskChoice& choice);
bool CreateVirtualDisk(HWND owner, DiskChoice& choice);
bool CreateRamDisk(HWND owner, DiskChoice& choice);
bool ChoosePhysicalDisk(HWND owner, DiskChoice& choice);
bool ChooseVolume(HWND owner, DiskChoice& choice);

} // namespace tiscsi
