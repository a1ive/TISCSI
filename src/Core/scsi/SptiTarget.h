// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/Win32/SCSITarget/SPTITarget.cs and
// LogicalUnitManager.cs): forwards SCSI commands to a local device through
// the SCSI Pass-Through Interface (IOCTL_SCSI_PASS_THROUGH_DIRECT).
//
// Unlike the original, the data-in length comes from the initiator's
// ExpectedDataTransferLength, so CDBs do not have to be parsed and block
// sizes do not have to be tracked.

#pragma once

#include "Core/scsi/ScsiTarget.h"
#include "Core/util/Log.h"

#include <windows.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tiscsi::scsi {

class SptiTarget final : public ScsiTarget {
public:
    // Largest data transfer per command (also bounded by the adapter).
    static constexpr uint32_t kMaximumTransferBytes = 16 * 1024 * 1024;

    // `devicePath`: a class device such as \\.\CdRom0, \\.\Tape0 or
    // \\.\PhysicalDrive1 (served as LUN 0), or a SCSI adapter such as
    // \\.\Scsi2: (every device not claimed by a class driver gets a LUN).
    // Requires administrator rights. Returns nullptr and a Win32 error.
    static std::unique_ptr<SptiTarget> Open(const std::wstring& devicePath, util::Logger logger, DWORD& error);
    ~SptiTarget() override;

    ScsiStatus ExecuteCommand(const CdbBytes& cdb, const LunBytes& lun, std::span<const uint8_t> data,
                              uint32_t dataInLength, Bytes& response) override;

    const std::wstring& DevicePath() const { return m_path; }
    size_t LunCount() const { return m_luns.size(); }

private:
    struct Address {
        uint8_t pathId = 0;
        uint8_t targetId = 0;
        uint8_t lun = 0;
    };

    SptiTarget(HANDLE handle, std::wstring path, util::Logger logger);
    bool EnumerateAdapterDevices(DWORD& error);
    bool EnsureBuffer(size_t length);

    HANDLE m_handle;
    std::wstring m_path;
    util::Logger m_log;
    std::map<uint16_t, Address> m_luns;
    uint8_t* m_buffer = nullptr; // page aligned, satisfies any adapter alignment mask
    size_t m_bufferSize = 0;
};

} // namespace tiscsi::scsi
