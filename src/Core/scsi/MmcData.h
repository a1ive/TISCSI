// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Data returned by a read-only CD/DVD-ROM drive (MMC) holding a single-session
// disc with one data track: an .iso image. Written from the MMC specification;
// iSCSI Console has no optical drive emulation.

#pragma once

#include "Core/scsi/ScsiData.h"

#include <cstdint>
#include <optional>

namespace tiscsi::scsi {

enum class MmcProfile : uint16_t {
    CdRom = 0x0008,
    DvdRom = 0x0010,
};

// A disc of up to 80 minutes (360000 sectors) is a CD, anything larger a DVD.
MmcProfile ProfileForDisc(uint64_t sectors);

// The functions returning std::optional return std::nullopt for requests
// with an invalid field in the CDB.

// READ TOC/PMA/ATIP formats 0 (TOC), 1 (session info) and 2 (raw TOC).
// `track` is the starting track (format 0) or session (format 2).
std::optional<Bytes> TocData(uint64_t sectors, uint8_t format, bool msf, uint8_t track);

// GET CONFIGURATION: `requestType` 0 = all features, 1 = current features,
// 2 = only `startingFeature`.
std::optional<Bytes> ConfigurationData(MmcProfile current, uint8_t requestType, uint16_t startingFeature);

// GET EVENT STATUS NOTIFICATION (polled): only the media class is
// supported, and the disc is always present.
Bytes EventStatusData(uint8_t notificationClassRequest);

Bytes DiscInformationData();

// READ TRACK INFORMATION: `addressType` 0 = LBA, 1 = track, 2 = session.
std::optional<Bytes> TrackInformationData(uint64_t sectors, uint8_t addressType, uint32_t address);

// READ DVD STRUCTURE formats 00h (physical), 01h (copyright: no protection),
// 04h (manufacturing information) and FFh (list of formats).
std::optional<Bytes> DvdStructureData(uint64_t sectors, uint8_t format, uint8_t layer);

Bytes MechanismStatusData();

// Mode pages
Bytes ReadWriteErrorRecoveryModePage();
Bytes MmCapabilitiesModePage();

} // namespace tiscsi::scsi
