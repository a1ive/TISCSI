// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/SCSI/SCSICommandDescriptorBlock).
// The C# class hierarchy (6/10/12/16-byte CDBs, Inquiry, Mode Sense) is
// flattened into one decoded structure. Of the MMC (CD/DVD) commands only the
// allocation length is decoded; their handlers read the other fields from the
// CDB bytes.

#pragma once

#include "Core/scsi/ScsiTypes.h"

#include <array>
#include <cstdint>
#include <optional>

namespace tiscsi::scsi {

struct Cdb {
    uint8_t opcode = 0;
    ServiceAction serviceAction = ServiceAction::None;
    uint64_t logicalBlockAddress = 0;
    // Number of blocks for READ/WRITE/VERIFY; allocation length for
    // INQUIRY, MODE SENSE, REQUEST SENSE, REPORT LUNS, READ CAPACITY (16)
    // and the MMC commands.
    uint32_t transferLength = 0;
    uint8_t control = 0;

    // INQUIRY
    bool evpd = false;
    VpdPage pageCode = VpdPage::SupportedVpdPages;

    // MODE SENSE (6) / (10)
    bool disableBlockDescriptors = false;
    uint8_t pageControl = 0;
    ModePageCode modePageCode = ModePageCode::VendorSpecific;
    uint8_t subpageCode = 0;

    // Returns std::nullopt for operation codes the target does not know.
    static std::optional<Cdb> Parse(const std::array<uint8_t, 16>& bytes);
};

} // namespace tiscsi::scsi
