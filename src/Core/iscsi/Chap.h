// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// The target's side of CHAP authentication during the security negotiation
// stage of login (RFC 3720 11.1.4, RFC 1994): the initiator offers
// AuthMethod=CHAP, then CHAP_A, and answers the target's challenge with
// CHAP_N / CHAP_R, optionally adding its own CHAP_I / CHAP_C for mutual
// CHAP. Only MD5 (CHAP_A=5) is supported, which is what initiators use.

#pragma once

#include "Core/iscsi/IscsiTarget.h"
#include "Core/util/KeyValueText.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tiscsi::iscsi {

// Per connection, during login.
struct ChapExchange {
    enum class Step { Start, MethodChosen, ChallengeSent, Done };
    Step step = Step::Start;
    uint8_t identifier = 0;
    std::vector<uint8_t> challenge;
};

struct ChapResult {
    enum class Outcome { Continue, Done, Failed };
    Outcome outcome = Outcome::Continue;
    util::KeyValueList response; // keys for the Login Response
    std::string reason;          // why it failed, for the log
};

// Handles the CHAP keys of one Login Request in the security stage.
ChapResult ChapStep(ChapExchange& exchange, const TargetAccess& access, const std::string& targetName,
                    const util::KeyValueList& request);

// MD5(identifier || secret || challenge), the CHAP response.
std::optional<std::array<uint8_t, 16>> ChapResponse(uint8_t identifier, std::string_view secret,
                                                    std::span<const uint8_t> challenge);
// iSCSI binary values: "0x" + hex, or "0b" + base64 (RFC 3720 5.1).
std::optional<std::vector<uint8_t>> DecodeBinaryValue(std::string_view text);
std::string EncodeHex(std::span<const uint8_t> bytes);

} // namespace tiscsi::iscsi
