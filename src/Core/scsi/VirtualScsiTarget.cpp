// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive

#include "Core/scsi/VirtualScsiTarget.h"

#include "Core/scsi/MmcData.h"
#include "Core/util/ByteOrder.h"

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
        return ScsiStatus::Good;
    case op::Verify10:
    case op::Verify12:
    case op::Verify16:
        return Verify(cdb, lun, data, response);
    case op::RequestSense:
        // Some initiators (e.g. EFI iSCSI DXE) send REQUEST SENSE right after connecting.
        response = FormatSenseData(SenseData::NoSense());
        TruncateTo(response, cdb.transferLength);
        return ScsiStatus::Good;
    case op::Inquiry:
        return Inquiry(cdb, lun, response);
    case op::ModeSense6:
    case op::ModeSense10:
        return ModeSense(cdb, lun, response);
    case op::ReadCapacity10:
        response = ReadCapacity10Data(disk.Size(), disk.BytesPerSector());
        return ScsiStatus::Good;
    case op::Read6:
    case op::Read10:
    case op::Read12:
    case op::Read16:
        return Read(cdb, lun, response);
    case op::Write6:
    case op::Write10:
    case op::Write12:
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
    if (disk.IsOpticalDisc()) {
        return ExecuteMmcCommand(cdb, cdbBytes, lun, response);
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
        if (m_disks[lun]->IsOpticalDisc()) {
            inquiry.peripheralDeviceType = PeripheralDeviceType::CdRomDevice;
            inquiry.removable = true;
            inquiry.productIdentification = std::format("SCSI DVD-ROM {}", lun);
        } else {
            inquiry.peripheralDeviceType = PeripheralDeviceType::DirectAccessBlockDevice;
            inquiry.productIdentification = std::format("SCSI Disk {}", lun);
        }
        inquiry.vendorIdentification = "TISCSI";
        inquiry.productRevisionLevel = "1.00";
        inquiry.cmdQue = true;
        inquiry.version = 5; // SPC-3
        if (m_hooks.standardInquiry) {
            m_hooks.standardInquiry(lun, inquiry);
        }
        response = inquiry.GetBytes();
    } else {
        m_log.Log(Severity::Verbose, "Inquiry: Page code: 0x{:X}", static_cast<uint8_t>(cdb.pageCode));
        if (m_disks[lun]->IsOpticalDisc() &&
            (cdb.pageCode == VpdPage::BlockLimits || cdb.pageCode == VpdPage::BlockDeviceCharacteristics)) {
            m_log.Log(Severity::Error, "Inquiry: VPD page 0x{:X} is only provided by block devices",
                      static_cast<uint8_t>(cdb.pageCode));
            return CheckCondition(response, SenseData::InvalidFieldInCdb());
        }
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

ScsiStatus VirtualScsiTarget::ModeSense(const Cdb& cdb, uint16_t lun, Bytes& response)
{
    const bool modeSense10 = cdb.opcode == op::ModeSense10;
    m_log.Log(Severity::Verbose, "ModeSense{}: Page code: 0x{:X}, Sub page code: 0x{:X}", modeSense10 ? 10 : 6,
              static_cast<uint8_t>(cdb.modePageCode), cdb.subpageCode);
    const disk::Disk& disk = *m_disks[lun];
    const bool optical = disk.IsOpticalDisc();
    Bytes pages;
    if (!(optical ? MmcModePages(cdb, pages) : DiskModePages(cdb, pages))) {
        return CheckCondition(response, SenseData::InvalidFieldInCdb());
    }

    // MMC drives have neither block descriptors nor a device-specific parameter.
    Bytes descriptor;
    if (!cdb.disableBlockDescriptors && !optical) {
        descriptor = ShortLbaBlockDescriptor(0, disk.BytesPerSector());
    }
    // Even with WP set, Windows does not always prevent writes; Write() enforces it.
    const bool writeProtect = !optical && disk.IsReadOnly();
    if (modeSense10) {
        const auto modeDataLength = static_cast<uint16_t>(6 + descriptor.size() + pages.size());
        response = ModeParameterHeader10(modeDataLength, writeProtect, !optical,
                                         static_cast<uint16_t>(descriptor.size()));
    } else {
        const auto modeDataLength = static_cast<uint8_t>(3 + descriptor.size() + pages.size());
        response = ModeParameterHeader6(modeDataLength, writeProtect, !optical,
                                        static_cast<uint8_t>(descriptor.size()));
    }
    response.insert(response.end(), descriptor.begin(), descriptor.end());
    response.insert(response.end(), pages.begin(), pages.end());
    TruncateTo(response, cdb.transferLength);
    return ScsiStatus::Good;
}

bool VirtualScsiTarget::DiskModePages(const Cdb& cdb, Bytes& pages)
{
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
            m_log.Log(Severity::Error, "ModeSense: Power condition subpage 0x{:x} is not implemented",
                      cdb.subpageCode);
            return false;
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
        m_log.Log(Severity::Error, "ModeSense: page 0x{:x} is not implemented",
                  static_cast<uint8_t>(cdb.modePageCode));
        return false;
    }
    return true;
}

