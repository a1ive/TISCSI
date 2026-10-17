// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/SCSI/Enums).

#pragma once

#include <cstdint>

namespace tiscsi::scsi {

enum class ScsiStatus : uint8_t {
    Good = 0x00,
    CheckCondition = 0x02,
    ConditionMet = 0x04,
    Busy = 0x08,
    TaskAborted = 0x40,
};

// Operation codes handled or recognized by the target.
namespace op {
constexpr uint8_t TestUnitReady = 0x00;
constexpr uint8_t RequestSense = 0x03;
constexpr uint8_t Read6 = 0x08;
constexpr uint8_t Write6 = 0x0A;
constexpr uint8_t Inquiry = 0x12;
constexpr uint8_t Reserve6 = 0x16;
constexpr uint8_t Release6 = 0x17;
constexpr uint8_t ModeSense6 = 0x1A;
constexpr uint8_t ReadCapacity10 = 0x25;
constexpr uint8_t Read10 = 0x28;
constexpr uint8_t Write10 = 0x2A;
constexpr uint8_t Verify10 = 0x2F;
constexpr uint8_t SynchronizeCache10 = 0x35;
constexpr uint8_t WriteSame10 = 0x41;
constexpr uint8_t Read16 = 0x88;
constexpr uint8_t Write16 = 0x8A;
constexpr uint8_t Verify16 = 0x8F;
constexpr uint8_t WriteSame16 = 0x93;
constexpr uint8_t ServiceActionIn16 = 0x9E;
constexpr uint8_t ReportLuns = 0xA0;
} // namespace op

enum class ServiceAction : uint8_t {
    None = 0x00,
    ReadCapacity16 = 0x10,
    ReadLong16 = 0x11,
};

enum class VpdPage : uint8_t {
    SupportedVpdPages = 0x00,
    UnitSerialNumber = 0x80,
    DeviceIdentification = 0x83,
    BlockLimits = 0xB0,
    BlockDeviceCharacteristics = 0xB1,
};

enum class ModePageCode : uint8_t {
    VendorSpecific = 0x00, // requested by Windows 2000
    CachingParameters = 0x08,
    Control = 0x0A,
    PowerCondition = 0x1A,
    InformationalExceptionsControl = 0x1C,
    ReturnAllPages = 0x3F,
};

enum class PeripheralDeviceType : uint8_t {
    DirectAccessBlockDevice = 0x00,
    SequentialAccessDevice = 0x01,
    CdRomDevice = 0x05,
};

enum class VersionDescriptor : uint16_t {
    Iscsi = 0x0960,
};

} // namespace tiscsi::scsi
