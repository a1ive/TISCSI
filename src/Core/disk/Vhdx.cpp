// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Implemented from Microsoft [MS-VHDX] v8.0 (2024-04-23).
// VirtualBox's VHDX reader was consulted for format support boundaries;
// this implementation does not depend on VirtualBox or the Windows VHD API.

#include "Core/disk/Vhdx.h"

#include <objbase.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <utility>

namespace tiscsi::disk {
namespace {

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kOffsetMask = 0xfffffffffff00000ull;
using Guid = std::array<uint8_t, 16>;
using Range = std::pair<uint64_t, uint64_t>;

uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t* p)
{
    return static_cast<uint32_t>(Le16(p)) | static_cast<uint32_t>(Le16(p + 2)) << 16;
}
uint64_t Le64(const uint8_t* p) { return Le32(p) | static_cast<uint64_t>(Le32(p + 4)) << 32; }
void Put32(uint8_t* p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void Put64(uint8_t* p, uint64_t v)
{
    Put32(p, static_cast<uint32_t>(v));
    Put32(p + 4, static_cast<uint32_t>(v >> 32));
}
Guid GetGuid(const uint8_t* p)
{
    Guid g;
    std::memcpy(g.data(), p, g.size());
    return g;
}
Guid Id(const wchar_t* text)
{
    GUID g{};
    CLSIDFromString(text, &g);
    return GetGuid(reinterpret_cast<const uint8_t*>(&g));
}
bool NewGuid(uint8_t* out)
{
    GUID g;
    if (FAILED(CoCreateGuid(&g))) return false;
    std::memcpy(out, &g, sizeof(g));
    return true;
}
const Guid kBat = Id(L"{2dc27766-f623-4200-9d64-115e9bfd4a08}");
const Guid kMetadata = Id(L"{8b7ca206-4790-4b9a-b8fe-575f050f886e}");
const Guid kParameters = Id(L"{caa16737-fa36-4d43-b3b6-33f0aa44e76b}");
const Guid kDiskSize = Id(L"{2fa54224-cd1b-4876-b211-5dbed83bf4b8}");
const Guid kDiskId = Id(L"{beca12ab-b2e6-4523-93ef-c309e000c746}");
const Guid kLogicalSector = Id(L"{8141bf1d-a96f-4709-ba47-f233a8faab5f}");
const Guid kPhysicalSector = Id(L"{cda348c7-445d-4471-9cc9-e9885251c556}");
const Guid kParent = Id(L"{a8d35f2d-b30b-454d-abf7-d3d84834ab0c}");
const Guid kParentType = Id(L"{b04aefb7-d19e-4a81-b789-25b8e9445913}");

uint32_t Crc32c(std::span<const uint8_t> data)
{
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) c = (c >> 1) ^ ((c & 1) ? 0x82f63b78u : 0);
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = UINT32_MAX;
    for (uint8_t b : data) c = table[(c ^ b) & 255] ^ (c >> 8);
    return ~c;
}
bool Checksum(std::span<uint8_t> data, const char* signature)
{
    if (std::memcmp(data.data(), signature, 4)) return false;
    const uint32_t saved = Le32(data.data() + 4);
    Put32(data.data() + 4, 0);
    const bool valid = saved == Crc32c(data);
    Put32(data.data() + 4, saved);
    return valid;
}
bool Fits(uint64_t offset, uint64_t length, uint64_t limit)
{
    return offset <= limit && length <= limit - offset;
}
bool Disjoint(std::vector<Range>& ranges)
{
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].first < ranges[i - 1].second) return false;
    }
    return true;
}
} // namespace

std::unique_ptr<VhdxDisk> VhdxDisk::Open(const std::wstring& path, bool readOnly, OpenError& error, int depth)
{
    if (depth > detail::kMaxParentDepth) {
        error.Set(ERROR_FILE_CORRUPT, path);
        return nullptr;
    }
    DWORD code;
    auto file = File::Open(path, readOnly ? File::Access::ReadShared : File::Access::ReadWriteExclusive, code);
    if (!file) {
        error.Set(code, path);
        return nullptr;
    }
    try {
        std::unique_ptr<VhdxDisk> disk(new VhdxDisk(std::move(file)));
        if (disk->Load(error, depth)) return disk;
    } catch (const std::bad_alloc&) {
        error.Set(ERROR_NOT_ENOUGH_MEMORY, path);
    }
    return nullptr;
}

