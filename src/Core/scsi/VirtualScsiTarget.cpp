// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive

#include "Core/scsi/VirtualScsiTarget.h"

#include <algorithm>
#include <format>

namespace tiscsi::scsi {

using util::Severity;

namespace {

// OUI used by iSCSI Console in its NAA identifiers.
constexpr uint32_t kNaaOui = 5;
constexpr uint32_t kMaximumTransferSizeLba = 2048;

void TruncateTo(Bytes& response, uint32_t allocationLength)
{
    if (response.size() > allocationLength) {
        response.resize(allocationLength);
    }
}

ScsiStatus CheckCondition(Bytes& response, const SenseData& sense)
{
    response = FormatSenseData(sense);
    return ScsiStatus::CheckCondition;
}

} // namespace

VirtualScsiTarget::VirtualScsiTarget(std::vector<std::unique_ptr<disk::Disk>> disks, util::Logger logger)
    : m_disks(std::move(disks)), m_log(std::move(logger))
{
}

ScsiStatus VirtualScsiTarget::ExecuteCommand(const CdbBytes& cdbBytes, const LunBytes& lunBytes,
                                             std::span<const uint8_t> data, uint32_t /*dataInLength*/,
                                             Bytes& response)
{
    response.clear();
    const std::optional<Cdb> parsed = Cdb::Parse(cdbBytes);
    if (!parsed) {
        m_log.Log(Severity::Error, "Unsupported SCSI Command (0x{:X})", cdbBytes[0]);
        return CheckCondition(response, SenseData::UnsupportedCommand());
    }
    const Cdb& cdb = *parsed;
    m_log.Log(Severity::Verbose, "Executing command: 0x{:02X}", cdb.opcode);

    if (cdb.opcode == op::ReportLuns) {
        response = ReportLunsData(m_disks.size());
        TruncateTo(response, cdb.transferLength);
        return ScsiStatus::Good;
    }

    const std::optional<uint16_t> lunNumber = DecodeSingleLevelLun(lunBytes);
    if (!lunNumber || *lunNumber >= m_disks.size()) {
        m_log.Log(Severity::Warning, "Initiator error: tried to execute command on LUN {} which does not exist",
                  lunNumber ? static_cast<int>(*lunNumber) : -1);
        return CheckCondition(response, SenseData::InvalidLun());
    }
    const uint16_t lun = *lunNumber;
    disk::Disk& disk = *m_disks[lun];

    switch (cdb.opcode) {
    case op::TestUnitReady:
    case op::Reserve6:
    case op::Release6:
    case op::Verify10:
    case op::Verify16:
        return ScsiStatus::Good;
    case op::RequestSense:
        // Some initiators (e.g. EFI iSCSI DXE) send REQUEST SENSE right after connecting.
        response = FormatSenseData(SenseData::NoSense());
        TruncateTo(response, cdb.transferLength);
        return ScsiStatus::Good;
    case op::Inquiry:
        return Inquiry(cdb, lun, response);
    case op::ModeSense6:
        return ModeSense6(cdb, lun, response);
    case op::ReadCapacity10:
        response = ReadCapacity10Data(disk.Size(), disk.BytesPerSector());
        return ScsiStatus::Good;
    case op::Read6:
    case op::Read10:
    case op::Read16:
        return Read(cdb, lun, response);
    case op::Write6:
    case op::Write10:
    case op::Write16:
        return Write(cdb, lun, data, response);
    case op::SynchronizeCache10:
        return SynchronizeCache(lun, response);
    case op::ServiceActionIn16:
        if (cdb.serviceAction == ServiceAction::ReadCapacity16) {
            response = ReadCapacity16Data(disk.Size(), disk.BytesPerSector());
            TruncateTo(response, cdb.transferLength);
            return ScsiStatus::Good;
        }
        break;
    }
    m_log.Log(Severity::Error, "Unsupported SCSI Command (0x{:X})", cdb.opcode);
    return CheckCondition(response, SenseData::UnsupportedCommand());
}

ScsiStatus VirtualScsiTarget::Inquiry(const Cdb& cdb, uint16_t lun, Bytes& response)
{
    if (!cdb.evpd) {
        if (static_cast<uint8_t>(cdb.pageCode) != 0) {
            m_log.Write(Severity::Warning, "Initiator error: Invalid Inquiry request");
            return CheckCondition(response, SenseData::InvalidFieldInCdb());
        }
        StandardInquiryData inquiry;
        inquiry.peripheralDeviceType = PeripheralDeviceType::DirectAccessBlockDevice;
        inquiry.vendorIdentification = "TISCSI";
        inquiry.productIdentification = std::format("SCSI Disk {}", lun);
        inquiry.productRevisionLevel = "1.00";
        inquiry.cmdQue = true;
        inquiry.version = 5; // SPC-3
        if (m_hooks.standardInquiry) {
            m_hooks.standardInquiry(lun, inquiry);
        }
        response = inquiry.GetBytes();
    } else {
        m_log.Log(Severity::Verbose, "Inquiry: Page code: 0x{:X}", static_cast<uint8_t>(cdb.pageCode));
        switch (cdb.pageCode) {
        case VpdPage::SupportedVpdPages:
            response = SupportedVpdPages({static_cast<uint8_t>(VpdPage::SupportedVpdPages),
                                          static_cast<uint8_t>(VpdPage::UnitSerialNumber),
                                          static_cast<uint8_t>(VpdPage::DeviceIdentification)});
            break;
        case VpdPage::UnitSerialNumber: {
            std::string serial = SerialNumber(lun);
            if (m_hooks.unitSerialNumber) {
                m_hooks.unitSerialNumber(lun, serial);
            }
            response = UnitSerialNumberPage(serial);
            break;
        }
        case VpdPage::DeviceIdentification: {
            // An NAA identifier is needed to prevent a 0xF4 BSOD during Windows setup.
            std::vector<IdentificationDescriptor> descriptors{
                IdentificationDescriptor::NaaExtended(kNaaOui, (m_deviceIdSeed + lun) & 0xFFFFFF)};
            if (m_hooks.deviceIdentification) {
                m_hooks.deviceIdentification(lun, descriptors);
            }
            response = DeviceIdentificationPage(descriptors);
            break;
        }
        case VpdPage::BlockLimits: // only provided when requested explicitly
            response = BlockLimitsPage(128, kMaximumTransferSizeLba, 128);
            break;
        case VpdPage::BlockDeviceCharacteristics: // only provided when requested explicitly
            response = BlockDeviceCharacteristicsPage(0);
            break;
        default:
            m_log.Log(Severity::Error, "Inquiry: Unsupported VPD Page request (0x{:X})",
                      static_cast<uint8_t>(cdb.pageCode));
            return CheckCondition(response, SenseData::InvalidFieldInCdb());
        }
    }
    TruncateTo(response, cdb.transferLength);
    return ScsiStatus::Good;
}

ScsiStatus VirtualScsiTarget::ModeSense6(const Cdb& cdb, uint16_t lun, Bytes& response)
{
    m_log.Log(Severity::Verbose, "ModeSense6: Page code: 0x{:X}, Sub page code: 0x{:X}",
              static_cast<uint8_t>(cdb.modePageCode), cdb.subpageCode);
    Bytes pages;
    switch (cdb.modePageCode) {
    case ModePageCode::CachingParameters:
        pages = CachingParametersPage(true);
        break;
    case ModePageCode::Control:
        pages = ControlModePage();
        break;
    case ModePageCode::PowerCondition:
        if (cdb.subpageCode == 0x00) {
            pages = PowerConditionModePage();
        } else if (cdb.subpageCode == 0x01) {
            pages = PowerConsumptionModePage();
        } else {
            m_log.Log(Severity::Error, "ModeSense6: Power condition subpage 0x{:x} is not implemented",
                      cdb.subpageCode);
            return CheckCondition(response, SenseData::InvalidFieldInCdb());
        }
        break;
    case ModePageCode::InformationalExceptionsControl:
        pages = InformationalExceptionsControlModePage();
        break;
    case ModePageCode::ReturnAllPages: {
        pages = CachingParametersPage(true);
        const Bytes iec = InformationalExceptionsControlModePage();
        pages.insert(pages.end(), iec.begin(), iec.end());
        break;
    }
    case ModePageCode::VendorSpecific:
        // Windows 2000 requests this page; Microsoft iSCSI Target returns it empty.
        break;
    default:
        m_log.Log(Severity::Error, "ModeSense6: ModeSense6 page 0x{:x} is not implemented",
                  static_cast<uint8_t>(cdb.modePageCode));
        return CheckCondition(response, SenseData::InvalidFieldInCdb());
    }

    const disk::Disk& disk = *m_disks[lun];
    Bytes descriptor;
    if (!cdb.disableBlockDescriptors) {
        descriptor = ShortLbaBlockDescriptor(0, disk.BytesPerSector());
    }
    const auto modeDataLength = static_cast<uint8_t>(3 + descriptor.size() + pages.size());
    // Even with WP set, Windows does not always prevent writes; Write() enforces it.
    response = ModeParameterHeader6(modeDataLength, disk.IsReadOnly(), true, static_cast<uint8_t>(descriptor.size()));
    response.insert(response.end(), descriptor.begin(), descriptor.end());
    response.insert(response.end(), pages.begin(), pages.end());
    TruncateTo(response, cdb.transferLength);
    return ScsiStatus::Good;
}

ScsiStatus VirtualScsiTarget::Read(const Cdb& cdb, uint16_t lun, Bytes& response)
{
    disk::Disk& disk = *m_disks[lun];
    if (static_cast<uint64_t>(cdb.transferLength) * disk.BytesPerSector() > kMaximumTransferBytes) {
        m_log.Log(Severity::Error, "Read error: transfer of {} blocks is too large", cdb.transferLength);
        return CheckCondition(response, SenseData::InvalidFieldInCdb());
    }
    m_log.Log(Severity::Verbose, "LUN {}: Reading {} blocks starting from LBA {}", lun, cdb.transferLength,
              cdb.logicalBlockAddress);
    switch (disk.ReadSectors(cdb.logicalBlockAddress, cdb.transferLength, response)) {
    case disk::IoResult::Ok:
        return ScsiStatus::Good;
    case disk::IoResult::OutOfRange:
        m_log.Write(Severity::Error, "Read error: LBA out of range");
        return CheckCondition(response, SenseData::LbaOutOfRange());
    default:
        m_log.Write(Severity::Error, "Read error: I/O failure");
        return CheckCondition(response, SenseData::UnrecoveredReadError());
    }
}

ScsiStatus VirtualScsiTarget::Write(const Cdb& cdb, uint16_t lun, std::span<const uint8_t> data, Bytes& response)
{
    disk::Disk& disk = *m_disks[lun];
    if (disk.IsReadOnly()) {
        m_log.Log(Severity::Verbose, "LUN {}: Refused attempt to write to a read-only disk", lun);
        return CheckCondition(response, SenseData::DataProtect());
    }
    const uint64_t length = static_cast<uint64_t>(cdb.transferLength) * disk.BytesPerSector();
    if (length > kMaximumTransferBytes || length > data.size()) {
        m_log.Log(Severity::Error, "Write error: {} blocks requested, {} bytes received", cdb.transferLength,
                  data.size());
        return CheckCondition(response, SenseData::InvalidFieldInCdb());
    }
    m_log.Log(Severity::Verbose, "LUN {}: Writing {} blocks starting from LBA {}", lun, cdb.transferLength,
              cdb.logicalBlockAddress);
    switch (disk.WriteSectors(cdb.logicalBlockAddress, data.first(static_cast<size_t>(length)))) {
    case disk::IoResult::Ok:
        return ScsiStatus::Good;
    case disk::IoResult::OutOfRange:
        m_log.Write(Severity::Error, "Write error: LBA out of range");
        return CheckCondition(response, SenseData::LbaOutOfRange());
    default:
        m_log.Write(Severity::Error, "Write error: I/O failure");
        return CheckCondition(response, SenseData::WriteFault());
    }
}

ScsiStatus VirtualScsiTarget::SynchronizeCache(uint16_t lun, Bytes& response)
{
    if (m_disks[lun]->Flush() != disk::IoResult::Ok) {
        m_log.Write(Severity::Error, "Synchronize cache error: I/O failure");
        return CheckCondition(response, SenseData::WriteFault());
    }
    return ScsiStatus::Good;
}

std::string VirtualScsiTarget::SerialNumber(uint16_t lun) const
{
    return std::format("{:06X}{:02X}", m_deviceIdSeed & 0xFFFFFF, lun & 0xFF);
}

} // namespace tiscsi::scsi
