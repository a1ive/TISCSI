// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/iscsi/Chap.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <charconv>

namespace tiscsi::iscsi {
namespace {

constexpr std::string_view kMd5 = "5";
constexpr size_t kChallengeLength = 16;

// CryptoAPI (MD5 and random numbers) is on every Windows version.
HCRYPTPROV CryptoProvider()
{
    static const HCRYPTPROV provider = [] {
        HCRYPTPROV handle = 0;
        if (!CryptAcquireContextW(&handle, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
            handle = 0;
        }
        return handle;
    }();
    return provider;
}

bool RandomBytes(std::span<uint8_t> bytes)
{
    const HCRYPTPROV provider = CryptoProvider();
    return provider && CryptGenRandom(provider, static_cast<DWORD>(bytes.size()), bytes.data());
}

// "CHAP,None" -> whether `item` is in the list.
bool ListContains(std::string_view list, std::string_view item)
{
    while (!list.empty()) {
        const size_t comma = list.find(',');
        if (list.substr(0, comma) == item) {
            return true;
        }
        list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
    }
    return false;
}

int Base64Digit(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

int HexDigit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// Compares without leaking how much of a response was right.
bool EqualConstantTime(std::span<const uint8_t> a, std::span<const uint8_t> b)
{
    if (a.size() != b.size()) {
        return false;
    }
    uint8_t difference = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        difference |= a[i] ^ b[i];
    }
    return difference == 0;
}

ChapResult Fail(std::string reason)
{
    ChapResult result;
    result.outcome = ChapResult::Outcome::Failed;
    result.reason = std::move(reason);
    return result;
}

} // namespace

std::optional<std::array<uint8_t, 16>> ChapResponse(uint8_t identifier, std::string_view secret,
                                                    std::span<const uint8_t> challenge)
{
    const HCRYPTPROV provider = CryptoProvider();
    HCRYPTHASH hash = 0;
    if (!provider || !CryptCreateHash(provider, CALG_MD5, 0, 0, &hash)) {
        return std::nullopt;
    }
    std::array<uint8_t, 16> digest = {};
    DWORD length = static_cast<DWORD>(digest.size());
    const bool ok = CryptHashData(hash, &identifier, 1, 0) &&
                    CryptHashData(hash, reinterpret_cast<const BYTE*>(secret.data()), static_cast<DWORD>(secret.size()), 0) &&
                    CryptHashData(hash, challenge.data(), static_cast<DWORD>(challenge.size()), 0) &&
                    CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &length, 0) && length == digest.size();
    CryptDestroyHash(hash);
    return ok ? std::optional(digest) : std::nullopt;
}

std::optional<std::vector<uint8_t>> DecodeBinaryValue(std::string_view text)
{
    if (text.size() < 3 || text[0] != '0') {
        return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    if (text[1] == 'x' || text[1] == 'X') {
        std::string_view digits = text.substr(2);
        if (digits.size() % 2 != 0) {
            return std::nullopt; // RFC 3720 allows an odd count, but no initiator sends one
        }
        for (size_t i = 0; i < digits.size(); i += 2) {
            const int high = HexDigit(digits[i]);
            const int low = HexDigit(digits[i + 1]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            bytes.push_back(static_cast<uint8_t>(high << 4 | low));
        }
        return bytes;
    }
    if (text[1] == 'b' || text[1] == 'B') {
        uint32_t bits = 0;
        int count = 0;
        for (char c : text.substr(2)) {
            if (c == '=') {
                break;
            }
            const int digit = Base64Digit(c);
            if (digit < 0) {
                return std::nullopt;
            }
            bits = bits << 6 | static_cast<uint32_t>(digit);
            count += 6;
            if (count >= 8) {
                count -= 8;
                bytes.push_back(static_cast<uint8_t>(bits >> count));
            }
        }
        return bytes;
    }
    return std::nullopt;
}

std::string EncodeHex(std::span<const uint8_t> bytes)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text = "0x";
    for (uint8_t byte : bytes) {
        text += kHex[byte >> 4];
        text += kHex[byte & 0x0F];
    }
    return text;
}

ChapResult ChapStep(ChapExchange& exchange, const TargetAccess& access, const std::string& targetName,
                    const util::KeyValueList& request)
{
    ChapResult result;
    using Step = ChapExchange::Step;

    if (exchange.step == Step::Start) {
        const std::optional<std::string_view> methods = request.Find("AuthMethod");
        if (!methods || !ListContains(*methods, "CHAP")) {
            return Fail("the target requires CHAP, which the initiator did not offer");
        }
        result.response.Add("AuthMethod", "CHAP");
        exchange.step = Step::MethodChosen;
    }

    if (exchange.step == Step::MethodChosen) {
        const std::optional<std::string_view> algorithms = request.Find("CHAP_A");
        if (!algorithms) {
            return result; // in the next request
        }
        if (!ListContains(*algorithms, kMd5)) {
            result.response.Add("CHAP_A", "Reject");
            result.outcome = ChapResult::Outcome::Failed;
            result.reason = "the initiator offered no CHAP algorithm the target supports (MD5)";
            return result;
        }
        uint8_t identifier = 0;
        exchange.challenge.assign(kChallengeLength, 0);
        if (!RandomBytes({&identifier, 1}) || !RandomBytes(exchange.challenge)) {
            return Fail("cannot generate a CHAP challenge");
        }
        exchange.identifier = identifier;
        result.response.Add("CHAP_A", std::string(kMd5));
        result.response.Add("CHAP_I", std::to_string(identifier));
        result.response.Add("CHAP_C", EncodeHex(exchange.challenge));
        exchange.step = Step::ChallengeSent;
        return result;
    }

    if (exchange.step != Step::ChallengeSent) {
        return result;
    }
    const std::optional<std::string_view> name = request.Find("CHAP_N");
    const std::optional<std::string_view> response = request.Find("CHAP_R");
    if (!name || !response) {
        return result;
    }
    if (!access.chapUser.empty() && *name != access.chapUser) {
        return Fail("unknown CHAP name " + std::string(*name));
    }
    const std::optional<std::vector<uint8_t>> received = DecodeBinaryValue(*response);
    const auto expected = ChapResponse(exchange.identifier, access.chapSecret, exchange.challenge);
    if (!received || !expected || !EqualConstantTime(*received, *expected)) {
        return Fail("wrong CHAP response (secret) from " + std::string(*name));
    }

    // Mutual CHAP: the initiator challenges the target in the same request.
    const std::optional<std::string_view> initiatorIdentifier = request.Find("CHAP_I");
    const std::optional<std::string_view> initiatorChallenge = request.Find("CHAP_C");
    if (initiatorIdentifier || initiatorChallenge) {
        if (!initiatorIdentifier || !initiatorChallenge) {
            return Fail("incomplete mutual CHAP challenge");
        }
        if (access.mutualSecret.empty()) {
            return Fail("the initiator asked for mutual CHAP, which the target has no secret for");
        }
        int identifier = -1;
        const auto [end, ec] = std::from_chars(initiatorIdentifier->data(),
                                               initiatorIdentifier->data() + initiatorIdentifier->size(), identifier);
        const std::optional<std::vector<uint8_t>> challenge = DecodeBinaryValue(*initiatorChallenge);
        if (ec != std::errc() || end != initiatorIdentifier->data() + initiatorIdentifier->size() || identifier < 0 ||
            identifier > 255 || !challenge || challenge->empty()) {
            return Fail("malformed mutual CHAP challenge");
        }
        // A challenge equal to ours would let the initiator have the target
        // compute the response it owes (reflection).
        if (EqualConstantTime(*challenge, exchange.challenge)) {
            return Fail("the initiator sent back the target's own challenge");
        }
        const auto targetResponse = ChapResponse(static_cast<uint8_t>(identifier), access.mutualSecret, *challenge);
        if (!targetResponse) {
            return Fail("cannot compute the mutual CHAP response");
        }
        result.response.Add("CHAP_N", access.mutualUser.empty() ? targetName : access.mutualUser);
        result.response.Add("CHAP_R", EncodeHex(*targetResponse));
    }
    exchange.step = Step::Done;
    result.outcome = ChapResult::Outcome::Done;
    return result;
}

} // namespace tiscsi::iscsi