bool VhdxDisk::Load(OpenError& error, int depth)
{
    error.Set(ERROR_FILE_CORRUPT, m_file->Path());
    std::array<uint8_t, 8> signature{};
    if (!m_file->GetSize(m_fileSize) || m_fileSize < kMiB ||
        !m_file->ReadAt(0, signature.data(), signature.size()) ||
        std::memcmp(signature.data(), "vhdxfile", 8)) return false;

    std::array<uint8_t, 4096> headers[2]{};
    bool valid[2]{};
    for (int i = 0; i < 2; ++i) {
        valid[i] = m_file->ReadAt((i + 1) * 65536ull, headers[i].data(), headers[i].size()) &&
                   Checksum(headers[i], "head");
    }
    if ((!valid[0] && !valid[1]) ||
        (valid[0] && valid[1] && Le64(headers[0].data() + 8) == Le64(headers[1].data() + 8))) return false;
    const int current = !valid[0] || (valid[1] && Le64(headers[1].data() + 8) > Le64(headers[0].data() + 8)) ? 1 : 0;
    m_header = headers[current];
    m_headerOffset = (current + 1) * 65536ull;
    // Do not read possibly stale metadata when an external log needs replay.
    if (Le16(m_header.data() + 66) != 1 || GetGuid(m_header.data() + 48) != Guid{}) {
        error.Set(ERROR_NOT_SUPPORTED, m_file->Path());
        return false;
    }
    const uint64_t logOffset = Le64(m_header.data() + 72);
    const uint32_t logLength = Le32(m_header.data() + 68);
    std::vector<Range> ranges{{0, kMiB}};
    if (logLength) {
        if (logOffset < kMiB || logOffset % kMiB || logLength % kMiB ||
            !Fits(logOffset, logLength, m_fileSize)) return false;
        ranges.emplace_back(logOffset, logOffset + logLength);
    }

    std::vector<uint8_t> table(65536);
    bool regionValid = false;
    for (uint64_t offset : {196608ull, 262144ull}) {
        if (m_file->ReadAt(offset, table.data(), table.size()) && Checksum(table, "regi")) {
            regionValid = true;
            break;
        }
    }
    if (!regionValid || Le32(table.data() + 8) > 2047) return false;
    uint64_t metadataOffset = 0;
    uint32_t metadataLength = 0, batLength = 0;
    std::set<Guid> regions;
    for (uint32_t i = 0; i < Le32(table.data() + 8); ++i) {
        const auto* p = table.data() + 16 + i * 32;
        const Guid id = GetGuid(p);
        const uint64_t offset = Le64(p + 16);
        const uint32_t length = Le32(p + 24), flags = Le32(p + 28);
        if (!regions.insert(id).second || offset < kMiB || offset % kMiB ||
            !length || length % kMiB || !Fits(offset, length, m_fileSize)) return false;
        ranges.emplace_back(offset, offset + length);
        if (id == kBat) {
            m_batOffset = offset;
            batLength = length;
        } else if (id == kMetadata) {
            metadataOffset = offset;
            metadataLength = length;
        } else if (flags & 1) {
            error.Set(ERROR_NOT_SUPPORTED, m_file->Path());
            return false;
        }
    }
    if (!batLength || !metadataLength || !Disjoint(ranges) ||
        !m_file->ReadAt(metadataOffset, table.data(), table.size()) ||
        std::memcmp(table.data(), "metadata", 8) || Le16(table.data() + 10) > 2047) return false;

    std::map<Guid, std::vector<uint8_t>> items;
    std::vector<Range> itemRanges{{0, 65536}};
    for (uint16_t i = 0; i < Le16(table.data() + 10); ++i) {
        const auto* p = table.data() + 32 + i * 32;
        const Guid id = GetGuid(p);
        const uint32_t offset = Le32(p + 16), length = Le32(p + 20), flags = Le32(p + 24);
        if (items.contains(id) || length > kMiB || !Fits(offset, length, metadataLength)) return false;
        if (length) itemRanges.emplace_back(offset, static_cast<uint64_t>(offset) + length);
        const bool known = id == kParameters || id == kDiskSize || id == kDiskId ||
                           id == kLogicalSector || id == kPhysicalSector || id == kParent;
        if (!known && (flags & 4)) {
            error.Set(ERROR_NOT_SUPPORTED, m_file->Path());
            return false;
        }
        if (known) {
            const uint32_t expected = id == kParameters || id == kParent ? 4 : 6;
            // Some existing writers omit IsRequired for physical sector size.
            if ((flags & 7) != expected && !(id == kPhysicalSector && (flags & 7) == 2)) return false;
        }
        auto& value = items[id];
        if (known) {
            value.resize(length);
            if (!m_file->ReadAt(metadataOffset + offset, value.data(), value.size())) return false;
        }
    }
    if (!Disjoint(itemRanges) || items[kParameters].size() != 8 || items[kDiskSize].size() != 8 ||
        items[kLogicalSector].size() != 4 || items[kPhysicalSector].size() != 4 || items[kDiskId].size() != 16) return false;
    m_blockSize = Le32(items[kParameters].data());
    const uint32_t parameters = Le32(items[kParameters].data() + 4);
    const bool hasParent = (parameters & 2) != 0;
    m_size = Le64(items[kDiskSize].data());
    m_sectorSize = Le32(items[kLogicalSector].data());
    const uint32_t physical = Le32(items[kPhysicalSector].data());
    if (parameters & ~3u || m_blockSize < kMiB || m_blockSize > 256 * kMiB ||
        (m_blockSize & (m_blockSize - 1)) || (m_sectorSize != 512 && m_sectorSize != 4096) ||
        (physical != 512 && physical != 4096) || !m_size || m_size > (64ull << 40) ||
        m_size % m_sectorSize) return false;
    m_chunkRatio = static_cast<uint32_t>((1ull << 23) * m_sectorSize / m_blockSize);
    const uint64_t blocks = (m_size + m_blockSize - 1) / m_blockSize;
    const uint64_t chunks = (blocks + m_chunkRatio - 1) / m_chunkRatio;
    const uint64_t entries = hasParent ? chunks * (m_chunkRatio + 1ull) : blocks + (blocks - 1) / m_chunkRatio;
    if (entries > batLength / 8 || entries > SIZE_MAX / sizeof(uint64_t)) return false;
    m_bat.resize(static_cast<size_t>(entries));
    if (!m_file->ReadAt(m_batOffset, m_bat.data(), m_bat.size() * sizeof(uint64_t))) return false;
    // Win32 and Win64 are little-endian. Validate every occupied BAT range,
    // including slots beyond the last payload block and unused bitmaps.
    for (size_t i = 0; i < m_bat.size(); ++i) {
        const uint64_t entry = m_bat[i], offset = entry & kOffsetMask;
        const unsigned state = static_cast<unsigned>(entry & 7);
        const bool bitmap = i % (m_chunkRatio + 1ull) == m_chunkRatio;
        if (entry & 0xffff8 || (bitmap ? state != 0 && state != 6 : state == 4 || state == 5)) return false;
        if (!bitmap && state == 7 && !hasParent) return false;
        if (state == 6 || state == 7 || offset) {
            const uint64_t length = bitmap ? kMiB : m_blockSize;
            if (offset < kMiB || !Fits(offset, length, m_fileSize)) return false;
            ranges.emplace_back(offset, offset + length);
        }
    }
    if (!Disjoint(ranges)) return false;
    for (uint64_t block = 0; block < blocks; ++block) {
        if ((m_bat[BatIndex(block)] & 7) == 7 &&
            (m_bat[static_cast<size_t>((block / m_chunkRatio + 1) * (m_chunkRatio + 1ull) - 1)] & 7) != 6) return false;
    }
    if (hasParent && !OpenParent(items[kParent], error, depth)) return false;
    error.Set(ERROR_SUCCESS, {});
    return true;
}

