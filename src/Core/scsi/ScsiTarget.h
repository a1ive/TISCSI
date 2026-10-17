// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/SCSITarget/SCSITarget.cs and
// SCSITargetInterface.cs).

#pragma once

#include "Core/scsi/Lun.h"
#include "Core/scsi/ScsiData.h"
#include "Core/scsi/ScsiTypes.h"
#include "Core/util/BlockingQueue.h"

#include <array>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace tiscsi::scsi {

using CdbBytes = std::array<uint8_t, 16>;

// Lets the transport layer amend INQUIRY data (e.g. the iSCSI layer adds its
// version descriptor and SCSI name string). Called on the command thread.
struct InquiryHooks {
    std::function<void(uint16_t lun, StandardInquiryData&)> standardInquiry;
    std::function<void(uint16_t lun, std::string& serialNumber)> unitSerialNumber;
    std::function<void(uint16_t lun, std::vector<IdentificationDescriptor>&)> deviceIdentification;
};

// A SCSI device server. ExecuteCommand is called from a single thread at a
// time (see ScsiCommandQueue).
class ScsiTarget {
public:
    virtual ~ScsiTarget() = default;

    // `data` is the data-out buffer of a write. `dataInLength` is the number
    // of bytes the initiator expects to read (0 if the command is not a read);
    // targets that do not interpret CDBs themselves (pass-through) need it.
    virtual ScsiStatus ExecuteCommand(const CdbBytes& cdb, const LunBytes& lun, std::span<const uint8_t> data,
                                      uint32_t dataInLength, Bytes& response) = 0;

    void SetInquiryHooks(InquiryHooks hooks) { m_hooks = std::move(hooks); }

protected:
    InquiryHooks m_hooks;
};

using CommandCompletion = std::function<void(ScsiStatus status, Bytes response)>;

// Executes queued commands for one target on a dedicated worker thread, in
// the order they were queued. Must be destroyed before the target it runs.
class ScsiCommandQueue {
public:
    explicit ScsiCommandQueue(ScsiTarget& target);
    ~ScsiCommandQueue();
    ScsiCommandQueue(const ScsiCommandQueue&) = delete;
    ScsiCommandQueue& operator=(const ScsiCommandQueue&) = delete;

    void QueueCommand(const CdbBytes& cdb, const LunBytes& lun, std::vector<uint8_t> data, uint32_t dataInLength,
                      CommandCompletion done);

private:
    struct Command {
        CdbBytes cdb;
        LunBytes lun;
        std::vector<uint8_t> data;
        uint32_t dataInLength;
        CommandCompletion done;
    };

    void Run();

    ScsiTarget& m_target;
    util::BlockingQueue<Command> m_queue;
    std::thread m_worker;
};

} // namespace tiscsi::scsi
