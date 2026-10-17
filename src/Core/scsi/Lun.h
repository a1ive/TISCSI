// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/SCSI/LUNStructure.cs).
// 8-byte SAM LUN structure; only single-level addressing is supported.

#pragma once

#include "Core/util/ByteOrder.h"

#include <array>
#include <cstdint>
#include <optional>

namespace tiscsi::scsi {

using LunBytes = std::array<uint8_t, 8>;

constexpr uint16_t kSingleLevelAddressingLimit = 16384;

// Peripheral device addressing for LUN <= 255, flat space addressing above.
inline LunBytes EncodeLun(uint16_t lun)
{
    LunBytes bytes{};
    const uint16_t first = lun <= 255 ? lun : static_cast<uint16_t>(0x4000 | (lun & 0x3FFF));
    util::WriteBE16(bytes.data(), first);
    return bytes;
}

// Returns the logical unit number if the structure uses single-level addressing.
inline std::optional<uint16_t> DecodeSingleLevelLun(const LunBytes& bytes)
{
    const uint16_t first = util::ReadBE16(bytes.data());
    switch (first >> 14) {
    case 0: // peripheral device addressing; bus must be 0
        if (((first >> 8) & 0x3F) == 0) {
            return static_cast<uint16_t>(first & 0xFF);
        }
        return std::nullopt;
    case 1: // flat space addressing
        return static_cast<uint16_t>(first & 0x3FFF);
    default:
        return std::nullopt;
    }
}

} // namespace tiscsi::scsi