bool VhdxDisk::OpenParent(const std::vector<uint8_t>& locator, OpenError& error, int depth)
{
    if (locator.size() < 20 || GetGuid(locator.data()) != kParentType) return false;
    const uint16_t count = Le16(locator.data() + 18);
    const size_t start = 20 + static_cast<size_t>(count) * 12;
    if (start > locator.size()) return false;
    std::map<std::wstring, std::wstring> fields;
    auto stringAt = [&](uint32_t offset, uint16_t length, std::wstring& text) {
        if (!length || length % 2 || offset < start || !Fits(offset, length, locator.size())) return false;
        for (size_t i = 0; i < length; i += 2) {
            const wchar_t c = static_cast<wchar_t>(Le16(locator.data() + offset + i));
            if (!c) return false;
            text.push_back(c);
        }
        return true;
    };
    for (unsigned i = 0; i < count; ++i) {
        const auto* p = locator.data() + 20 + i * 12;
        std::wstring key, value;
        if (!stringAt(Le32(p), Le16(p + 8), key) || !stringAt(Le32(p + 4), Le16(p + 10), value) ||
            !fields.emplace(key, value).second) return false;
    }
    GUID linkage{};
    if (!fields.contains(L"parent_linkage") || FAILED(CLSIDFromString(fields[L"parent_linkage"].c_str(), &linkage))) return false;
    const Guid parentId = GetGuid(reinterpret_cast<uint8_t*>(&linkage));
    const std::wstring directory = DirectoryOf(FullPath(m_file->Path()));
    OpenError last;
    last.Set(ERROR_FILE_NOT_FOUND, m_file->Path());
    for (const wchar_t* key : {L"relative_path", L"volume_path", L"absolute_win32_path"}) {
        auto it = fields.find(key);
        if (it == fields.end()) continue;
        const auto candidate = ResolveRelative(directory, it->second);
        auto parent = Open(candidate, true, last, depth + 1);
        if (!parent) continue;
        if (parent->Size() != m_size || parent->BytesPerSector() != m_sectorSize ||
            GetGuid(parent->m_header.data() + 32) != parentId) {
            last.Set(ERROR_FILE_CORRUPT, candidate);
            continue;
        }
        m_parent = std::move(parent);
        return true;
    }
    error = std::move(last);
    return false;
}

