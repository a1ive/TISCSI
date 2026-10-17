// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/ISCSI.PDU). Target side only: PDUs sent by
// an initiator are parsed, PDUs sent by a target are built.
//
// All target-to-initiator PDUs carry StatSN / ExpCmdSN / MaxCmdSN at the same
// offsets (24 / 28 / 32); the connection's sender thread fills them in just
// before transmission (see SetStatSN / SetCommandWindow).

#pragma once

#include "Core/scsi/Lun.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tiscsi::iscsi {

constexpr size_t kBasicHeaderSegmentLength = 48;
constexpr uint32_t kReservedTag = 0xFFFFFFFF;

enum class OpCode : uint8_t {
    // Initiator opcodes
    NopOut = 0x00,
    ScsiCommand = 0x01,
    TaskManagementRequest = 0x02,
    LoginRequest = 0x03,
    TextRequest = 0x04,
    ScsiDataOut = 0x05,
    LogoutRequest = 0x06,
    SnackRequest = 0x10,
    // Target opcodes
    NopIn = 0x20,
    ScsiResponse = 0x21,
    TaskManagementResponse = 0x22,
    LoginResponse = 0x23,
    TextResponse = 0x24,
    ScsiDataIn = 0x25,
    LogoutResponse = 0x26,
    ReadyToTransfer = 0x31,
    AsyncMessage = 0x32,
    Reject = 0x3F,
};

enum class LoginStatus : uint16_t {
    Success = 0x0000,
    TargetMovedTemporarily = 0x0101,
    TargetMovedPermanently = 0x0102,
    InitiatorError = 0x0200,
    AuthenticationFailure = 0x0201,
    AuthorizationFailure = 0x0202,
    NotFound = 0x0203,
    TargetRemoved = 0x0204,
    UnsupportedVersion = 0x0205,
    TooManyConnections = 0x0206,
    MissingParameter = 0x0207,
    CanNotIncludeInSession = 0x0208,
    SessionTypeNotSupported = 0x0209,
    SessionDoesNotExist = 0x020A,
    InvalidDuringLogon = 0x020B,
    TargetError = 0x0300,
    ServiceUnavailable = 0x0301,
    OutOfResources = 0x0302,
};

enum class RejectReason : uint8_t {
    DataDigestError = 0x02,
    SnackReject = 0x03,
    ProtocolError = 0x04,
    CommandNotSupported = 0x05,
    ImmediateCommandReject = 0x06,
    TaskInProgress = 0x07,
    InvalidDataAck = 0x08,
    InvalidPduField = 0x09,
    LongOperationReject = 0x0A,
    NegotiationReset = 0x0B,
    WaitingForLogout = 0x0C,
};

enum class LogoutReason : uint8_t {
    CloseTheSession = 0,
    CloseTheConnection = 1,
    RemoveTheConnectionForRecovery = 2,
};

enum class LogoutResponseCode : uint8_t {
    ClosedSuccessfully = 0,
    CidNotFound = 1,
    ConnectionRecoveryNotSupported = 2,
    CleanupFailed = 3,
};

// A PDU as it appears on the wire (header and data digests are not supported).
struct Pdu {
    std::array<uint8_t, kBasicHeaderSegmentLength> bhs{};
    std::vector<uint8_t> ahs;
    std::vector<uint8_t> data;

    OpCode Opcode() const { return static_cast<OpCode>(bhs[0] & 0x3F); }
    bool Immediate() const { return (bhs[0] & 0x40) != 0; }
    bool Final() const { return (bhs[1] & 0x80) != 0; }
    uint32_t InitiatorTaskTag() const;

    // Header, AHS, data and padding to a 4-byte boundary.
    std::vector<uint8_t> Serialize() const;
};

// Lengths decoded from a basic header segment.
size_t TotalAhsLength(const uint8_t* bhs);
size_t DataSegmentLength(const uint8_t* bhs);
// AHS + data + padding that follow the 48-byte header.
size_t RemainingLength(const uint8_t* bhs);

// CmdSN of initiator PDUs that carry one.
std::optional<uint32_t> GetCmdSN(const Pdu& pdu);

void SetStatSN(Pdu& pdu, uint32_t statSN);
void SetCommandWindow(Pdu& pdu, uint32_t expCmdSN, uint32_t maxCmdSN);

// ---- Initiator to target -------------------------------------------------

struct LoginRequest {
    bool transit = false;
    bool continueFlag = false;
    uint8_t currentStage = 0;
    uint8_t nextStage = 0;
    uint8_t versionMax = 0;
    uint8_t versionMin = 0;
    uint64_t isid = 0; // 48 bits
    uint16_t tsih = 0;
    uint32_t initiatorTaskTag = 0;
    uint16_t cid = 0;
    uint32_t cmdSN = 0;
    uint32_t expStatSN = 0;
    std::string text;

    static LoginRequest Parse(const Pdu& pdu);
};

