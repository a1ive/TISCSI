// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive

#include "Core/scsi/SptiTarget.h"

#include "Core/scsi/ScsiData.h"
#include "Core/util/ByteOrder.h"

#include <winioctl.h>
#include <ntddscsi.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cwctype>

namespace tiscsi::scsi {

using util::Severity;

namespace {

constexpr ULONG kTimeoutSeconds = 60;
constexpr size_t kSenseBufferLength = 32;

struct PassThroughRequest {
    SCSI_PASS_THROUGH_DIRECT sptd;
    ULONG filler; // keeps the sense buffer aligned
    UCHAR sense[kSenseBufferLength];
};

// CDB length from the operation code's group (SPC).
UCHAR CdbLength(uint8_t opcode)
{
    switch (opcode >> 5) {
    case 0:
        return 6;
    case 1:
    case 2:
        return 10;
    case 5:
        return 12;
    default:
        return 16; // group 4, and reserved / vendor-specific groups
    }
}

bool IsAdapterPath(const std::wstring& path)
{
    // \\.\ScsiN:
    std::wstring lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return lower.starts_with(L"\\\\.\\scsi") && lower.ends_with(L":");
}

ScsiStatus CheckCondition(Bytes& response, const SenseData& sense)
{
    response = FormatSenseData(sense);
    return ScsiStatus::CheckCondition;
}

} // namespace

std::unique_ptr<SptiTarget> SptiTarget::Open(const std::wstring& devicePath, util::Logger logger, DWORD& error)
{
    // Exclusive, as in iSCSI Console: fails while Windows uses the device.
    HANDLE handle = CreateFileW(devicePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return nullptr;
    }
    std::unique_ptr<SptiTarget> target(new SptiTarget(handle, devicePath, std::move(logger)));

    IO_SCSI_CAPABILITIES capabilities = {};
    DWORD returned = 0;
    if (DeviceIoControl(handle, IOCTL_SCSI_GET_CAPABILITIES, nullptr, 0, &capabilities, sizeof(capabilities), &returned,
                        nullptr)) {
        // Transfers above the adapter's maximum fail with ERROR_INVALID_PARAMETER.
        target->m_log.Log(Severity::Information, "{}: maximum transfer length {} bytes, alignment mask 0x{:x}",
                          util::ToUtf8(devicePath), capabilities.MaximumTransferLength, capabilities.AlignmentMask);
    }

    if (IsAdapterPath(devicePath)) {
        if (!target->EnumerateAdapterDevices(error)) {
            return nullptr;
        }
    } else {
        // A class device: the class driver addresses its own device, so the
        // address fields are not used.
        target->m_luns[0] = Address{};
    }
    error = ERROR_SUCCESS;
    return target;
}

SptiTarget::SptiTarget(HANDLE handle, std::wstring path, util::Logger logger)
    : m_handle(handle), m_path(std::move(path)), m_log(std::move(logger))
{
}

SptiTarget::~SptiTarget()
{
    if (m_buffer) {
        VirtualFree(m_buffer, 0, MEM_RELEASE);
    }
    CloseHandle(m_handle);
}

bool SptiTarget::EnumerateAdapterDevices(DWORD& error)
{
    std::vector<uint8_t> buffer(64 * 1024);
    DWORD returned = 0;
    if (!DeviceIoControl(m_handle, IOCTL_SCSI_GET_INQUIRY_DATA, nullptr, 0, buffer.data(),
                         static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
        error = GetLastError();
        return false;
    }
    const auto* info = reinterpret_cast<const SCSI_ADAPTER_BUS_INFO*>(buffer.data());
    const size_t end = std::min<size_t>(returned, buffer.size());
    for (UCHAR bus = 0; bus < info->NumberOfBuses; ++bus) {
        if (offsetof(SCSI_ADAPTER_BUS_INFO, BusData) + (bus + 1) * sizeof(SCSI_BUS_DATA) > end) {
            break;
        }
        ULONG offset = info->BusData[bus].InquiryDataOffset;
        while (offset != 0 && offset + sizeof(SCSI_INQUIRY_DATA) <= end) {
            const auto* device = reinterpret_cast<const SCSI_INQUIRY_DATA*>(buffer.data() + offset);
            // Devices claimed by a class driver must be accessed through it.
            if (!device->DeviceClaimed && device->InquiryDataLength > 0) {
                const uint8_t type = device->InquiryData[0] & 0x1F;
                if (type == static_cast<uint8_t>(PeripheralDeviceType::DirectAccessBlockDevice) ||
                    type == static_cast<uint8_t>(PeripheralDeviceType::SequentialAccessDevice) ||
                    type == static_cast<uint8_t>(PeripheralDeviceType::CdRomDevice)) {
                    const auto lun = static_cast<uint16_t>(m_luns.size());
                    m_luns[lun] = Address{device->PathId, device->TargetId, device->Lun};
                    m_log.Log(Severity::Verbose, "Assigned LUN {} to device PathId: {}, TargetId: {}, LUN: {}", lun,
                              device->PathId, device->TargetId, device->Lun);
                }
            }
            if (device->NextInquiryDataOffset <= offset) {
                break;
            }
            offset = device->NextInquiryDataOffset;
        }
    }
    if (m_luns.empty()) {
        error = ERROR_NOT_FOUND; // every device is claimed by a class driver
        return false;
    }
    return true;
}

bool SptiTarget::EnsureBuffer(size_t length)
{
    if (length <= m_bufferSize) {
        return true;
    }
    if (m_buffer) {
        VirtualFree(m_buffer, 0, MEM_RELEASE);
    }
    m_bufferSize = std::max(length, size_t{64 * 1024});
    m_buffer = static_cast<uint8_t*>(VirtualAlloc(nullptr, m_bufferSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!m_buffer) {
        m_bufferSize = 0;
        return false;
    }
    return true;
}

ScsiStatus SptiTarget::ExecuteCommand(const CdbBytes& cdb, const LunBytes& lunBytes, std::span<const uint8_t> data,
                                      uint32_t dataInLength, Bytes& response)
{
    response.clear();
    if (cdb[0] == op::ReportLuns) {
        // Answered locally from our LUN mapping.
        std::vector<uint16_t> luns;
        for (const auto& [lun, address] : m_luns) {
            luns.push_back(lun);
        }
        response = ReportLunsData(luns);
        const uint32_t allocationLength = util::ReadBE32(&cdb[6]);
        if (response.size() > allocationLength) {
            response.resize(allocationLength);
        }
        return ScsiStatus::Good;
    }

    const std::optional<uint16_t> lunNumber = DecodeSingleLevelLun(lunBytes);
    const auto it = lunNumber ? m_luns.find(*lunNumber) : m_luns.end();
    if (it == m_luns.end()) {
        return CheckCondition(response, SenseData::InvalidLun());
    }

    const bool write = !data.empty();
    const size_t length = write ? data.size() : dataInLength;
    if (length > kMaximumTransferBytes || !EnsureBuffer(length)) {
        return CheckCondition(response, SenseData::InvalidFieldInCdb());
    }
    if (write) {
        std::memcpy(m_buffer, data.data(), data.size());
    }

    PassThroughRequest request = {};
    SCSI_PASS_THROUGH_DIRECT& sptd = request.sptd;
    sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    sptd.PathId = it->second.pathId;
    sptd.TargetId = it->second.targetId;
    sptd.Lun = it->second.lun;
    sptd.CdbLength = CdbLength(cdb[0]);
    sptd.SenseInfoLength = static_cast<UCHAR>(kSenseBufferLength);
    sptd.DataIn = static_cast<UCHAR>(write ? SCSI_IOCTL_DATA_OUT : length ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_UNSPECIFIED);
    sptd.DataTransferLength = static_cast<ULONG>(length);
    sptd.TimeOutValue = kTimeoutSeconds;
    sptd.DataBuffer = length ? m_buffer : nullptr;
    sptd.SenseInfoOffset = offsetof(PassThroughRequest, sense);
    std::memcpy(sptd.Cdb, cdb.data(), sptd.CdbLength);
    m_log.Log(Severity::Verbose, "SCSI Command: 0x{:02X}, Direction: {}, Transfer Length: {}, LUN: {}", cdb[0],
              write ? "out" : length ? "in" : "none", length, it->first);

    DWORD returned = 0;
    if (!DeviceIoControl(m_handle, IOCTL_SCSI_PASS_THROUGH_DIRECT, &request, sizeof(request), &request, sizeof(request),
                         &returned, nullptr)) {
        // ERROR_INVALID_FUNCTION: a class driver claimed the device (use the
        // class device path); ERROR_INVALID_PARAMETER: transfer too large for
        // the adapter; ERROR_ACCESS_DENIED: not running as administrator.
        m_log.Log(Severity::Error, "IOCTL_SCSI_PASS_THROUGH_DIRECT failed, error {}, device {}", GetLastError(),
                  util::ToUtf8(m_path));
        return CheckCondition(response, SenseData::UnsupportedCommand());
    }

    const auto status = static_cast<ScsiStatus>(sptd.ScsiStatus);
    if (status == ScsiStatus::Good) {
        if (!write && sptd.DataTransferLength > 0) {
            // DataTransferLength now holds the number of bytes actually transferred.
            const size_t received = std::min<size_t>(sptd.DataTransferLength, length);
            response.assign(m_buffer, m_buffer + received);
        }
        return status;
    }
    if (status == ScsiStatus::CheckCondition) {
        // Fixed and descriptor formats both keep the additional length in byte 7.
        const size_t senseLength = std::min<size_t>(kSenseBufferLength, 8u + request.sense[7]);
        response.resize(2 + senseLength);
        util::WriteBE16(response.data(), static_cast<uint16_t>(senseLength));
        std::memcpy(response.data() + 2, request.sense, senseLength);
        m_log.Log(Severity::Verbose, "SCSI Status: CHECK CONDITION, sense key 0x{:x}", request.sense[2] & 0x0F);
    }
    return status;
}

} // namespace tiscsi::scsi
