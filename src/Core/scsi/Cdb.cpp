// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive

#include "Core/scsi/Cdb.h"

#include "Core/util/ByteOrder.h"

namespace tiscsi::scsi {

using util::ReadBE16;
using util::ReadBE24;
using util::ReadBE32;
using util::ReadBE64;

namespace {

void Parse6(const uint8_t* b, Cdb& cdb)
{
    cdb.logicalBlockAddress = ReadBE24(b + 1) & 0x1FFFFF;
    cdb.transferLength = b[4];
    cdb.control = b[5];
    // SBC: a transfer length of 0 in READ (6) / WRITE (6) means 256 blocks.
    if ((cdb.opcode == op::Read6 || cdb.opcode == op::Write6) && cdb.transferLength == 0) {
        cdb.transferLength = 256;
    }
}

void Parse10(const uint8_t* b, Cdb& cdb)
{
    cdb.serviceAction = static_cast<ServiceAction>(b[1] & 0x1F);
    cdb.logicalBlockAddress = ReadBE32(b + 2);
    cdb.transferLength = ReadBE16(b + 7);
    cdb.control = b[9];
}

void Parse12(const uint8_t* b, Cdb& cdb)
{
    cdb.serviceAction = static_cast<ServiceAction>(b[1] & 0x1F);
    cdb.logicalBlockAddress = ReadBE32(b + 2);
    cdb.transferLength = ReadBE32(b + 6);
    cdb.control = b[11];
}

void Parse16(const uint8_t* b, Cdb& cdb)
{
    cdb.serviceAction = static_cast<ServiceAction>(b[1] & 0x1F);
    cdb.logicalBlockAddress = ReadBE64(b + 2);
    cdb.transferLength = ReadBE32(b + 10);
    cdb.control = b[15];
}

} // namespace

std::optional<Cdb> Cdb::Parse(const std::array<uint8_t, 16>& bytes)
{
    const uint8_t* b = bytes.data();
    Cdb cdb;
    cdb.opcode = b[0];
    switch (cdb.opcode) {
    case op::TestUnitReady:
    case op::RequestSense:
    case op::Read6:
    case op::Write6:
    case op::Reserve6:
    case op::Release6:
        Parse6(b, cdb);
        break;
    case op::Inquiry:
        cdb.evpd = (b[1] & 0x01) != 0;
        cdb.pageCode = static_cast<VpdPage>(b[2]);
        cdb.transferLength = ReadBE16(b + 3);
        cdb.control = b[5];
        break;
    case op::ModeSense6:
        cdb.disableBlockDescriptors = (b[1] & 0x08) != 0;
        cdb.pageControl = b[2] >> 6;
        cdb.modePageCode = static_cast<ModePageCode>(b[2] & 0x3F);
        cdb.subpageCode = b[3];
        cdb.transferLength = b[4];
        cdb.control = b[5];
        break;
    case op::ReadCapacity10:
    case op::Read10:
    case op::Write10:
    case op::Verify10:
    case op::SynchronizeCache10:
    case op::WriteSame10:
        Parse10(b, cdb);
        break;
    case op::ReportLuns:
        Parse12(b, cdb);
        break;
    case op::Read16:
    case op::Write16:
    case op::Verify16:
    case op::WriteSame16:
    case op::ServiceActionIn16:
        Parse16(b, cdb);
        break;
    default:
        return std::nullopt;
    }
    return cdb;
}

} // namespace tiscsi::scsi
