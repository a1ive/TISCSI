// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive

#include "Core/scsi/ScsiData.h"

#include "Core/scsi/Lun.h"
#include "Core/util/ByteOrder.h"

#include <algorithm>

namespace tiscsi::scsi {

using util::WriteBE16;
using util::WriteBE24;
using util::WriteBE32;
using util::WriteBE64;

Bytes SenseData::GetBytes() const
{
    if (information && *information > UINT32_MAX) {
        Bytes b(20, 0);
        b[0] = 0x72; // current errors, descriptor format
        b[1] = senseKey & 0x0F;
        b[2] = additionalSenseCode;
        b[3] = additionalSenseCodeQualifier;
        b[7] = 12;
        b[8] = 0x00; // information descriptor
        b[9] = 10;
        b[10] = 0x80; // valid
        WriteBE64(b.data() + 12, *information);
        return b;
    }
    Bytes b(18, 0);
    b[0] = 0x80 | 0x70; // valid, current errors
    b[2] = senseKey & 0x0F;
    if (information) {
        WriteBE32(b.data() + 3, static_cast<uint32_t>(*information));
    }
    b[7] = 10; // additional sense length
    b[12] = additionalSenseCode;
    b[13] = additionalSenseCodeQualifier;
    return b;
}

Bytes FormatSenseData(const SenseData& sense)
{
    const Bytes sd = sense.GetBytes();
    Bytes result(2 + sd.size());
    WriteBE16(result.data(), static_cast<uint16_t>(sd.size()));
    std::copy(sd.begin(), sd.end(), result.begin() + 2);
    return result;
}

Bytes StandardInquiryData::GetBytes() const
{
    Bytes b(96, 0);
    b[0] = static_cast<uint8_t>(peripheralDeviceType) & 0x1F;
    if (removable) {
        b[1] |= 0x80;
    }
    b[2] = version;
    b[3] = 2;  // response data format
    b[4] = 91; // additional length (96 - 5)
    if (cmdQue) {
        b[7] |= 0x02;
    }
    util::WriteAsciiField(&b[8], vendorIdentification, 8);
    util::WriteAsciiField(&b[16], productIdentification, 16);
    util::WriteAsciiField(&b[32], productRevisionLevel, 4);
    WriteBE64(&b[36], driveSerialNumber);
    for (size_t i = 0; i < 8 && i < versionDescriptors.size(); ++i) {
        WriteBE16(&b[58 + i * 2], static_cast<uint16_t>(versionDescriptors[i]));
    }
    return b;
}

Bytes IdentificationDescriptor::GetBytes() const
{
    Bytes b(4 + identifier.size(), 0);
    b[0] = static_cast<uint8_t>(static_cast<uint8_t>(protocol) << 4 | (static_cast<uint8_t>(codeSet) & 0x0F));
    b[1] = static_cast<uint8_t>((piv ? 0x80 : 0) | (static_cast<uint8_t>(association) & 0x03) << 4 |
                                (static_cast<uint8_t>(type) & 0x0F));
    b[3] = static_cast<uint8_t>(identifier.size());
    std::copy(identifier.begin(), identifier.end(), b.begin() + 4);
    return b;
}

IdentificationDescriptor IdentificationDescriptor::NaaExtended(uint32_t oui, uint32_t vendorSpecific)
{
    IdentificationDescriptor d;
    d.codeSet = CodeSet::Binary;
    d.type = IdentifierType::Naa;
    d.identifier.assign(8, 0);
    d.identifier[0] = 0x02 << 4; // NAA IEEE Extended
    WriteBE24(&d.identifier[2], oui);
    WriteBE24(&d.identifier[5], vendorSpecific);
    return d;
}

IdentificationDescriptor IdentificationDescriptor::ScsiNameString(const std::string& iscsiName)
{
    IdentificationDescriptor d;
    d.codeSet = CodeSet::Ascii;
    d.type = IdentifierType::ScsiNameString;
    d.association = Association::TargetDevice;
    d.protocol = ProtocolId::Iscsi;
    d.identifier.assign(iscsiName.begin(), iscsiName.end());
    return d;
}

Bytes SupportedVpdPages(std::vector<uint8_t> pages)
{
    std::sort(pages.begin(), pages.end()); // must be in ascending order
    Bytes b(4 + pages.size(), 0);
    b[1] = static_cast<uint8_t>(VpdPage::SupportedVpdPages);
    b[3] = static_cast<uint8_t>(pages.size());
    std::copy(pages.begin(), pages.end(), b.begin() + 4);
    return b;
}

Bytes UnitSerialNumberPage(const std::string& serialNumber)
{
    Bytes b(4 + serialNumber.size(), 0);
    b[1] = static_cast<uint8_t>(VpdPage::UnitSerialNumber);
    b[3] = static_cast<uint8_t>(serialNumber.size());
    std::copy(serialNumber.begin(), serialNumber.end(), b.begin() + 4);
    return b;
}

Bytes DeviceIdentificationPage(const std::vector<IdentificationDescriptor>& descriptors)
{
    Bytes b(4, 0);
    b[1] = static_cast<uint8_t>(VpdPage::DeviceIdentification);
    for (const IdentificationDescriptor& d : descriptors) {
        const Bytes bytes = d.GetBytes();
        b.insert(b.end(), bytes.begin(), bytes.end());
    }
    WriteBE16(&b[2], static_cast<uint16_t>(b.size() - 4));
    return b;
}

Bytes BlockLimitsPage(uint16_t optimalTransferLengthGranularity, uint32_t maximumTransferLength,
                      uint32_t optimalTransferLength)
{
    Bytes b(4 + 12, 0);
    b[1] = static_cast<uint8_t>(VpdPage::BlockLimits);
    b[3] = 12;
    WriteBE16(&b[6], optimalTransferLengthGranularity);
    WriteBE32(&b[8], maximumTransferLength);
    WriteBE32(&b[12], optimalTransferLength);
    return b;
}

Bytes BlockDeviceCharacteristicsPage(uint16_t mediumRotationRate)
{
    Bytes b(4 + 4, 0);
    b[1] = static_cast<uint8_t>(VpdPage::BlockDeviceCharacteristics);
    b[3] = 4;
    WriteBE16(&b[4], mediumRotationRate);
    return b;
}

namespace {

// page_0 mode page format: page code, page length, parameters.
Bytes ModePage0(ModePageCode code, uint8_t pageLength)
{
    Bytes b(2 + pageLength, 0);
    b[0] = static_cast<uint8_t>(code) & 0x3F;
    b[1] = pageLength;
    return b;
}

} // namespace

Bytes CachingParametersPage(bool readCacheDisable)
{
    Bytes b = ModePage0(ModePageCode::CachingParameters, 18);
    if (readCacheDisable) {
        b[2] |= 0x01;
    }
    return b;
}

Bytes ControlModePage() { return ModePage0(ModePageCode::Control, 10); }

Bytes PowerConditionModePage() { return ModePage0(ModePageCode::PowerCondition, 38); }

Bytes PowerConsumptionModePage()
{
    // sub_page format: SPF bit, subpage 01h, 2-byte page length.
    Bytes b(4 + 12, 0);
    b[0] = 0x40 | (static_cast<uint8_t>(ModePageCode::PowerCondition) & 0x3F);
    b[1] = 0x01;
    WriteBE16(&b[2], 12);
    return b;
}

Bytes InformationalExceptionsControlModePage() { return ModePage0(ModePageCode::InformationalExceptionsControl, 10); }

Bytes ModeParameterHeader6(uint8_t modeDataLength, bool writeProtect, bool dpoFua, uint8_t blockDescriptorLength)
{
    Bytes b(4, 0);
    b[0] = modeDataLength;
    if (writeProtect) {
        b[2] |= 0x80;
    }
    if (dpoFua) {
        b[2] |= 0x10;
    }
    b[3] = blockDescriptorLength;
    return b;
}

Bytes ModeParameterHeader10(uint16_t modeDataLength, bool writeProtect, bool dpoFua, uint16_t blockDescriptorLength)
{
    Bytes b(8, 0);
    WriteBE16(&b[0], modeDataLength);
    if (writeProtect) {
        b[3] |= 0x80;
    }
    if (dpoFua) {
        b[3] |= 0x10;
    }
    WriteBE16(&b[6], blockDescriptorLength);
    return b;
}

Bytes ShortLbaBlockDescriptor(uint32_t numberOfBlocks, uint32_t logicalBlockLength)
{
    Bytes b(8, 0);
    WriteBE32(&b[0], numberOfBlocks);
    WriteBE24(&b[5], logicalBlockLength);
    return b;
}

Bytes ReadCapacity10Data(uint64_t diskSize, uint32_t blockSize)
{
    // Disks too large for 32 bits report 0xFFFFFFFF so the initiator uses READ CAPACITY (16).
    const uint64_t lastLba = diskSize / blockSize - 1;
    Bytes b(8, 0);
    WriteBE32(&b[0], lastLba <= 0xFFFFFFFF ? static_cast<uint32_t>(lastLba) : 0xFFFFFFFF);
    WriteBE32(&b[4], blockSize);
    return b;
}

Bytes ReadCapacity16Data(uint64_t diskSize, uint32_t blockSize)
{
    Bytes b(32, 0);
    WriteBE64(&b[0], diskSize / blockSize - 1);
    WriteBE32(&b[8], blockSize);
    return b;
}

Bytes ReportLunsData(size_t lunCount)
{
    std::vector<uint16_t> luns(std::min<size_t>(lunCount, kSingleLevelAddressingLimit));
    for (size_t i = 0; i < luns.size(); ++i) {
        luns[i] = static_cast<uint16_t>(i);
    }
    return ReportLunsData(luns);
}

Bytes ReportLunsData(const std::vector<uint16_t>& luns)
{
    Bytes b(8 + luns.size() * 8, 0);
    WriteBE32(&b[0], static_cast<uint32_t>(luns.size() * 8));
    for (size_t i = 0; i < luns.size(); ++i) {
        const LunBytes lun = EncodeLun(luns[i]);
        std::copy(lun.begin(), lun.end(), b.begin() + 8 + i * 8);
    }
    return b;
}

} // namespace tiscsi::scsi
