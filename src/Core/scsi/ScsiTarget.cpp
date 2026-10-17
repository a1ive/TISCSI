// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive

#include "Core/scsi/ScsiTarget.h"

namespace tiscsi::scsi {

ScsiCommandQueue::ScsiCommandQueue(ScsiTarget& target) : m_target(target), m_worker([this] { Run(); }) {}

ScsiCommandQueue::~ScsiCommandQueue()
{
    m_queue.Stop();
    m_worker.join();
}

void ScsiCommandQueue::QueueCommand(const CdbBytes& cdb, const LunBytes& lun, std::vector<uint8_t> data,
                                    uint32_t dataInLength, CommandCompletion done)
{
    m_queue.Enqueue(Command{cdb, lun, std::move(data), dataInLength, std::move(done)});
}

void ScsiCommandQueue::Run()
{
    while (auto command = m_queue.Dequeue()) {
        Bytes response;
        const ScsiStatus status = m_target.ExecuteCommand(command->cdb, command->lun, command->data,
                                                           command->dataInLength, response);
        command->done(status, std::move(response));
    }
}

} // namespace tiscsi::scsi
