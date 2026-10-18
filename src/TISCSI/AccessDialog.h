// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// "Access Control": who may log in to a target (allowed initiator names,
// CHAP and mutual CHAP), checked like the configuration file settings.

#pragma once

#include "Core/iscsi/IscsiTarget.h"

#include <windows.h>

#include <string>

namespace tiscsi {

// Edits `access`; false if the dialog was cancelled.
bool EditTargetAccess(HWND owner, iscsi::TargetAccess& access);

// Short description for the target list: "Any", "CHAP", "Initiator list +
// Mutual CHAP", ...
std::wstring AccessSummary(const iscsi::TargetAccess& access);

} // namespace tiscsi
