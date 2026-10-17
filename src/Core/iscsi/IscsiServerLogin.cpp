// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2012-2024 Tal Aloni <tal.aloni.il@gmail.com>
// Copyright (C) 2026 A1ive
//
// Ported from iSCSI Console (ISCSIServer.Login.cs).

#include "Core/iscsi/IscsiServer.h"

#include <algorithm>
#include <charconv>
#include <string>

namespace tiscsi::iscsi {

using util::Severity;

namespace {

// Login stages
constexpr uint8_t kSecurityNegotiation = 0;
constexpr uint8_t kLoginOperationalNegotiation = 1;
constexpr uint8_t kFullFeaturePhase = 3;

std::optional<int> ParseNumber(std::optional<std::string_view> text)
{
    if (!text) {
        return std::nullopt;
    }
    int value = 0;
    const auto [end, ec] = std::from_chars(text->data(), text->data() + text->size(), value);
    if (ec != std::errc() || end != text->data() + text->size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> ParseBoolean(std::optional<std::string_view> text)
{
    if (!text) {
        return std::nullopt;
    }
    return *text == "Yes";
}

const char* YesNo(bool value) { return value ? "Yes" : "No"; }

LoginResponse GetLoginResponseTemplate(const LoginRequest& request)
{
    LoginResponse response;
    response.continueFlag = false;
    response.currentStage = request.currentStage;
    response.nextStage = request.nextStage;
    response.versionMax = request.versionMax;
    response.versionActive = request.versionMin;
    response.isid = request.isid;
    // Except in the final response of a new session, echo the request's TSIH.
    response.tsih = request.tsih;
    response.initiatorTaskTag = request.initiatorTaskTag;
    return response;
}

void UpdateOperationalParameters(const util::KeyValueList& parameters, Connection& connection, Session& session)
{
    if (auto value = ParseNumber(parameters.Find("MaxRecvDataSegmentLength"))) {
        connection.initiatorMaxRecvDataSegmentLength = static_cast<uint32_t>(std::clamp(*value, 512, 16777215));
    }
    if (auto value = ParseNumber(parameters.Find("MaxConnections"))) {
        session.maxConnections = std::clamp(*value, 1, desired::kMaxConnections);
    }
    if (auto value = ParseBoolean(parameters.Find("InitialR2T"))) {
        session.initialR2T = *value || desired::kInitialR2T;
    }
    if (auto value = ParseBoolean(parameters.Find("ImmediateData"))) {
        session.immediateData = *value && desired::kImmediateData;
    }
    if (auto value = ParseNumber(parameters.Find("MaxBurstLength"))) {
        session.maxBurstLength = std::clamp(*value, 512, desired::kMaxBurstLength);
    }
    if (auto value = ParseNumber(parameters.Find("FirstBurstLength"))) {
        session.firstBurstLength = std::clamp(*value, 512, desired::kFirstBurstLength);
    }
    if (auto value = ParseBoolean(parameters.Find("DataPDUInOrder"))) {
        session.dataPduInOrder = *value || desired::kDataPduInOrder;
    }
    if (auto value = ParseBoolean(parameters.Find("DataSequenceInOrder"))) {
        session.dataSequenceInOrder = *value || desired::kDataSequenceInOrder;
    }
    if (auto value = ParseNumber(parameters.Find("DefaultTime2Wait"))) {
        session.defaultTime2Wait = std::clamp(std::max(*value, desired::kDefaultTime2Wait), 0, 3600);
    }
    if (auto value = ParseNumber(parameters.Find("DefaultTime2Retain"))) {
        session.defaultTime2Retain = std::clamp(*value, 0, desired::kDefaultTime2Retain);
    }
    if (auto value = ParseNumber(parameters.Find("MaxOutstandingR2T"))) {
        session.maxOutstandingR2T = std::clamp(*value, 1, desired::kMaxOutstandingR2T);
    }
}

util::KeyValueList GetLoginResponseOperationalParameters(const Connection& connection, const Session& session)
{
    util::KeyValueList parameters;
    parameters.Add("HeaderDigest", "None");
    parameters.Add("DataDigest", "None");
    parameters.Add("MaxRecvDataSegmentLength", std::to_string(connection.targetMaxRecvDataSegmentLength));
    if (!session.isDiscovery) {
        parameters.Add("MaxConnections", std::to_string(session.maxConnections));
        parameters.Add("InitialR2T", YesNo(session.initialR2T)); // Microsoft iSCSI Target supports InitialR2T=No
        parameters.Add("ImmediateData", YesNo(session.immediateData));
        parameters.Add("MaxBurstLength", std::to_string(session.maxBurstLength));
        parameters.Add("FirstBurstLength", std::to_string(session.firstBurstLength));
        parameters.Add("MaxOutstandingR2T", std::to_string(session.maxOutstandingR2T));
        parameters.Add("DataPDUInOrder", YesNo(session.dataPduInOrder));
        parameters.Add("DataSequenceInOrder", YesNo(session.dataSequenceInOrder));
    }
    parameters.Add("DefaultTime2Wait", std::to_string(session.defaultTime2Wait));
    parameters.Add("DefaultTime2Retain", std::to_string(session.defaultTime2Retain));
    parameters.Add("ErrorRecoveryLevel", std::to_string(session.errorRecoveryLevel));
    return parameters;
}

} // namespace

LoginResponse IscsiServer::GetLoginResponse(const LoginRequest& request, const ConnectionPtr& connection)
{
    // The numbering fields are only valid if the status class is 0. Command
    // numbering starts with the first login request of the session.
    if (request.continueFlag) {
        connection->AddTextToSequence(request.initiatorTaskTag, request.text);
        LoginResponse response = GetLoginResponseTemplate(request);
        response.transit = false;
        response.expCmdSN = request.cmdSN; // the session may not exist yet
        if (request.transit) {
            m_log.Log(Severity::Warning, "[{}] Initiator error: Received login request with both Transit and Continue set to true",
                      connection->Identifier());
            response.status = LoginStatus::InitiatorError;
        }
        return response;
    }

    const std::string text = connection->AddTextToSequence(request.initiatorTaskTag, request.text);
    connection->RemoveTextSequence(request.initiatorTaskTag);
    const util::KeyValueList parameters = util::KeyValueList::ParseNullDelimited(text);
    if (!connection->GetSession()) {
        const LoginStatus status = SetUpSession(request, parameters, connection);
        if (status != LoginStatus::Success) {
            LoginResponse response = GetLoginResponseTemplate(request);
            response.transit = request.transit;
            response.status = status;
            return response;
        }
    }
    return GetFinalLoginResponse(request, parameters, connection);
}

LoginStatus IscsiServer::SetUpSession(const LoginRequest& request, const util::KeyValueList& parameters,
                                      const ConnectionPtr& connection)
{
    const std::string initiatorName(parameters.Find("InitiatorName").value_or(""));
    if (initiatorName.empty()) {
        // RFC 3720: the initiator MUST provide InitiatorName in the first login of every connection.
        m_log.Log(Severity::Warning, "[ISID={:x},TSIH={:x},CID={:x}] Initiator error: InitiatorName was not included in the login request",
                  request.isid, request.tsih, request.cid);
        return LoginStatus::InitiatorError;
    }

    if (request.tsih == 0) {
        // New session; the target assigns the TSIH. The same ISID may log in
        // to another target, so reinstatement only applies per target.
        SessionPtr session = m_sessionManager.StartSession(initiatorName, request.isid);
        connection->cid = request.cid;
        connection->SetSession(session);
        m_log.Log(Severity::Verbose, "[{}] Session has been started", session->Identifier());
        session->commandNumberingStarted = true;
        session->expCmdSN = request.cmdSN;

        if (parameters.Find("SessionType") == "Discovery") {
            session->isDiscovery = true;
            return LoginStatus::Success;
        }
        // Normal session (the default): TargetName is mandatory.
        if (auto targetName = parameters.Find("TargetName")) {
            return SetUpNormalSession(request, std::string(*targetName), connection);
        }
        m_log.Log(Severity::Warning, "[{}] Initiator error: TargetName was not included in a non-discovery session",
                  connection->Identifier());
        return LoginStatus::InitiatorError;
    }

    SessionPtr existingSession = m_sessionManager.FindSession(initiatorName, request.isid, request.tsih);
    if (!existingSession) {
        return LoginStatus::SessionDoesNotExist;
    }
    connection->SetSession(existingSession);
    if (ConnectionPtr existingConnection = m_connectionManager.FindConnection(existingSession, request.cid)) {
        // Connection reinstatement
        m_log.Log(Severity::Verbose, "[{}] Initiating implicit logout", existingConnection->Identifier());
        m_connectionManager.ReleaseConnection(existingConnection);
    } else if (m_connectionManager.GetSessionConnections(existingSession).size() >=
               static_cast<size_t>(existingSession->maxConnections)) {
        return LoginStatus::TooManyConnections;
    }
    connection->cid = request.cid;
    return LoginStatus::Success;
}

LoginStatus IscsiServer::SetUpNormalSession(const LoginRequest& request, const std::string& targetName,
                                            const ConnectionPtr& connection)
{
    const SessionPtr session = connection->GetSession();
    session->isDiscovery = false;
    // Terminate an existing session between this initiator and the target
    // before reinstating a new one in its place.
    if (SessionPtr existing = m_sessionManager.FindSession(session->initiatorName, request.isid, targetName)) {
        m_log.Log(Severity::Verbose, "[{}] Terminating old session with target: {}", connection->Identifier(), targetName);
        TerminateSession(existing, SessionTerminationReason::ImplicitLogout);
    }

    std::lock_guard lock(m_targetsMutex);
    std::shared_ptr<IscsiTarget> target = FindTargetLocked(targetName);
    if (!target) {
        m_log.Log(Severity::Warning, "[{}] Initiator requested an unknown target: {}", connection->Identifier(), targetName);
        return LoginStatus::NotFound;
    }
    if (!target->AuthorizeInitiator(session->initiatorName, session->isid, connection->InitiatorAddress())) {
        m_log.Log(Severity::Warning, "[{}] Initiator was not authorized to access {}", connection->Identifier(), targetName);
        return LoginStatus::AuthorizationFailure;
    }
    m_sessionManager.SetTarget(*session, std::move(target));
    return LoginStatus::Success;
}

LoginResponse IscsiServer::GetFinalLoginResponse(const LoginRequest& request, const util::KeyValueList& parameters,
                                                 const ConnectionPtr& connection)
{
    const SessionPtr session = connection->GetSession();
    LoginResponse response = GetLoginResponseTemplate(request);
    response.transit = request.transit;
    response.tsih = session->tsih;
    response.status = LoginStatus::Success;

    // Two optional stages, at least one of which must be present: security
    // negotiation (0) and operational parameter negotiation (1), then the
    // full feature phase (3).
    if (request.currentStage == kSecurityNegotiation) {
        util::KeyValueList responseParameters;
        responseParameters.Add("AuthMethod", "None");
        if (session->target) {
            // Returned with the first Login Response that may carry it.
            responseParameters.Add("TargetPortalGroupTag", "1");
        }
        if (request.transit) {
            if (request.nextStage == kFullFeaturePhase) {
                connection->isFullFeaturePhase = true;
                session->isFullFeaturePhase = true;
            } else if (request.nextStage != kLoginOperationalNegotiation) {
                m_log.Log(Severity::Warning, "[{}] Initiator error: Received login request with Invalid NextStage",
                          connection->Identifier());
                response.status = LoginStatus::InitiatorError;
            }
        }
        response.text = responseParameters.ToNullDelimited();
    } else if (request.currentStage == kLoginOperationalNegotiation) {
        UpdateOperationalParameters(parameters, *connection, *session);
        response.text = GetLoginResponseOperationalParameters(*connection, *session).ToNullDelimited();
        if (request.transit) {
            if (request.nextStage == kFullFeaturePhase) {
                connection->isFullFeaturePhase = true;
                session->isFullFeaturePhase = true;
            } else {
                m_log.Log(Severity::Warning, "[{}] Initiator error: Received login request with Invalid NextStage",
                          connection->Identifier());
                response.status = LoginStatus::InitiatorError;
            }
        }
    } else {
        m_log.Log(Severity::Warning, "[{}] Initiator error: Received login request with Invalid CurrentStage",
                  connection->Identifier());
        response.status = LoginStatus::InitiatorError;
    }
    return response;
}

} // namespace tiscsi::iscsi
