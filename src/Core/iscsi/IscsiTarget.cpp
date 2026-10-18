// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive

#include "Core/iscsi/IscsiTarget.h"

#include "Core/iscsi/IscsiName.h"
#include "Core/scsi/VirtualScsiTarget.h"

#include <algorithm>

namespace tiscsi::iscsi {
namespace {

// FNV-1a over the lower-cased name: iSCSI names are case-insensitive.
uint32_t HashName(const std::string& name)
{
    uint32_t hash = 2166136261u;
    for (char c : name) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    return hash;
}

} // namespace

IscsiTarget::IscsiTarget(std::string name, std::vector<std::unique_ptr<disk::Disk>> disks, util::Logger scsiLogger)
    : m_name(std::move(name))
{
    auto target = std::make_unique<scsi::VirtualScsiTarget>(std::move(disks), std::move(scsiLogger));
    target->SetDeviceIdSeed(HashName(m_name));
    m_scsiTarget = std::move(target);
    InstallInquiryHooks();
    m_commandQueue = std::make_unique<scsi::ScsiCommandQueue>(*m_scsiTarget);
}

IscsiTarget::IscsiTarget(std::string name, std::unique_ptr<scsi::ScsiTarget> scsiTarget)
    : m_name(std::move(name)), m_scsiTarget(std::move(scsiTarget))
{
    InstallInquiryHooks();
    m_commandQueue = std::make_unique<scsi::ScsiCommandQueue>(*m_scsiTarget);
}

IscsiTarget::~IscsiTarget() { m_commandQueue.reset(); }

void IscsiTarget::InstallInquiryHooks()
{
    scsi::InquiryHooks hooks;
    hooks.standardInquiry = [](uint16_t, scsi::StandardInquiryData& data) {
        data.versionDescriptors.push_back(scsi::VersionDescriptor::Iscsi);
    };
    hooks.deviceIdentification = [this](uint16_t, std::vector<scsi::IdentificationDescriptor>& descriptors) {
        // Needed for WinPE to pick up the disk during boot (after iPXE's sanhook).
        descriptors.push_back(scsi::IdentificationDescriptor::ScsiNameString(m_name));
    };
    m_scsiTarget->SetInquiryHooks(std::move(hooks));
}

void IscsiTarget::QueueCommand(const scsi::CdbBytes& cdb, const scsi::LunBytes& lun, std::vector<uint8_t> data,
                               uint32_t dataInLength, scsi::CommandCompletion done)
{
    m_commandQueue->QueueCommand(cdb, lun, std::move(data), dataInLength, std::move(done));
}

bool TargetAccess::AllowsInitiator(std::string_view initiatorName) const
{
    return allowedInitiators.empty() ||
           std::any_of(allowedInitiators.begin(), allowedInitiators.end(),
                       [&](const std::string& allowed) { return EqualsIgnoreCase(allowed, initiatorName); });
}

bool IscsiTarget::AuthorizeInitiator(const std::string& initiatorName, uint64_t isid,
                                     const std::string& initiatorAddress) const
{
    if (!access.AllowsInitiator(initiatorName)) {
        return false;
    }
    return onAuthorizationRequest ? onAuthorizationRequest(initiatorName, isid, initiatorAddress) : true;
}

util::KeyValueList IscsiTarget::GetTextResponse(const util::KeyValueList& request) const
{
    return onTextRequest ? onTextRequest(request) : util::KeyValueList{};
}

void IscsiTarget::NotifySessionTermination(const std::string& initiatorName, uint64_t isid,
                                           SessionTerminationReason reason) const
{
    if (onSessionTermination) {
        onSessionTermination(initiatorName, isid, reason);
    }
}

} // namespace tiscsi::iscsi