size_t VhdxDisk::BatIndex(uint64_t block) const { return static_cast<size_t>(block + block / m_chunkRatio); }

bool VhdxDisk::Read(uint64_t offset, size_t length, uint8_t* out)
{
    while (length) {
        const uint64_t block = offset / m_blockSize;
        const uint32_t first = static_cast<uint32_t>(offset % m_blockSize);
        size_t take = std::min(length, static_cast<size_t>(m_blockSize - first));
        const uint64_t entry = m_bat[BatIndex(block)];
        const unsigned state = static_cast<unsigned>(entry & 7);
        bool fromParent = state == 0 && m_parent;
        bool fromFile = state == 6;
        if (state == 7) {
            const uint64_t chunk = block / m_chunkRatio;
            if (m_bitmapChunk != chunk) {
                m_bitmap.resize(static_cast<size_t>(kMiB));
                const uint64_t bitmap = m_bat[static_cast<size_t>((chunk + 1) * (m_chunkRatio + 1ull) - 1)];
                if (!m_file->ReadAt(bitmap & kOffsetMask, m_bitmap.data(), m_bitmap.size())) return false;
                m_bitmapChunk = chunk;
            }
            const uint64_t bit = (offset / m_sectorSize) % (1ull << 23);
            auto present = [&](uint64_t b) { return (m_bitmap[static_cast<size_t>(b / 8)] >> (b % 8)) & 1; };
            fromFile = present(bit) != 0;
            fromParent = !fromFile;
            size_t run = m_sectorSize;
            while (run < take && (present(bit + run / m_sectorSize) != 0) == fromFile) run += m_sectorSize;
            take = run;
        }
        if (fromFile) {
            if (!m_file->ReadAt((entry & kOffsetMask) + first, out, take)) return false;
        } else if (fromParent) {
            if (!m_parent->Read(offset, take, out)) return false;
        } else {
            std::memset(out, 0, take);
        }
        offset += take;
        out += take;
        length -= take;
    }
    return true;
}

