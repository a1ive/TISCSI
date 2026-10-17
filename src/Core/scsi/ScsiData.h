// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/SCSI/SCSIReturnParameters): data returned
// by the device server. Only the fields the target actually sets are modeled;
// everything else is encoded as zero.

#pragma once

#include "Core/scsi/ScsiTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tiscsi::scsi {

using Bytes = std::vector<uint8_t>;

// Fixed-format sense data (18 bytes).
struct SenseData {
    uint8_t senseKey = 0;
    uint8_t additionalSenseCode = 0;
    uint8_t additionalSenseCodeQualifier = 0;

    Bytes GetBytes() const;

    static SenseData NoSense() { return {0x00, 0x00, 0x00}; }
    static SenseData IllegalRequest(uint8_t asc, uint8_t ascq = 0) { return {0x05, asc, ascq}; }
    static SenseData UnsupportedCommand() { return IllegalRequest(0x20); }
    static SenseData LbaOutOfRange() { return IllegalRequest(0x21); }
    static SenseData InvalidFieldInCdb() { return IllegalRequest(0x24); }
    static SenseData InvalidLun() { return IllegalRequest(0x25); }
    static SenseData DataProtect() { return {0x07, 0x27, 0x00}; }          // write protected
    static SenseData UnrecoveredReadError() { return {0x03, 0x11, 0x00}; } // medium error
    static SenseData WriteFault() { return {0x03, 0x03, 0x00}; }           // medium error
};

// Sense data as carried in an iSCSI SCSI Response: 2-byte length prefix.
Bytes FormatSenseData(const SenseData& sense);

struct StandardInquiryData {
    PeripheralDeviceType peripheralDeviceType = PeripheralDeviceType::DirectAccessBlockDevice;
    bool removable = false;
    uint8_t version = 5; // SPC-3
    bool cmdQue = false;
    std::string vendorIdentification;  // 8 chars
    std::string productIdentification; // 16 chars
    std::string productRevisionLevel;  // 4 chars
    uint64_t driveSerialNumber = 0;
    std::vector<VersionDescriptor> versionDescriptors; // up to 8

    Bytes GetBytes() const; // 96 bytes
};

enum class CodeSet : uint8_t { Binary = 1, Ascii = 2, Utf8 = 3 };
enum class Association : uint8_t { LogicalUnit = 0, TargetPort = 1, TargetDevice = 2 };
enum class ProtocolId : uint8_t { FibreChannel = 0, Iscsi = 5 };
enum class IdentifierType : uint8_t {
    VendorSpecific = 0x00,
    T10 = 0x01,
    Eui64 = 0x02,
    Naa = 0x03,
    ScsiNameString = 0x08,
};

struct IdentificationDescriptor {
    ProtocolId protocol = ProtocolId::FibreChannel;
    CodeSet codeSet = CodeSet::Binary;
    bool piv = false;
    Association association = Association::LogicalUnit;
    IdentifierType type = IdentifierType::VendorSpecific;
    Bytes identifier;

    Bytes GetBytes() const;

    static IdentificationDescriptor NaaExtended(uint32_t oui, uint32_t vendorSpecific);
    static IdentificationDescriptor ScsiNameString(const std::string& iscsiName);
};

// VPD pages
Bytes SupportedVpdPages(std::vector<uint8_t> pages);
Bytes UnitSerialNumberPage(const std::string& serialNumber);
Bytes DeviceIdentificationPage(const std::vector<IdentificationDescriptor>& descriptors);
Bytes BlockLimitsPage(uint16_t optimalTransferLengthGranularity, uint32_t maximumTransferLength,
                      uint32_t optimalTransferLength);
Bytes BlockDeviceCharacteristicsPage(uint16_t mediumRotationRate);

// Mode pages and mode parameter data
Bytes CachingParametersPage(bool readCacheDisable);
Bytes ControlModePage();
Bytes PowerConditionModePage();
Bytes PowerConsumptionModePage();
Bytes InformationalExceptionsControlModePage();
Bytes ModeParameterHeader6(uint8_t modeDataLength, bool writeProtect, bool dpoFua, uint8_t blockDescriptorLength);
Bytes ShortLbaBlockDescriptor(uint32_t numberOfBlocks, uint32_t logicalBlockLength);

// Capacity and LUN inventory
Bytes ReadCapacity10Data(uint64_t diskSize, uint32_t blockSize);
Bytes ReadCapacity16Data(uint64_t diskSize, uint32_t blockSize);
Bytes ReportLunsData(size_t lunCount);
Bytes ReportLunsData(const std::vector<uint16_t>& luns);

} // namespace tiscsi::scsi
