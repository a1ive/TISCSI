// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSIServer.PDUProcessor.cs, ISCSIServer.TextRequest.cs,
// ISCSIServer.Logout.cs, TargetResponseHelper.cs, ServerResponseHelper.cs).

#include "Core/iscsi/IscsiServer.h"

#include "Core/scsi/ScsiData.h"
#include "Core/scsi/VirtualScsiTarget.h"

#include <algorithm>

namespace tiscsi::iscsi {

using util::Severity;

namespace {

bool IsSupportedInitiatorOpcode(OpCode opcode)
{
    switch (opcode) {
    case OpCode::NopOut:
    case OpCode::ScsiCommand:
    case OpCode::LoginRequest:
    case OpCode::TextRequest:
    case OpCode::ScsiDataOut:
    case OpCode::LogoutRequest:
        return true;
    default:
        return false;
    }
}

// RFC 3720: Vendor-specific keys start with "X-" (or "X#" if registered with IANA).
bool IsVendorSpecificRequest(const util::KeyValueList& request)
{
    return !request.empty() && std::all_of(request.begin(), request.end(), [](const auto& entry) {
        return entry.first.starts_with("X-") || entry.first.starts_with("X#");
    });
}

// Builds the PDUs that complete a SCSI command: a SCSI Response, or Data-In
// PDUs with status in the last one. Data beyond ExpectedDataTransferLength is
// cut off and reported as residual overflow.
std::vector<Pdu> PrepareScsiCommandResponse(uint32_t initiatorTaskTag, bool read, uint32_t expectedLength,
                                            scsi::ScsiStatus status, scsi::Bytes response,
                                            uint32_t maxRecvDataSegmentLength)
{
    std::vector<Pdu> result;
    if (!read || status != scsi::ScsiStatus::Good) {
        // Errors must be reported (with sense data) in a SCSI Response PDU.
        ScsiResponse pdu;
        pdu.initiatorTaskTag = initiatorTaskTag;
        pdu.status = static_cast<uint8_t>(status);
        pdu.data = std::move(response);
        if (read && expectedLength > 0) {
            // No read data was transferred.
            pdu.residualUnderflow = true;
            pdu.residualCount = expectedLength;
        }
        result.push_back(pdu.ToPdu());
        return result;
    }

    bool overflow = false;
    bool underflow = false;
    uint32_t residual = 0;
    if (response.size() > expectedLength) {
        overflow = true;
        residual = static_cast<uint32_t>(response.size() - expectedLength);
        response.resize(expectedLength);
    } else if (response.size() < expectedLength) {
        underflow = true;
        residual = static_cast<uint32_t>(expectedLength - response.size());
    }

    const size_t total = response.size();
    size_t offset = 0;
    uint32_t dataSN = 0;
    do {
        const size_t length = std::min<size_t>(maxRecvDataSegmentLength, total - offset);
        ScsiDataIn pdu;
        pdu.initiatorTaskTag = initiatorTaskTag;
        pdu.bufferOffset = static_cast<uint32_t>(offset);
        pdu.dataSN = dataSN++;
        pdu.data.assign(response.begin() + offset, response.begin() + offset + length);
        offset += length;
        if (offset == total) {
            pdu.final = true;
            pdu.statusPresent = true;
            pdu.status = static_cast<uint8_t>(status);
            pdu.residualOverflow = overflow;
            pdu.residualUnderflow = underflow;
            pdu.residualCount = residual;
        }
        result.push_back(pdu.ToPdu());
    } while (offset < total);
    return result;
}

} // namespace

void IscsiServer::SendReject(const ConnectionPtr& connection, RejectReason reason, const Pdu& rejected)
{
    Reject reject;
    reject.reason = reason;
    reject.rejectedHeader = rejected.bhs;
    connection->sendQueue.Enqueue(reject.ToPdu());
}

bool IscsiServer::ValidateCommandNumbering(const Pdu& pdu, const ConnectionPtr& connection)
{
    const SessionPtr session = connection->GetSession();
    if (!session) {
        return true;
    }
    const std::optional<uint32_t> cmdSN = GetCmdSN(pdu);
    m_log.Log(Severity::Verbose, "[{}] Received PDU from initiator, Operation: 0x{:02X}, Size: {}, CmdSN: {}",
              connection->Identifier(), static_cast<uint8_t>(pdu.Opcode()), kBasicHeaderSegmentLength + pdu.data.size(),
              cmdSN ? static_cast<int64_t>(*cmdSN) : -1);
    if (!cmdSN) {
        return true;
    }
    // RFC 3720: On any connection, the initiator MUST send commands in
    // increasing order of CmdSN (except for retransmissions).
    if (session->commandNumberingStarted) {
        if (*cmdSN != session->expCmdSN) {
            return false;
        }
    } else {
        session->expCmdSN = *cmdSN;
        session->commandNumberingStarted = true;
    }
    // Non-immediate commands consume a CmdSN.
    const OpCode opcode = pdu.Opcode();
    const bool consumesCmdSN = opcode == OpCode::ScsiCommand || opcode == OpCode::TextRequest ||
                               opcode == OpCode::LogoutRequest ||
                               (opcode == OpCode::NopOut && pdu.InitiatorTaskTag() != kReservedTag);
    if (consumesCmdSN && !pdu.Immediate()) {
        ++session->expCmdSN;
    }
    return true;
}

void IscsiServer::ProcessPdu(Pdu&& pdu, const ConnectionPtr& connection)
{
    const OpCode opcode = pdu.Opcode();
    if (!IsSupportedInitiatorOpcode(opcode)) {
        m_log.Log(Severity::Error, "[{}] Unsupported PDU (0x{:X})", connection->Identifier(),
                  static_cast<uint8_t>(opcode));
        SendReject(connection, RejectReason::CommandNotSupported, pdu);
        return;
    }
    if (!ValidateCommandNumbering(pdu, connection)) {
        m_log.Log(Severity::Warning, "[{}] Ignoring PDU with CmdSN outside of expected range", connection->Identifier());
        return;
    }

    if (!connection->isFullFeaturePhase) {
        if (opcode == OpCode::LoginRequest) {
            const LoginRequest request = LoginRequest::Parse(pdu);
            m_log.Log(Severity::Verbose,
                      "[ISID={:x},TSIH={:x},CID={:x}] Login Request, current stage: {}, next stage: {}, parameters: {}",
                      request.isid, request.tsih, request.cid, request.currentStage, request.nextStage,
                      util::KeyValueList::ParseNullDelimited(request.text).ToDisplayString());
            const LoginResponse response = GetLoginResponse(request, connection);
            if (connection->isFullFeaturePhase) {
                m_connectionManager.AddConnection(connection);
            }
            m_log.Log(Severity::Verbose, "[{}] Login Response parameters: {}", connection->Identifier(),
                      util::KeyValueList::ParseNullDelimited(response.text).ToDisplayString());
            connection->sendQueue.Enqueue(response.ToPdu());
            return;
        }
        // Before the full feature phase only Login Requests are allowed.
        m_log.Log(Severity::Warning, "[{}] Initiator error: Improper command during login phase, OpCode: 0x{:x}",
                  connection->Identifier(), static_cast<uint8_t>(opcode));
        if (const SessionPtr session = connection->GetSession()) {
            // Once login has started: Login reject ("invalid during login"), then disconnect.
            LoginResponse response;
            response.tsih = session->tsih;
            response.status = LoginStatus::InvalidDuringLogon;
            connection->sendQueue.Enqueue(response.ToPdu());
        } else {
            // Before login has started: terminate the connection immediately.
            connection->Abort();
        }
        return;
    }

    const SessionPtr session = connection->GetSession();
    if (opcode == OpCode::TextRequest) {
        connection->sendQueue.Enqueue(GetTextResponse(TextRequest::Parse(pdu), connection).ToPdu());
        return;
    }
    if (opcode == OpCode::LogoutRequest) {
        connection->sendQueue.Enqueue(GetLogoutResponse(LogoutRequest::Parse(pdu), pdu, connection));
        return;
    }
    if (session->isDiscovery) {
        // Discovery sessions accept only SendTargets text requests and logout.
        m_log.Log(Severity::Warning, "[{}] Initiator error: Improper command during discovery session, OpCode: 0x{:x}",
                  connection->Identifier(), static_cast<uint8_t>(opcode));
        SendReject(connection, RejectReason::ProtocolError, pdu);
        return;
    }

    switch (opcode) {
    case OpCode::NopOut: {
        NopOut request = NopOut::Parse(std::move(pdu));
        if (request.initiatorTaskTag != kReservedTag) {
            // Reply with the same ITT; the TTT of such a reply is reserved.
            NopIn response;
            response.initiatorTaskTag = request.initiatorTaskTag;
            response.lun = request.lun;
            response.data = std::move(request.data);
            connection->sendQueue.Enqueue(response.ToPdu());
        }
        break;
    }
    case OpCode::ScsiDataOut: {
        const Pdu header{pdu.bhs, {}, {}};
        ProcessDataOut(ScsiDataOut::Parse(std::move(pdu)), header, connection);
        break;
    }
    case OpCode::ScsiCommand:
        ProcessScsiCommand(ScsiCommand::Parse(std::move(pdu)), connection);
        break;
    case OpCode::LoginRequest:
        // A login phase must not occur on a connection that completed login.
        m_log.Log(Severity::Warning, "[{}] Initiator Error: Login request during full feature phase",
                  connection->Identifier());
        SendReject(connection, RejectReason::ProtocolError, pdu);
        break;
    default:
        SendReject(connection, RejectReason::CommandNotSupported, pdu);
        break;
    }
}

void IscsiServer::ProcessScsiCommand(ScsiCommand&& parsed, const ConnectionPtr& connection)
{
    auto command = std::make_shared<ScsiCommand>(std::move(parsed));
    const SessionPtr session = connection->GetSession();
    m_log.Log(Severity::Debug,
              "[{}] SCSICommandPDU: CmdSN: {}, LUN: {}, InitiatorTaskTag: {:x}, DataSegmentLength: {}, "
              "ExpectedDataTransferLength: {}, Final: {}",
              connection->Identifier(), command->cmdSN, scsi::DecodeSingleLevelLun(command->lun).value_or(0xFFFF),
              command->initiatorTaskTag, command->immediateDataLength, command->expectedDataTransferLength,
              command->final);

    const uint32_t expectedLength = command->expectedDataTransferLength;
    if (command->write && command->immediateDataLength < expectedLength) {
        if (expectedLength > scsi::VirtualScsiTarget::kMaximumTransferBytes) {
            m_log.Log(Severity::Error, "[{}] Write of {} bytes is too large", connection->Identifier(), expectedLength);
            ScsiResponse response;
            response.initiatorTaskTag = command->initiatorTaskTag;
            response.status = static_cast<uint8_t>(scsi::ScsiStatus::CheckCondition);
            response.data = scsi::FormatSenseData(scsi::SenseData::InvalidFieldInCdb());
            connection->sendQueue.Enqueue(response.ToPdu());
            return;
        }
        // The command runs once all of its data has arrived.
        const uint32_t transferTag = session->NextTransferTag();
        command->data.resize(expectedLength);

        uint32_t bytesLeft = expectedLength - command->immediateDataLength;
        uint32_t nextOffset = command->immediateDataLength;
        if (!session->initialR2T) {
            // Unsolicited Data-Out PDUs may fill the first burst.
            const uint32_t firstBurstEnd = std::min(static_cast<uint32_t>(session->firstBurstLength), expectedLength);
            const uint32_t unsolicited = firstBurstEnd > nextOffset ? firstBurstEnd - nextOffset : 0;
            bytesLeft -= unsolicited;
            nextOffset += unsolicited;
        }
        const auto burst = static_cast<uint32_t>(session->maxBurstLength);
        const uint32_t totalR2Ts = (bytesLeft + burst - 1) / burst;
        const uint32_t outgoingR2Ts = std::min(static_cast<uint32_t>(session->maxOutstandingR2T), totalR2Ts);

        for (uint32_t index = 0; index < outgoingR2Ts; ++index) {
            ReadyToTransfer r2t;
            r2t.lun = command->lun;
            r2t.initiatorTaskTag = command->initiatorTaskTag;
            r2t.r2tSN = index; // R2TSN starts at 0 for every command
            r2t.targetTransferTag = transferTag;
            r2t.bufferOffset = nextOffset;
            r2t.desiredDataTransferLength = std::min(burst, expectedLength - nextOffset);
            m_log.Log(Severity::Debug,
                      "[{}] Enqueued R2T, InitiatorTaskTag: {:x}, TargetTransferTag: {:x}, BufferOffset: {}, "
                      "DesiredDataTransferLength: {}, R2TSN: {}",
                      connection->Identifier(), r2t.initiatorTaskTag, r2t.targetTransferTag, r2t.bufferOffset,
                      r2t.desiredDataTransferLength, r2t.r2tSN);
            connection->sendQueue.Enqueue(r2t.ToPdu());
            nextOffset += burst;
        }
        connection->AddTransfer(command->initiatorTaskTag, transferTag,
                                TransferEntry{command, transferTag, outgoingR2Ts, nextOffset, totalR2Ts});
        session->commandsInTransfer.push_back(command->cmdSN);
        return;
    }

    // Commands must be executed in CmdSN order: e.g. a read must wait for a
    // preceding write whose data is still arriving.
    if (session->IsPrecedingCommandPending(command->cmdSN)) {
        session->delayedCommands.push_back(command);
    } else {
        ExecuteCommands({command}, connection);
    }
}

void IscsiServer::ProcessDataOut(ScsiDataOut&& request, const Pdu& header, const ConnectionPtr& connection)
{
    const SessionPtr session = connection->GetSession();
    m_log.Log(Severity::Debug,
              "[{}] SCSIDataOutPDU: InitiatorTaskTag: {:x}, TargetTransferTag: {:x}, BufferOffset: {}, "
              "DataSegmentLength: {}, DataSN: {}, Final: {}",
              connection->Identifier(), request.initiatorTaskTag, request.targetTransferTag, request.bufferOffset,
              request.data.size(), request.dataSN, request.final);

    TransferEntry* transfer = nullptr;
    if (request.targetTransferTag != kReservedTag) {
        transfer = connection->GetTransfer(request.targetTransferTag);
    } else if (!session->initialR2T) {
        // Unsolicited data carries a reserved TTT.
        transfer = connection->GetTransferByTaskTag(request.initiatorTaskTag);
    }
    const uint32_t total = transfer ? transfer->command->expectedDataTransferLength : 0;
    if (!transfer || request.bufferOffset > total || request.data.size() > total - request.bufferOffset) {
        m_log.Log(Severity::Warning, "[{}] Initiator error: Invalid TargetTransferTag or buffer range: {:x}",
                  connection->Identifier(), request.targetTransferTag);
        SendReject(connection, RejectReason::InvalidPduField, header);
        return;
    }

    std::copy(request.data.begin(), request.data.end(), transfer->command->data.begin() + request.bufferOffset);

    if (request.bufferOffset + request.data.size() == total) {
        // Last Data-Out PDU (DataPDUInOrder=Yes).
        std::shared_ptr<ScsiCommand> command = transfer->command;
        connection->RemoveTransfer(request.initiatorTaskTag, transfer->transferTag);
        std::erase(session->commandsInTransfer, command->cmdSN);
        if (session->IsPrecedingCommandPending(command->cmdSN)) {
            session->delayedCommands.push_back(command);
        } else {
            std::vector<std::shared_ptr<ScsiCommand>> commands{command};
            for (auto& ready : session->TakeDelayedCommandsReadyForExecution()) {
                commands.push_back(std::move(ready));
            }
            ExecuteCommands(commands, connection);
        }
        return;
    }

    // An R2T is outstanding until its last Data-Out PDU (F bit) arrives; then
    // send the next R2T, if any remain.
    if (request.final && transfer->nextR2TSN < transfer->totalR2Ts) {
        const auto burst = static_cast<uint32_t>(session->maxBurstLength);
        ReadyToTransfer r2t;
        r2t.lun = transfer->command->lun;
        r2t.initiatorTaskTag = request.initiatorTaskTag;
        r2t.targetTransferTag = transfer->transferTag;
        r2t.r2tSN = transfer->nextR2TSN;
        r2t.bufferOffset = transfer->nextOffset;
        r2t.desiredDataTransferLength = std::min(burst, total - transfer->nextOffset);
        connection->sendQueue.Enqueue(r2t.ToPdu());
        ++transfer->nextR2TSN;
        transfer->nextOffset += burst;
    }
}

void IscsiServer::ExecuteCommands(const std::vector<std::shared_ptr<ScsiCommand>>& commands,
                                  const ConnectionPtr& connection)
{
    if (commands.empty()) {
        return;
    }
    const std::shared_ptr<IscsiTarget> target = connection->GetSession()->target;
    connection->runningCommands.Add(static_cast<int>(commands.size()));
    for (const auto& command : commands) {
        m_log.Log(Severity::Debug, "[{}] Queuing command: CmdSN: {}, InitiatorTaskTag: {:x}", connection->Identifier(),
                  command->cmdSN, command->initiatorTaskTag);
        const uint32_t itt = command->initiatorTaskTag;
        const bool read = command->read;
        const uint32_t expectedLength = command->expectedDataTransferLength;
        target->QueueCommand(command->cdb, command->lun, std::move(command->data), read ? expectedLength : 0,
                             [connection, itt, read, expectedLength](scsi::ScsiStatus status, scsi::Bytes response) {
                                 // Enqueue before decrementing so a logout waiting for
                                 // in-flight commands is answered after their responses.
                                 connection->sendQueue.Enqueue(PrepareScsiCommandResponse(
                                     itt, read, expectedLength, status, std::move(response),
                                     connection->initiatorMaxRecvDataSegmentLength));
                                 connection->runningCommands.Decrement();
                             });
    }
}

TextResponse IscsiServer::GetTextResponse(const TextRequest& request, const ConnectionPtr& connection)
{
    TextResponse response;
    response.final = request.final;
    response.initiatorTaskTag = request.initiatorTaskTag;
    // A final Text Response carries the reserved TTT.
    response.targetTransferTag = request.final ? kReservedTag : 0;
    if (request.continueFlag) {
        connection->AddTextToSequence(request.initiatorTaskTag, request.text);
        return response;
    }

    const std::string text = connection->AddTextToSequence(request.initiatorTaskTag, request.text);
    connection->RemoveTextSequence(request.initiatorTaskTag);
    const util::KeyValueList parameters = util::KeyValueList::ParseNullDelimited(text);
    const SessionPtr session = connection->GetSession();
    util::KeyValueList result;
    // Text keys are case sensitive.
    if (parameters.Contains("SendTargets")) {
        // Only the targets this initiator may log in to.
        std::lock_guard lock(m_targetsMutex);
        for (const auto& target : m_targets) {
            if (target->access.AllowsInitiator(session->initiatorName)) {
                result.Add("TargetName", target->Name());
            }
        }
    } else if (session->isDiscovery || !IsVendorSpecificRequest(parameters)) {
        for (const auto& [key, value] : parameters) {
            result.Add(key, "Reject");
        }
    } else {
        // Vendor-specific keys are only valid in normal sessions; the target handles them.
        result = session->target->GetTextResponse(parameters);
    }
    response.text = result.ToNullDelimited();
    return response;
}

Pdu IscsiServer::GetLogoutResponse(const LogoutRequest& request, const Pdu& header, const ConnectionPtr& connection)
{
    m_log.Log(Severity::Verbose, "[{}] Logout Request", connection->Identifier());
    const SessionPtr session = connection->GetSession();
    auto reject = [&header] {
        Reject pdu;
        pdu.reason = RejectReason::ProtocolError;
        pdu.rejectedHeader = header.bhs;
        return pdu.ToPdu();
    };
    auto respond = [&request](LogoutResponseCode code) {
        LogoutResponse pdu;
        pdu.response = code;
        pdu.initiatorTaskTag = request.initiatorTaskTag;
        return pdu.ToPdu();
    };

    // Discovery sessions accept only "close the session".
    if (session->isDiscovery && request.reason != LogoutReason::CloseTheSession) {
        return reject();
    }

    std::vector<ConnectionPtr> connectionsToClose;
    switch (request.reason) {
    case LogoutReason::CloseTheSession:
        connectionsToClose = m_connectionManager.GetSessionConnections(session);
        break;
    case LogoutReason::CloseTheConnection:
        // The logout for a CID may arrive on another connection if the TCP
        // connection for that CID has already been terminated.
        if (ConnectionPtr existing = m_connectionManager.FindConnection(session, request.cid)) {
            connectionsToClose.push_back(std::move(existing));
        } else {
            return respond(LogoutResponseCode::CidNotFound);
        }
        break;
    case LogoutReason::RemoveTheConnectionForRecovery:
        return respond(LogoutResponseCode::ConnectionRecoveryNotSupported);
    default:
        return reject();
    }

    for (const ConnectionPtr& toClose : connectionsToClose) {
        toClose->runningCommands.WaitUntilZero(); // let pending I/O complete
        if (toClose != connection) {
            toClose->sendQueue.Stop();
            toClose->Abort();
        }
        m_connectionManager.RemoveConnection(toClose);
    }
    if (request.reason == LogoutReason::CloseTheSession) {
        m_log.Log(Severity::Verbose, "[{}] Session has been closed", session->Identifier());
        m_sessionManager.RemoveSession(session, SessionTerminationReason::Logout);
    }
    // The connection is closed after the Logout Response has been sent.
    return respond(LogoutResponseCode::ClosedSuccessfully);
}

} // namespace tiscsi::iscsi
