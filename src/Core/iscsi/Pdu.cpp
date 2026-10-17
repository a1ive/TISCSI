// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive

#include "Core/iscsi/Pdu.h"

#include <algorithm>
#include <cstring>

namespace tiscsi::iscsi {

using util::ReadBE16;
using util::ReadBE24;
using util::ReadBE32;
using util::WriteBE16;
using util::WriteBE24;
using util::WriteBE32;

namespace {

// Byte offsets within the basic header segment.
constexpr size_t kOffsetLun = 8;
constexpr size_t kOffsetIsid = 8;
constexpr size_t kOffsetTsih = 14;
constexpr size_t kOffsetItt = 16;
constexpr size_t kOffsetStatSN = 24;
constexpr size_t kOffsetExpCmdSN = 28;
constexpr size_t kOffsetMaxCmdSN = 32;

Pdu MakePdu(OpCode opcode, uint8_t flags)
{
    Pdu pdu;
    pdu.bhs[0] = static_cast<uint8_t>(opcode);
    pdu.bhs[1] = flags;
    return pdu;
}

uint64_t ReadIsid(const uint8_t* p) { return static_cast<uint64_t>(ReadBE32(p)) << 16 | ReadBE16(p + 4); }

void WriteIsid(uint8_t* p, uint64_t isid)
{
    WriteBE32(p, static_cast<uint32_t>(isid >> 16));
    WriteBE16(p + 4, static_cast<uint16_t>(isid));
}

scsi::LunBytes ReadLun(const Pdu& pdu)
{
    scsi::LunBytes lun;
    std::copy_n(pdu.bhs.begin() + kOffsetLun, lun.size(), lun.begin());
    return lun;
}

void WriteLun(Pdu& pdu, const scsi::LunBytes& lun) { std::copy(lun.begin(), lun.end(), pdu.bhs.begin() + kOffsetLun); }

std::vector<uint8_t> ToBytes(const std::string& text) { return {text.begin(), text.end()}; }

std::string ToText(const std::vector<uint8_t>& data) { return {data.begin(), data.end()}; }

} // namespace

uint32_t Pdu::InitiatorTaskTag() const { return ReadBE32(&bhs[kOffsetItt]); }

std::vector<uint8_t> Pdu::Serialize() const
{
    const size_t length = kBasicHeaderSegmentLength + ahs.size() + data.size();
    std::vector<uint8_t> buffer((length + 3) & ~size_t{3}, 0);
    std::copy(bhs.begin(), bhs.end(), buffer.begin());
    buffer[4] = static_cast<uint8_t>(ahs.size() / 4);
    WriteBE24(&buffer[5], static_cast<uint32_t>(data.size()));
    std::copy(ahs.begin(), ahs.end(), buffer.begin() + kBasicHeaderSegmentLength);
    std::copy(data.begin(), data.end(), buffer.begin() + kBasicHeaderSegmentLength + ahs.size());
    return buffer;
}

size_t TotalAhsLength(const uint8_t* bhs) { return static_cast<size_t>(bhs[4]) * 4; }

size_t DataSegmentLength(const uint8_t* bhs) { return ReadBE24(bhs + 5); }

size_t RemainingLength(const uint8_t* bhs)
{
    const size_t length = TotalAhsLength(bhs) + DataSegmentLength(bhs);
    return (length + 3) & ~size_t{3};
}

std::optional<uint32_t> GetCmdSN(const Pdu& pdu)
{
    switch (pdu.Opcode()) {
    case OpCode::NopOut:
    case OpCode::ScsiCommand:
    case OpCode::LoginRequest:
    case OpCode::TextRequest:
    case OpCode::LogoutRequest:
        return ReadBE32(&pdu.bhs[24]);
    default:
        return std::nullopt;
    }
}

void SetStatSN(Pdu& pdu, uint32_t statSN) { WriteBE32(&pdu.bhs[kOffsetStatSN], statSN); }

void SetCommandWindow(Pdu& pdu, uint32_t expCmdSN, uint32_t maxCmdSN)
{
    WriteBE32(&pdu.bhs[kOffsetExpCmdSN], expCmdSN);
    WriteBE32(&pdu.bhs[kOffsetMaxCmdSN], maxCmdSN);
}

// ---- Initiator to target -------------------------------------------------

LoginRequest LoginRequest::Parse(const Pdu& pdu)
{
    const uint8_t* h = pdu.bhs.data();
    LoginRequest r;
    r.transit = (h[1] & 0x80) != 0;
    r.continueFlag = (h[1] & 0x40) != 0;
    r.currentStage = (h[1] >> 2) & 0x03;
    r.nextStage = h[1] & 0x03;
    r.versionMax = h[2];
    r.versionMin = h[3];
    r.isid = ReadIsid(h + kOffsetIsid);
    r.tsih = ReadBE16(h + kOffsetTsih);
    r.initiatorTaskTag = ReadBE32(h + kOffsetItt);
    r.cid = ReadBE16(h + 20);
    r.cmdSN = ReadBE32(h + 24);
    r.expStatSN = ReadBE32(h + 28);
    r.text = ToText(pdu.data);
    return r;
}

ScsiCommand ScsiCommand::Parse(Pdu&& pdu)
{
    const uint8_t* h = pdu.bhs.data();
    ScsiCommand r;
    r.immediate = pdu.Immediate();
    r.final = (h[1] & 0x80) != 0;
    r.read = (h[1] & 0x40) != 0;
    r.write = (h[1] & 0x20) != 0;
    r.taskAttributes = h[1] & 0x07;
    r.lun = ReadLun(pdu);
    r.initiatorTaskTag = ReadBE32(h + kOffsetItt);
    r.expectedDataTransferLength = ReadBE32(h + 20);
    r.cmdSN = ReadBE32(h + 24);
    r.expStatSN = ReadBE32(h + 28);
    std::copy_n(h + 32, r.cdb.size(), r.cdb.begin());
    r.data = std::move(pdu.data);
    r.immediateDataLength = static_cast<uint32_t>(r.data.size());
    return r;
}

ScsiDataOut ScsiDataOut::Parse(Pdu&& pdu)
{
    const uint8_t* h = pdu.bhs.data();
    ScsiDataOut r;
    r.final = (h[1] & 0x80) != 0;
    r.lun = ReadLun(pdu);
    r.initiatorTaskTag = ReadBE32(h + kOffsetItt);
    r.targetTransferTag = ReadBE32(h + 20);
    r.expStatSN = ReadBE32(h + 28);
    r.dataSN = ReadBE32(h + 36);
    r.bufferOffset = ReadBE32(h + 40);
    r.data = std::move(pdu.data);
    return r;
}

TextRequest TextRequest::Parse(const Pdu& pdu)
{
    const uint8_t* h = pdu.bhs.data();
    TextRequest r;
    r.final = (h[1] & 0x80) != 0;
    r.continueFlag = (h[1] & 0x40) != 0;
    r.initiatorTaskTag = ReadBE32(h + kOffsetItt);
    r.targetTransferTag = ReadBE32(h + 20);
    r.cmdSN = ReadBE32(h + 24);
    r.expStatSN = ReadBE32(h + 28);
    r.text = ToText(pdu.data);
    return r;
}

LogoutRequest LogoutRequest::Parse(const Pdu& pdu)
{
    const uint8_t* h = pdu.bhs.data();
    LogoutRequest r;
    r.reason = static_cast<LogoutReason>(h[1] & 0x7F);
    r.initiatorTaskTag = ReadBE32(h + kOffsetItt);
    r.cid = ReadBE16(h + 20);
    r.cmdSN = ReadBE32(h + 24);
    r.expStatSN = ReadBE32(h + 28);
    return r;
}

NopOut NopOut::Parse(Pdu&& pdu)
{
    const uint8_t* h = pdu.bhs.data();
    NopOut r;
    r.lun = ReadLun(pdu);
    r.initiatorTaskTag = ReadBE32(h + kOffsetItt);
    r.targetTransferTag = ReadBE32(h + 20);
    r.cmdSN = ReadBE32(h + 24);
    r.expStatSN = ReadBE32(h + 28);
    r.data = std::move(pdu.data);
    return r;
}

// ---- Target to initiator -------------------------------------------------

Pdu LoginResponse::ToPdu() const
{
    uint8_t flags = static_cast<uint8_t>((currentStage & 0x03) << 2 | (nextStage & 0x03));
    if (transit) {
        flags |= 0x80;
    }
    if (continueFlag) {
        flags |= 0x40;
    }
    Pdu pdu = MakePdu(OpCode::LoginResponse, flags);
    uint8_t* h = pdu.bhs.data();
    h[2] = versionMax;
    h[3] = versionActive;
    WriteIsid(h + kOffsetIsid, isid);
    WriteBE16(h + kOffsetTsih, tsih);
    WriteBE32(h + kOffsetItt, initiatorTaskTag);
    WriteBE32(h + kOffsetExpCmdSN, expCmdSN);
    WriteBE16(h + 36, static_cast<uint16_t>(status));
    pdu.data = ToBytes(text);
    return pdu;
}

Pdu ScsiResponse::ToPdu() const
{
    uint8_t flags = 0x80;
    if (residualOverflow) {
        flags |= 0x04;
    }
    if (residualUnderflow) {
        flags |= 0x02;
    }
    Pdu pdu = MakePdu(OpCode::ScsiResponse, flags);
    uint8_t* h = pdu.bhs.data();
    h[2] = response;
    h[3] = status;
    WriteBE32(h + kOffsetItt, initiatorTaskTag);
    WriteBE32(h + 36, expDataSN);
    WriteBE32(h + 44, residualCount);
    pdu.data = data;
    return pdu;
}

Pdu ScsiDataIn::ToPdu() const
{
    uint8_t flags = 0;
    // The S bit requires the F bit.
    if (final || statusPresent) {
        flags |= 0x80;
    }
    if (residualOverflow) {
        flags |= 0x04;
    }
    if (residualUnderflow) {
        flags |= 0x02;
    }
    if (statusPresent) {
        flags |= 0x01;
    }
    Pdu pdu = MakePdu(OpCode::ScsiDataIn, flags);
    uint8_t* h = pdu.bhs.data();
    h[3] = status;
    WriteBE32(h + kOffsetItt, initiatorTaskTag);
    WriteBE32(h + 20, targetTransferTag);
    WriteBE32(h + 36, dataSN);
    WriteBE32(h + 40, bufferOffset);
    WriteBE32(h + 44, residualCount);
    pdu.data = data;
    return pdu;
}

Pdu ReadyToTransfer::ToPdu() const
{
    Pdu pdu = MakePdu(OpCode::ReadyToTransfer, 0x80);
    uint8_t* h = pdu.bhs.data();
    WriteLun(pdu, lun);
    WriteBE32(h + kOffsetItt, initiatorTaskTag);
    WriteBE32(h + 20, targetTransferTag);
    WriteBE32(h + 36, r2tSN);
    WriteBE32(h + 40, bufferOffset);
    WriteBE32(h + 44, desiredDataTransferLength);
    return pdu;
}

Pdu NopIn::ToPdu() const
{
    Pdu pdu = MakePdu(OpCode::NopIn, 0x80);
    uint8_t* h = pdu.bhs.data();
    WriteLun(pdu, lun);
    WriteBE32(h + kOffsetItt, initiatorTaskTag);
    WriteBE32(h + 20, targetTransferTag);
    pdu.data = data;
    return pdu;
}

Pdu TextResponse::ToPdu() const
{
    uint8_t flags = 0;
    if (final) {
        flags |= 0x80;
    }
    if (continueFlag) {
        flags |= 0x40;
    }
    Pdu pdu = MakePdu(OpCode::TextResponse, flags);
    uint8_t* h = pdu.bhs.data();
    WriteBE32(h + kOffsetItt, initiatorTaskTag);
    WriteBE32(h + 20, targetTransferTag);
    pdu.data = ToBytes(text);
    return pdu;
}

Pdu LogoutResponse::ToPdu() const
{
    Pdu pdu = MakePdu(OpCode::LogoutResponse, 0x80);
    uint8_t* h = pdu.bhs.data();
    h[2] = static_cast<uint8_t>(response);
    WriteBE32(h + kOffsetItt, initiatorTaskTag);
    WriteBE16(h + 40, timeToWait);
    WriteBE16(h + 42, timeToRetain);
    return pdu;
}

Pdu Reject::ToPdu() const
{
    Pdu pdu = MakePdu(OpCode::Reject, 0x80);
    uint8_t* h = pdu.bhs.data();
    h[2] = static_cast<uint8_t>(reason);
    WriteBE32(h + kOffsetItt, kReservedTag);
    pdu.data.assign(rejectedHeader.begin(), rejectedHeader.end());
    return pdu;
}

} // namespace tiscsi::iscsi
