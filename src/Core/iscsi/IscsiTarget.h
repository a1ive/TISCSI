// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSI/ISCSI.Server/ISCSITarget.cs and
// TargetEventArgs).

#pragma once

#include "Core/disk/Disk.h"
#include "Core/scsi/ScsiTarget.h"
#include "Core/util/KeyValueText.h"
#include "Core/util/Log.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tiscsi::iscsi {

// Who may log in to a target. Not part of iSCSI Console, which let every
// initiator in.
struct TargetAccess {
    // Initiator names allowed to log in (and to see the target in discovery);
    // empty: any. Initiators choose their names, so this alone keeps out
    // mistakes, not attackers: use CHAP for that.
    std::vector<std::string> allowedInitiators;
    // CHAP (RFC 3720 11.1.4, MD5), required if `chapSecret` is set. An empty
    // `chapUser` accepts any CHAP name.
    std::string chapUser;
    std::string chapSecret;
    // Mutual CHAP: how the target answers the initiator's challenge, if the
    // initiator asks. An empty `mutualUser` is the target name.
    std::string mutualUser;
    std::string mutualSecret;

    bool RequiresChap() const { return !chapSecret.empty(); }
    bool AllowsInitiator(std::string_view initiatorName) const;
    bool operator==(const TargetAccess&) const = default;
};

enum class SessionTerminationReason {
    Logout,
    ImplicitLogout, // session reinstatement
    ConnectionFailure,
    TargetReset,
};

// An iSCSI target: a name plus the SCSI device server behind it.
// Callbacks run on server threads and must not block for long.
class IscsiTarget {
public:
    // Returns false to refuse the login.
    using AuthorizationHandler =
        std::function<bool(const std::string& initiatorName, uint64_t isid, const std::string& initiatorAddress)>;
    using SessionTerminationHandler =
        std::function<void(const std::string& initiatorName, uint64_t isid, SessionTerminationReason reason)>;
    // Vendor-specific ("X-" / "X#") text keys in normal sessions.
    using TextRequestHandler = std::function<util::KeyValueList(const util::KeyValueList& request)>;

    // Serves `disks` through a VirtualScsiTarget, LUN = index.
    IscsiTarget(std::string name, std::vector<std::unique_ptr<disk::Disk>> disks, util::Logger scsiLogger);
    IscsiTarget(std::string name, std::unique_ptr<scsi::ScsiTarget> scsiTarget);
    ~IscsiTarget();
    IscsiTarget(const IscsiTarget&) = delete;
    IscsiTarget& operator=(const IscsiTarget&) = delete;

    const std::string& Name() const { return m_name; }
    scsi::ScsiTarget& GetScsiTarget() { return *m_scsiTarget; }

    void QueueCommand(const scsi::CdbBytes& cdb, const scsi::LunBytes& lun, std::vector<uint8_t> data,
                      uint32_t dataInLength, scsi::CommandCompletion done);

    // Set before the target is added to a server.
    AuthorizationHandler onAuthorizationRequest;
    SessionTerminationHandler onSessionTermination;
    TextRequestHandler onTextRequest;
    TargetAccess access;

    // The allowed initiators, then onAuthorizationRequest. CHAP follows in
    // the security negotiation stage.
    bool AuthorizeInitiator(const std::string& initiatorName, uint64_t isid, const std::string& initiatorAddress) const;
    util::KeyValueList GetTextResponse(const util::KeyValueList& request) const;
    void NotifySessionTermination(const std::string& initiatorName, uint64_t isid,
                                  SessionTerminationReason reason) const;

private:
    void InstallInquiryHooks();

    std::string m_name;
    std::unique_ptr<scsi::ScsiTarget> m_scsiTarget;
    // Declared after m_scsiTarget: the worker must stop before the target goes away.
    std::unique_ptr<scsi::ScsiCommandQueue> m_commandQueue;
};

} // namespace tiscsi::iscsi
