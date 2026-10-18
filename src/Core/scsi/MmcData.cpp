// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/scsi/MmcData.h"

#include "Core/util/ByteOrder.h"

#include <algorithm>
#include <initializer_list>
#include <utility>
#include <vector>

namespace tiscsi::scsi {

using util::ReadBE16;
using util::WriteBE16;
using util::WriteBE32;

namespace {

constexpr uint64_t kCdMaxSectors = 80 * 60 * 75;
constexpr uint8_t kLeadOutTrack = 0xAA;
constexpr uint8_t kAdrControlData = 0x14; // Q sub-channel mode 1, data track
constexpr uint8_t kMediaEventClass = 4;
constexpr uint32_t kDvdDataAreaStart = 0x030000; // first physical sector of the data area
constexpr uint16_t kReadSpeedKBps = 50 * 176;    // reported as 50x CD speed

void WriteMsf(uint8_t* p, uint64_t lba)
{
    // Addresses start after the 2 s pre-gap. A DVD is longer than 255
    // minutes: saturate at the largest address the fields can hold.
    const uint64_t frames = std::min<uint64_t>(lba + 150, (255 * 60 + 59) * 75 + 74);
    p[0] = static_cast<uint8_t>(frames / (60 * 75));
    p[1] = static_cast<uint8_t>(frames / 75 % 60);
    p[2] = static_cast<uint8_t>(frames % 75);
}

// 4-byte address field: LBA, or a reserved byte and minute/second/frame.
void WriteAddress(uint8_t* p, uint64_t lba, bool msf)
{
    if (msf) {
        p[0] = 0;
        WriteMsf(p + 1, lba);
    } else {
        WriteBE32(p, static_cast<uint32_t>(std::min<uint64_t>(lba, 0xFFFFFFFF)));
    }
}

void AppendTrackDescriptor(Bytes& b, uint8_t track, uint64_t lba, bool msf)
{
    uint8_t d[8] = {0, kAdrControlData, track, 0};
    WriteAddress(&d[4], lba, msf);
    b.insert(b.end(), std::begin(d), std::end(d));
}

// Raw TOC (Q sub-channel) descriptor of session 1; PMIN/PSEC/PFRAME given.
void AppendRawTocDescriptor(Bytes& b, uint8_t point, std::initializer_list<uint8_t> p)
{
    uint8_t d[11] = {1, kAdrControlData, 0, point};
    std::copy(p.begin(), p.end(), &d[8]);
    b.insert(b.end(), std::begin(d), std::end(d));
}

Bytes FeatureDescriptor(uint16_t code, uint8_t version, bool persistent, bool current, uint8_t additionalLength)
{
    Bytes b(4 + additionalLength, 0);
    WriteBE16(&b[0], code);
    b[2] = static_cast<uint8_t>(version << 2 | (persistent ? 0x02 : 0) | (current ? 0x01 : 0));
    b[3] = additionalLength;
    return b;
}

} // namespace

MmcProfile ProfileForDisc(uint64_t sectors)
{
    return sectors <= kCdMaxSectors ? MmcProfile::CdRom : MmcProfile::DvdRom;
}

std::optional<Bytes> TocData(uint64_t sectors, uint8_t format, bool msf, uint8_t track)
{
    Bytes b(4, 0);
    b[2] = 1; // first track (format 0) or session
    b[3] = 1; // last track or session
    switch (format) {
    case 0:
        if (track > 1 && track != kLeadOutTrack) {
            return std::nullopt;
        }
        if (track <= 1) {
            AppendTrackDescriptor(b, 1, 0, msf);
        }
        AppendTrackDescriptor(b, kLeadOutTrack, sectors, msf);
        break;
    case 1: // first track in the last session
        AppendTrackDescriptor(b, 1, 0, msf);
        break;
    case 2: { // always in MSF form
        if (track > 1) {
            return std::nullopt;
        }
        uint8_t leadOut[3];
        uint8_t trackStart[3];
        WriteMsf(leadOut, sectors);
        WriteMsf(trackStart, 0);
        AppendRawTocDescriptor(b, 0xA0, {1, 0x00, 0}); // first track, CD-DA or CD-ROM disc
        AppendRawTocDescriptor(b, 0xA1, {1, 0, 0});    // last track
        AppendRawTocDescriptor(b, 0xA2, {leadOut[0], leadOut[1], leadOut[2]});
        AppendRawTocDescriptor(b, 1, {trackStart[0], trackStart[1], trackStart[2]});
        break;
    }
    default: // PMA, ATIP and CD-TEXT do not exist on a pressed disc
        return std::nullopt;
    }
    WriteBE16(&b[0], static_cast<uint16_t>(b.size() - 2));
    return b;
}

std::optional<Bytes> ConfigurationData(MmcProfile current, uint8_t requestType, uint16_t startingFeature)
{
    if (requestType > 2) {
        return std::nullopt;
    }
    const bool dvd = current == MmcProfile::DvdRom;
    std::vector<Bytes> features;

    Bytes profiles = FeatureDescriptor(0x0000, 0, true, true, 8); // Profile List
    WriteBE16(&profiles[4], static_cast<uint16_t>(MmcProfile::DvdRom));
    profiles[6] = dvd ? 1 : 0;
    WriteBE16(&profiles[8], static_cast<uint16_t>(MmcProfile::CdRom));
    profiles[10] = dvd ? 0 : 1;
    features.push_back(std::move(profiles));

    Bytes core = FeatureDescriptor(0x0001, 2, true, true, 8);
    WriteBE32(&core[4], 1); // physical interface: SCSI family
    core[8] = 0x01;         // DBE (deferred errors are reported)
    features.push_back(std::move(core));

    Bytes removable = FeatureDescriptor(0x0003, 0, true, true, 4); // Removable Medium
    removable[4] = 0x21; // tray loading mechanism, lockable (PREVENT ALLOW MEDIUM REMOVAL)
    features.push_back(std::move(removable));

    Bytes randomReadable = FeatureDescriptor(0x0010, 0, false, true, 8);
    WriteBE32(&randomReadable[4], 2048);        // logical block size
    WriteBE16(&randomReadable[8], dvd ? 16 : 1); // blocking (ECC block)
    features.push_back(std::move(randomReadable));

    features.push_back(FeatureDescriptor(0x001E, 2, false, !dvd, 4)); // CD Read: no CD-DA, C2 or CD-Text
    features.push_back(FeatureDescriptor(0x001F, 0, false, dvd, 0));  // DVD Read

    Bytes b(8, 0);
    WriteBE16(&b[6], static_cast<uint16_t>(current));
    for (const Bytes& feature : features) {
        const uint16_t code = ReadBE16(feature.data());
        const bool wanted = requestType == 2 ? code == startingFeature
                                             : code >= startingFeature && (requestType == 0 || (feature[2] & 0x01));
        if (wanted) {
            b.insert(b.end(), feature.begin(), feature.end());
        }
    }
    WriteBE32(&b[0], static_cast<uint32_t>(b.size() - 4));
    return b;
}

Bytes EventStatusData(uint8_t notificationClassRequest)
{
    Bytes b(4, 0);
    b[3] = 1 << kMediaEventClass; // supported event classes
    if (notificationClassRequest & (1 << kMediaEventClass)) {
        b[2] = kMediaEventClass;
        b.insert(b.end(), {0x00, 0x02, 0, 0}); // no change; tray closed, media present
    } else {
        b[2] = 0x80; // NEA: no requested event class is supported
    }
    WriteBE16(&b[0], static_cast<uint16_t>(b.size() - 4));
    return b;
}

Bytes DiscInformationData()
{
    Bytes b(34, 0);
    WriteBE16(&b[0], 32);
    b[2] = 0x0E; // last session complete, disc finalized
    b[3] = 1;    // first track on the disc
    b[4] = 1;    // number of sessions
    b[5] = 1;    // first track in the last session
    b[6] = 1;    // last track in the last session
    b[7] = 0x20; // URU: unrestricted use
    b[8] = 0x00; // disc type: CD-DA or CD-ROM (also used for DVDs)
    return b;
}

std::optional<Bytes> TrackInformationData(uint64_t sectors, uint8_t addressType, uint32_t address)
{
    const bool valid = addressType == 0 ? address < sectors : (addressType == 1 || addressType == 2) && address == 1;
    if (!valid) {
        return std::nullopt;
    }
    Bytes b(36, 0);
    WriteBE16(&b[0], 34);
    b[2] = 1;    // track number
    b[3] = 1;    // session number
    b[5] = 0x04; // track mode: data, recorded uninterrupted
    b[6] = 0x01; // data mode 1
    WriteBE32(&b[24], static_cast<uint32_t>(std::min<uint64_t>(sectors, 0xFFFFFFFF))); // track size
    return b;
}

std::optional<Bytes> DvdStructureData(uint64_t sectors, uint8_t format, uint8_t layer)
{
    Bytes b;
    switch (format) {
    case 0x00: // physical format information
        if (layer != 0) {
            return std::nullopt;
        }
        b.assign(4 + 2048, 0);
        b[4] = 0x01; // DVD-ROM, part version 1
        b[5] = 0x0F; // 120 mm disc, maximum rate not specified
        b[6] = 0x01; // one layer, embossed (read-only)
        WriteBE32(&b[8], kDvdDataAreaStart);
        WriteBE32(&b[12], static_cast<uint32_t>(std::min<uint64_t>(kDvdDataAreaStart + sectors - 1, 0xFFFFFF)));
        break;
    case 0x01: // copyright information: no protection, all regions
        b.assign(4 + 4, 0);
        break;
    case 0x04: // disc manufacturing information
        b.assign(4 + 2048, 0);
        break;
    case 0xFF: { // list of the formats above
        struct Format {
            uint8_t code;
            uint16_t length;
        };
        constexpr Format kFormats[] = {{0x00, 4 + 2048}, {0x01, 4 + 4}, {0x04, 4 + 2048}};
        b.assign(4, 0);
        for (const Format& f : kFormats) {
            uint8_t d[4] = {f.code, 0x40}; // readable with READ DVD STRUCTURE
            WriteBE16(&d[2], f.length);
            b.insert(b.end(), std::begin(d), std::end(d));
        }
        break;
    }
    default:
        return std::nullopt;
    }
    WriteBE16(&b[0], static_cast<uint16_t>(b.size() - 2));
    return b;
}

Bytes MechanismStatusData()
{
    Bytes b(8, 0);
    b[5] = 1; // one slot (not a changer)
    return b;
}

Bytes ReadWriteErrorRecoveryModePage()
{
    Bytes b(12, 0);
    b[0] = static_cast<uint8_t>(ModePageCode::ReadWriteErrorRecovery);
    b[1] = 10;
    b[3] = 0x20; // read retry count
    return b;
}

Bytes MmCapabilitiesModePage()
{
    // The MMC-2 length, as most drives report it.
    Bytes b(2 + 0x14, 0);
    b[0] = static_cast<uint8_t>(ModePageCode::MmCapabilities);
    b[1] = 0x14;
    b[2] = 0x0B; // reads CD-R, CD-RW and DVD-ROM; writes nothing
    b[6] = 0x21; // tray loading mechanism, lockable
    WriteBE16(&b[8], kReadSpeedKBps);  // maximum read speed
    WriteBE16(&b[14], kReadSpeedKBps); // current read speed
    return b;
}

} // namespace tiscsi::scsi