bool VirtualScsiTarget::MmcModePages(const Cdb& cdb, Bytes& pages)
{
    switch (cdb.modePageCode) {
    case ModePageCode::ReadWriteErrorRecovery:
        pages = ReadWriteErrorRecoveryModePage();
        break;
    case ModePageCode::MmCapabilities:
        pages = MmCapabilitiesModePage();
        break;
    case ModePageCode::ReturnAllPages: {
        pages = ReadWriteErrorRecoveryModePage();
        const Bytes capabilities = MmCapabilitiesModePage();
        pages.insert(pages.end(), capabilities.begin(), capabilities.end());
        break;
    }
    case ModePageCode::VendorSpecific:
        break;
    default:
        m_log.Log(Severity::Error, "ModeSense: page 0x{:x} is not implemented for a CD/DVD-ROM drive",
                  static_cast<uint8_t>(cdb.modePageCode));
        return false;
    }
    return true;
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

ScsiStatus VirtualScsiTarget::Verify(const Cdb& cdb, uint16_t lun, std::span<const uint8_t> data, Bytes& response)
{
    disk::Disk& disk = *m_disks[lun];
    const uint8_t byteCheck = (cdb.verifyFlags >> 1) & 0x03;
    // No protection information or relative addressing. DPO is a cache-retention
    // hint; this target has no read cache of its own. BYTCHK=2 is reserved in SBC.
    // On MMC devices bit 2 is BLKVFY, not the high bit of SBC's BYTCHK field.
    if ((cdb.verifyFlags & 0xE9) != 0 || byteCheck == 2 || (disk.IsOpticalDisc() && byteCheck == 3)) {
        return CheckCondition(response, SenseData::InvalidFieldInCdb());
    }
    const uint32_t count = cdb.transferLength;
    const uint32_t blockSize = disk.BytesPerSector();
    const uint64_t expectedLength = count == 0 || byteCheck == 0 ? 0 :
        byteCheck == 3 ? blockSize : static_cast<uint64_t>(count) * blockSize;
    if (expectedLength > kMaximumTransferBytes || expectedLength != data.size()) {
        return CheckCondition(response, SenseData::InvalidFieldInCdb());
    }
    // A zero verification length is a no-op, not a request to verify to the end.
    if (count == 0) {
        return ScsiStatus::Good;
    }
    const uint64_t total = disk.TotalSectors();
    if (cdb.logicalBlockAddress >= total || count > total - cdb.logicalBlockAddress) {
        return CheckCondition(response, SenseData::LbaOutOfRange());
    }
    // SBC requires pending writes to the referenced blocks to reach the medium.
    // Disk::Flush has whole-disk granularity and is also safe on read-only disks.
    if (disk.Flush() != disk::IoResult::Ok) {
        return CheckCondition(response, SenseData::WriteFault());
    }
    constexpr uint32_t kVerifyChunkBytes = 1024 * 1024;
    const uint32_t chunkBlocks = std::max(uint32_t{1}, kVerifyChunkBytes / blockSize);
    Bytes buffer;
    for (uint32_t done = 0; done < count;) {
        const uint32_t blocks = std::min(count - done, chunkBlocks);
        const size_t length = static_cast<size_t>(blocks) * blockSize;
        switch (disk.ReadSectors(cdb.logicalBlockAddress + done, blocks, buffer)) {
        case disk::IoResult::Ok:
            break;
        case disk::IoResult::OutOfRange:
            return CheckCondition(response, SenseData::LbaOutOfRange());
        default:
            return CheckCondition(response, SenseData::UnrecoveredReadError());
        }
        if (buffer.size() != length) {
            return CheckCondition(response, SenseData::UnrecoveredReadError());
        }
        if (byteCheck != 0) {
            for (uint32_t block = 0; block < blocks; ++block) {
                const size_t offset = byteCheck == 3 ? 0 : static_cast<size_t>(done + block) * blockSize;
                const auto first = buffer.begin() + static_cast<size_t>(block) * blockSize;
                if (!std::equal(first, first + blockSize, data.begin() + offset)) {
                    return CheckCondition(response, SenseData::Miscompare(cdb.logicalBlockAddress + done + block));
                }
            }
        }
        done += blocks;
    }
    return ScsiStatus::Good;
}

ScsiStatus VirtualScsiTarget::SynchronizeCache(uint16_t lun, Bytes& response)
{
    if (m_disks[lun]->Flush() != disk::IoResult::Ok) {
        m_log.Write(Severity::Error, "Synchronize cache error: I/O failure");
        return CheckCondition(response, SenseData::WriteFault());
    }
    return ScsiStatus::Good;
}

ScsiStatus VirtualScsiTarget::ExecuteMmcCommand(const Cdb& cdb, const CdbBytes& cdbBytes, uint16_t lun,
                                                Bytes& response)
{
    const uint8_t* b = cdbBytes.data();
    const uint64_t sectors = m_disks[lun]->TotalSectors();
    const MmcProfile profile = ProfileForDisc(sectors);
    std::optional<Bytes> data;
    switch (cdb.opcode) {
    case op::StartStopUnit: // the disc stays in the drive
    case op::PreventAllowMediumRemoval:
    case op::SetCdSpeed:
        return ScsiStatus::Good;
    case op::ReadToc: {
        uint8_t format = b[2] & 0x0F;
        if (format == 0) {
            format = b[9] >> 6; // older (SFF-8020) position, which Windows still uses for some requests
        }
        data = TocData(sectors, format, (b[1] & 0x02) != 0, b[6]);
        break;
    }
    case op::GetConfiguration:
        data = ConfigurationData(profile, b[1] & 0x03, util::ReadBE16(b + 2));
        break;
    case op::GetEventStatusNotification:
        if (b[1] & 0x01) { // asynchronous notification is not supported
            data = EventStatusData(b[4]);
        }
        break;
    case op::ReadDiscInformation:
        if ((b[1] & 0x07) == 0) { // the other data types are for BD
            data = DiscInformationData();
        }
        break;
    case op::ReadTrackInformation:
        data = TrackInformationData(sectors, b[1] & 0x03, util::ReadBE32(b + 2));
        break;
    case op::ReadDvdStructure:
        if ((b[1] & 0x0F) != 0) { // BD media type
            break;
        }
        if (b[7] != 0xFF && profile == MmcProfile::CdRom) {
            m_log.Log(Severity::Verbose, "LUN {}: READ DVD STRUCTURE on a CD", lun);
            return CheckCondition(response, SenseData::IncompatibleMediumFormat());
        }
        data = DvdStructureData(sectors, b[7], b[6]);
        break;
    case op::MechanismStatus:
        data = MechanismStatusData();
        break;
    default:
        m_log.Log(Severity::Error, "Unsupported SCSI Command (0x{:X})", cdb.opcode);
        return CheckCondition(response, SenseData::UnsupportedCommand());
    }
    if (!data) {
        m_log.Log(Severity::Warning, "Initiator error: invalid field in MMC command 0x{:02X}", cdb.opcode);
        return CheckCondition(response, SenseData::InvalidFieldInCdb());
    }
    response = std::move(*data);
    TruncateTo(response, cdb.transferLength);
    return ScsiStatus::Good;
}

std::string VirtualScsiTarget::SerialNumber(uint16_t lun) const
{
    return std::format("{:06X}{:02X}", m_deviceIdSeed & 0xFFFFFF, lun & 0xFF);
}

} // namespace tiscsi::scsi