IoResult VhdxDisk::ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out)
{
    if (!IsInRange(lba, count) || count > SIZE_MAX / m_sectorSize) return IoResult::OutOfRange;
    if (m_failed) return IoResult::ReadError;
    try {
        out.resize(static_cast<size_t>(count) * m_sectorSize);
        return Read(lba * m_sectorSize, out.size(), out.data()) ? IoResult::Ok : IoResult::ReadError;
    } catch (const std::bad_alloc&) {
        return IoResult::ReadError;
    }
}

bool VhdxDisk::UpdateHeaders()
{
    for (int i = 0; i < 2; ++i) {
        const uint64_t sequence = Le64(m_header.data() + 8);
        if (sequence == UINT64_MAX) return false;
        Put64(m_header.data() + 8, sequence + 1);
        Put32(m_header.data() + 4, 0);
        Put32(m_header.data() + 4, Crc32c(m_header));
        const uint64_t next = m_headerOffset == 65536 ? 131072 : 65536;
        if (!m_file->WriteAt(next, m_header.data(), m_header.size()) || !m_file->Flush()) return false;
        m_headerOffset = next;
    }
    return true;
}

bool VhdxDisk::BeginWrite()
{
    if (m_writeStarted) return true;
    if (!NewGuid(m_header.data() + 16) || !NewGuid(m_header.data() + 32) || !UpdateHeaders()) return false;
    m_writeStarted = true;
    return true;
}

bool VhdxDisk::CommitBat(size_t index, uint64_t entry)
{
    std::array<uint8_t, 4096> sector{};
    const uint64_t sectorOffset = m_batOffset + (index * 8ull / 4096) * 4096;
    if (!m_file->ReadAt(sectorOffset, sector.data(), sector.size())) return false;
    Put64(sector.data() + (index * 8ull % 4096), entry);

    // A new log region avoids a crash window in which a new LogGuid would
    // refer to no valid entry. At most 1 MiB is appended per session that
    // changes the BAT; old log storage is left unreferenced.
    if (!m_logOffset) {
        if (m_fileSize > INT64_MAX - 2 * kMiB || !NewGuid(m_logGuid.data())) return false;
        m_logOffset = (m_fileSize + kMiB - 1) & ~(kMiB - 1);
        m_fileSize = m_logOffset + kMiB;
        if (!m_file->SetSize(m_fileSize)) return false;
        std::array<uint8_t, 4096> checkpoint{};
        std::memcpy(checkpoint.data(), "loge", 4);
        Put32(checkpoint.data() + 8, 4096);
        Put64(checkpoint.data() + 16, ++m_logSequence);
        std::memcpy(checkpoint.data() + 32, m_logGuid.data(), 16);
        Put64(checkpoint.data() + 48, m_fileSize);
        Put64(checkpoint.data() + 56, m_fileSize);
        Put32(checkpoint.data() + 4, Crc32c(checkpoint));
        if (!m_file->WriteAt(m_logOffset, checkpoint.data(), checkpoint.size()) || !m_file->Flush()) return false;
        Put64(m_header.data() + 72, m_logOffset);
        Put32(m_header.data() + 68, static_cast<uint32_t>(kMiB));
        m_header[64] = m_header[65] = 0;
    }
    std::memcpy(m_header.data() + 48, m_logGuid.data(), 16);
    if (!UpdateHeaders()) return false;

    // Alternate two independent entries, each its own tail. The previous
    // entry remains valid until the new entry and its CRC are durable.
    const uint64_t sequence = ++m_logSequence;
    const uint32_t slot = sequence % 2 ? 0 : 8192;
    std::array<uint8_t, 8192> log{};
    std::memcpy(log.data(), "loge", 4);
    Put32(log.data() + 8, static_cast<uint32_t>(log.size()));
    Put32(log.data() + 12, slot);
    Put64(log.data() + 16, sequence);
    Put32(log.data() + 24, 1);
    std::memcpy(log.data() + 32, m_logGuid.data(), 16);
    Put64(log.data() + 48, m_fileSize);
    Put64(log.data() + 56, m_fileSize);
    std::memcpy(log.data() + 64, "desc", 4);
    std::memcpy(log.data() + 68, sector.data() + 4092, 4);
    std::memcpy(log.data() + 72, sector.data(), 8);
    Put64(log.data() + 80, sectorOffset);
    Put64(log.data() + 88, sequence);
    std::memcpy(log.data() + 4096, "data", 4);
    Put32(log.data() + 4100, static_cast<uint32_t>(sequence >> 32));
    std::memcpy(log.data() + 4104, sector.data() + 8, 4084);
    Put32(log.data() + 8188, static_cast<uint32_t>(sequence));
    Put32(log.data() + 4, Crc32c(log));
    if (!m_file->WriteAt(m_logOffset + slot, log.data(), log.size()) || !m_file->Flush() ||
        !m_file->WriteAt(sectorOffset, sector.data(), sector.size()) || !m_file->Flush()) return false;
    std::fill_n(m_header.data() + 48, 16, uint8_t{0});
    if (!UpdateHeaders()) return false;
    m_bat[index] = entry;
    return true;
}

