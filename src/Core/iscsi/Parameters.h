// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/ISCSI.Common/DefaultParameters.cs and
// ISCSI/ISCSI.Server/ISCSIServer.Parameters.cs).

#pragma once

#include <cstdint>

namespace tiscsi::iscsi {

// Operational parameter defaults defined by RFC 3720.
namespace defaults {
constexpr int kMaxConnections = 1;
constexpr bool kInitialR2T = true;
constexpr bool kImmediateData = true;
constexpr int kMaxBurstLength = 262144;
constexpr int kFirstBurstLength = 65536;
constexpr int kDefaultTime2Wait = 2;
constexpr int kDefaultTime2Retain = 20;
constexpr int kMaxOutstandingR2T = 1;
constexpr bool kDataPduInOrder = true;
constexpr bool kDataSequenceInOrder = true;
constexpr int kErrorRecoveryLevel = 0;
// Used by both sides until a value is declared during login.
constexpr int kMaxRecvDataSegmentLength = 8192;
} // namespace defaults

// Session parameters offered to the initiator.
namespace desired {
constexpr int kMaxConnections = 1; // implementation limit
constexpr bool kInitialR2T = false;
constexpr bool kImmediateData = true;
constexpr int kMaxBurstLength = defaults::kMaxBurstLength;
constexpr int kFirstBurstLength = defaults::kFirstBurstLength;
constexpr int kDefaultTime2Wait = 0;
constexpr int kDefaultTime2Retain = 20;
constexpr int kMaxOutstandingR2T = 16;
constexpr bool kDataPduInOrder = true;      // implementation limit
constexpr bool kDataSequenceInOrder = true; // implementation limit
constexpr int kErrorRecoveryLevel = 0;      // implementation limit
} // namespace desired

// Connection parameters declared by the target.
namespace declared {
constexpr int kMaxRecvDataSegmentLength = 262144;
} // namespace declared

// MaxCmdSN = ExpCmdSN + CommandQueueSize. 0 would allow one command at a time;
// most of the gain over a low-latency link comes from going from 0 to 1.
constexpr uint32_t kDefaultCommandQueueSize = 64;

} // namespace tiscsi::iscsi