struct ScsiCommand {
    bool immediate = false;
    bool final = false;
    bool read = false;
    bool write = false;
    uint8_t taskAttributes = 0;
    scsi::LunBytes lun{};
    uint32_t initiatorTaskTag = 0;
    uint32_t expectedDataTransferLength = 0;
    uint32_t cmdSN = 0;
    uint32_t expStatSN = 0;
    std::array<uint8_t, 16> cdb{};
    // Immediate data; grows to the full write buffer while Data-Out PDUs arrive.
    std::vector<uint8_t> data;
    uint32_t immediateDataLength = 0;

    static ScsiCommand Parse(Pdu&& pdu);
};

struct ScsiDataOut {
    bool final = false;
    scsi::LunBytes lun{};
    uint32_t initiatorTaskTag = 0;
    uint32_t targetTransferTag = 0;
    uint32_t expStatSN = 0;
    uint32_t dataSN = 0;
    uint32_t bufferOffset = 0;
    std::vector<uint8_t> data;

    static ScsiDataOut Parse(Pdu&& pdu);
};

struct TextRequest {
    bool final = false;
    bool continueFlag = false;
    uint32_t initiatorTaskTag = 0;
    uint32_t targetTransferTag = 0;
    uint32_t cmdSN = 0;
    uint32_t expStatSN = 0;
    std::string text;

    static TextRequest Parse(const Pdu& pdu);
};

struct LogoutRequest {
    LogoutReason reason = LogoutReason::CloseTheSession;
    uint32_t initiatorTaskTag = 0;
    uint16_t cid = 0;
    uint32_t cmdSN = 0;
    uint32_t expStatSN = 0;

    static LogoutRequest Parse(const Pdu& pdu);
};

struct NopOut {
    scsi::LunBytes lun{};
    uint32_t initiatorTaskTag = 0;
    uint32_t targetTransferTag = 0;
    uint32_t cmdSN = 0;
    uint32_t expStatSN = 0;
    std::vector<uint8_t> data;

    static NopOut Parse(Pdu&& pdu);
};

// ---- Target to initiator -------------------------------------------------

struct LoginResponse {
    bool transit = false;
    bool continueFlag = false;
    uint8_t currentStage = 0;
    uint8_t nextStage = 0;
    uint8_t versionMax = 0;
    uint8_t versionActive = 0;
    uint64_t isid = 0;
    uint16_t tsih = 0;
    uint32_t initiatorTaskTag = 0;
    uint32_t expCmdSN = 0; // only needed before the session exists
    LoginStatus status = LoginStatus::Success;
    std::string text;

    Pdu ToPdu() const;
};

struct ScsiResponse {
    bool residualOverflow = false;
    bool residualUnderflow = false;
    uint8_t response = 0; // 0 = command completed at target
    uint8_t status = 0;   // SCSI status
    uint32_t initiatorTaskTag = 0;
    uint32_t expDataSN = 0;
    uint32_t residualCount = 0;
    std::vector<uint8_t> data; // sense data (with 2-byte length prefix)

    Pdu ToPdu() const;
};

struct ScsiDataIn {
    bool final = false;
    bool statusPresent = false;
    bool residualOverflow = false;
    bool residualUnderflow = false;
    uint8_t status = 0;
    uint32_t initiatorTaskTag = 0;
    uint32_t targetTransferTag = kReservedTag;
    uint32_t dataSN = 0;
    uint32_t bufferOffset = 0;
    uint32_t residualCount = 0;
    std::vector<uint8_t> data;

    Pdu ToPdu() const;
};

struct ReadyToTransfer {
    scsi::LunBytes lun{};
    uint32_t initiatorTaskTag = 0;
    uint32_t targetTransferTag = 0;
    uint32_t r2tSN = 0;
    uint32_t bufferOffset = 0;
    uint32_t desiredDataTransferLength = 0;

    Pdu ToPdu() const;
};

struct NopIn {
    scsi::LunBytes lun{};
    uint32_t initiatorTaskTag = kReservedTag;
    uint32_t targetTransferTag = kReservedTag;
    std::vector<uint8_t> data;

    Pdu ToPdu() const;
};

struct TextResponse {
    bool final = false;
    bool continueFlag = false;
    uint32_t initiatorTaskTag = 0;
    uint32_t targetTransferTag = kReservedTag;
    std::string text;

    Pdu ToPdu() const;
};

struct LogoutResponse {
    LogoutResponseCode response = LogoutResponseCode::ClosedSuccessfully;
    uint32_t initiatorTaskTag = 0;
    uint16_t timeToWait = 0;
    uint16_t timeToRetain = 0;

    Pdu ToPdu() const;
};

struct Reject {
    RejectReason reason = RejectReason::ProtocolError;
    // Header of the rejected PDU, returned as the data segment.
    std::array<uint8_t, kBasicHeaderSegmentLength> rejectedHeader{};

    Pdu ToPdu() const;
};

} // namespace tiscsi::iscsi