bool VhdxDisk::Allocate(uint64_t block, uint32_t first, std::span<const uint8_t> data)
{
    if (m_fileSize > INT64_MAX - kMiB - m_blockSize) return false;
    const uint64_t allocation = (m_fileSize + kMiB - 1) & ~(kMiB - 1);
    m_fileSize = allocation + m_blockSize;
    if (!m_file->SetSize(m_fileSize)) return false;
    // Materialize a full block from its current virtual contents. This also
    // handles partial child blocks without changing their sector bitmap.
    std::vector<uint8_t> buffer(static_cast<size_t>(kMiB));
    const uint64_t start = block * m_blockSize;
    for (uint32_t offset = 0; offset < m_blockSize; offset += static_cast<uint32_t>(kMiB)) {
        std::fill(buffer.begin(), buffer.end(), uint8_t{0});
        const size_t available = start + offset < m_size ?
            static_cast<size_t>(std::min(kMiB, m_size - start - offset)) : 0;
        if (available && !Read(start + offset, available, buffer.data())) return false;
        const size_t begin = std::max<size_t>(offset, first);
        const size_t end = std::min<size_t>(offset + kMiB, first + data.size());
        if (begin < end) std::memcpy(buffer.data() + begin - offset, data.data() + begin - first, end - begin);
        if (!m_file->WriteAt(allocation + offset, buffer.data(), buffer.size())) return false;
    }
    return m_file->Flush() && CommitBat(BatIndex(block), allocation | 6);
}

IoResult VhdxDisk::WriteSectors(uint64_t lba, std::span<const uint8_t> data)
{
    if (IsReadOnly() || m_failed) return IoResult::WriteError;
    if (data.size() % m_sectorSize || !IsInRange(lba, data.size() / m_sectorSize)) return IoResult::OutOfRange;
    if (data.empty()) return IoResult::Ok;
    try {
        if (!BeginWrite()) {
            m_failed = true;
            return IoResult::WriteError;
        }
        uint64_t offset = lba * m_sectorSize;
        while (!data.empty()) {
            const uint64_t block = offset / m_blockSize;
            const uint32_t first = static_cast<uint32_t>(offset % m_blockSize);
            const size_t take = std::min(data.size(), static_cast<size_t>(m_blockSize - first));
            const uint64_t entry = m_bat[BatIndex(block)];
            const bool ok = (entry & 7) == 6 ?
                m_file->WriteAt((entry & kOffsetMask) + first, data.data(), take) :
                Allocate(block, first, data.first(take));
            if (!ok) {
                m_failed = true;
                return IoResult::WriteError;
            }
            offset += take;
            data = data.subspan(take);
        }
        return IoResult::Ok;
    } catch (const std::bad_alloc&) {
        m_failed = true;
        return IoResult::WriteError;
    }
}

IoResult VhdxDisk::Flush()
{
    if (m_failed) return IoResult::WriteError;
    return IsReadOnly() || m_file->Flush() ? IoResult::Ok : IoResult::WriteError;
}

} // namespace tiscsi::disk
