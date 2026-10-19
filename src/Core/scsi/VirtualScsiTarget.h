// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/SCSITarget/VirtualSCSITarget.cs).
// Serves a list of Disk objects as direct-access block devices, or as
// CD/DVD-ROM drives for optical disc images; LUN = index.

#pragma once

#include "Core/disk/Disk.h"
#include "Core/scsi/Cdb.h"
#include "Core/scsi/ScsiTarget.h"
#include "Core/util/Log.h"

#include <memory>
#include <vector>

namespace tiscsi::scsi {

class VirtualScsiTarget final : public ScsiTarget {
public:
    // Largest READ / WRITE accepted, in bytes (protects against huge allocations).
    static constexpr uint32_t kMaximumTransferBytes = 32 * 1024 * 1024;

    VirtualScsiTarget(std::vector<std::unique_ptr<disk::Disk>> disks, util::Logger logger);

    ScsiStatus ExecuteCommand(const CdbBytes& cdb, const LunBytes& lun, std::span<const uint8_t> data,
                              uint32_t dataInLength, Bytes& response) override;

    const std::vector<std::unique_ptr<disk::Disk>>& Disks() const { return m_disks; }

    // Distinguishes this target's logical units in device identifiers and
    // serial numbers (iSCSI Console reported the same IDs for every target).
    void SetDeviceIdSeed(uint32_t seed) { m_deviceIdSeed = seed; }

private:
    ScsiStatus Inquiry(const Cdb& cdb, uint16_t lun, Bytes& response);
    ScsiStatus ModeSense(const Cdb& cdb, uint16_t lun, Bytes& response);
    bool DiskModePages(const Cdb& cdb, Bytes& pages);
    bool MmcModePages(const Cdb& cdb, Bytes& pages);
    ScsiStatus Read(const Cdb& cdb, uint16_t lun, Bytes& response);
    ScsiStatus Write(const Cdb& cdb, uint16_t lun, std::span<const uint8_t> data, Bytes& response);
    ScsiStatus Verify(const Cdb& cdb, uint16_t lun, std::span<const uint8_t> data, Bytes& response);
    ScsiStatus SynchronizeCache(uint16_t lun, Bytes& response);
    // Commands of CD/DVD-ROM drives (MMC).
    ScsiStatus ExecuteMmcCommand(const Cdb& cdb, const CdbBytes& cdbBytes, uint16_t lun, Bytes& response);

    std::string SerialNumber(uint16_t lun) const;

    std::vector<std::unique_ptr<disk::Disk>> m_disks;
    util::Logger m_log;
    uint32_t m_deviceIdSeed = 0;
};

} // namespace tiscsi::scsi
