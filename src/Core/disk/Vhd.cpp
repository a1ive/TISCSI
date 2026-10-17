// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/disk/Vhd.h"

#include "Core/util/ByteOrder.h"

#include <objbase.h>

#include <algorithm>
#include <cstring>

namespace tiscsi::disk {

using util::ReadBE16;
using util::ReadBE32;
using util::ReadBE64;
using util::WriteBE16;
using util::WriteBE32;
using util::WriteBE64;

namespace {

constexpr char kFooterCookie[] = "conectix";
constexpr char kDynamicCookie[] = "cxsparse";
constexpr uint32_t kUnusedEntry = 0xFFFFFFFF;
constexpr uint32_t kPlatformW2ru = 0x57327275; // relative Windows path, UTF-16LE
constexpr uint32_t kPlatformW2ku = 0x57326B75; // absolute Windows path, UTF-16LE
constexpr uint32_t kCreatorApplication = 0x74697363; // "tisc"
constexpr uint32_t kCreatorVersion = 0x00010000;
constexpr uint32_t kCreatorHostWindows = 0x5769326B; // "Wi2k"
constexpr size_t kBitmapCacheLimit = 4096;

uint32_t Checksum(const uint8_t* bytes, size_t length, size_t checksumOffset)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        if (i < checksumOffset || i >= checksumOffset + 4) {
            sum += bytes[i];
        }
    }
    return ~sum;
}

uint32_t CurrentTimeStamp()
{
    // FILETIME of 2000-01-01 00:00:00 UTC
    constexpr uint64_t kEpoch2000 = 125911584000000000ull;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const uint64_t now = static_cast<uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime;
    return now > kEpoch2000 ? static_cast<uint32_t>((now - kEpoch2000) / 10000000) : 0;
}

Guid16 NewUniqueId()
{
    GUID guid;
    Guid16 id{};
    if (SUCCEEDED(CoCreateGuid(&guid))) {
        std::memcpy(id.data(), &guid, id.size());
    }
    return id;
}

// CHS geometry algorithm from the VHD specification (appendix).
void ComputeGeometry(uint64_t size, VhdFooter& footer)
{
    uint64_t totalSectors = std::min<uint64_t>(size / 512, 65535ull * 16 * 255);
    uint32_t sectorsPerTrack;
    uint32_t heads;
    uint64_t cylinderTimesHeads;
    if (totalSectors >= 65535ull * 16 * 63) {
        sectorsPerTrack = 255;
        heads = 16;
        cylinderTimesHeads = totalSectors / sectorsPerTrack;
    } else {
        sectorsPerTrack = 17;
        cylinderTimesHeads = totalSectors / sectorsPerTrack;
        heads = static_cast<uint32_t>((cylinderTimesHeads + 1023) / 1024);
        heads = std::max(heads, 4u);
        if (cylinderTimesHeads >= heads * 1024ull || heads > 16) {
            sectorsPerTrack = 31;
            heads = 16;
            cylinderTimesHeads = totalSectors / sectorsPerTrack;
        }
        if (cylinderTimesHeads >= heads * 1024ull) {
            sectorsPerTrack = 63;
            heads = 16;
            cylinderTimesHeads = totalSectors / sectorsPerTrack;
        }
    }
    footer.cylinders = static_cast<uint16_t>(cylinderTimesHeads / heads);
    footer.heads = static_cast<uint8_t>(heads);
    footer.sectorsPerTrack = static_cast<uint8_t>(sectorsPerTrack);
}

VhdFooter NewFooter(uint64_t size, VhdFooter::DiskType type)
{
    VhdFooter footer;
    footer.dataOffset = type == VhdFooter::DiskType::Fixed ? UINT64_MAX : VhdFooter::kSize;
    footer.timeStamp = CurrentTimeStamp();
    footer.creatorApplication = kCreatorApplication;
    footer.creatorVersion = kCreatorVersion;
    footer.creatorHostOs = kCreatorHostWindows;
    footer.originalSize = size;
    footer.currentSize = size;
    ComputeGeometry(size, footer);
    footer.diskType = type;
    footer.uniqueId = NewUniqueId();
    return footer;
}

bool BitIsSet(const std::vector<uint8_t>& bitmap, uint32_t sector)
{
    return (bitmap[sector / 8] & (0x80 >> (sector % 8))) != 0;
}

void SetBits(std::vector<uint8_t>& bitmap, uint32_t first, uint32_t count)
{
    for (uint32_t s = first; s < first + count; ++s) {
        bitmap[s / 8] |= static_cast<uint8_t>(0x80 >> (s % 8));
    }
}

bool FileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

} // namespace

// ---- On-disk structures --------------------------------------------------

std::optional<VhdFooter> VhdFooter::Parse(const uint8_t* b)
{
    if (std::memcmp(b, kFooterCookie, 8) != 0 || ReadBE32(b + 64) != Checksum(b, kSize, 64)) {
        return std::nullopt;
    }
    VhdFooter f;
    f.features = ReadBE32(b + 8);
    f.fileFormatVersion = ReadBE32(b + 12);
    f.dataOffset = ReadBE64(b + 16);
    f.timeStamp = ReadBE32(b + 24);
    f.creatorApplication = ReadBE32(b + 28);
    f.creatorVersion = ReadBE32(b + 32);
    f.creatorHostOs = ReadBE32(b + 36);
    f.originalSize = ReadBE64(b + 40);
    f.currentSize = ReadBE64(b + 48);
    f.cylinders = ReadBE16(b + 56);
    f.heads = b[58];
    f.sectorsPerTrack = b[59];
    f.diskType = static_cast<DiskType>(ReadBE32(b + 60));
    std::memcpy(f.uniqueId.data(), b + 68, 16);
    f.savedState = b[84];
    return f;
}

std::array<uint8_t, VhdFooter::kSize> VhdFooter::Serialize() const
{
    std::array<uint8_t, kSize> b{};
    std::memcpy(b.data(), kFooterCookie, 8);
    WriteBE32(&b[8], features);
    WriteBE32(&b[12], fileFormatVersion);
    WriteBE64(&b[16], dataOffset);
    WriteBE32(&b[24], timeStamp);
    WriteBE32(&b[28], creatorApplication);
    WriteBE32(&b[32], creatorVersion);
    WriteBE32(&b[36], creatorHostOs);
    WriteBE64(&b[40], originalSize);
    WriteBE64(&b[48], currentSize);
    WriteBE16(&b[56], cylinders);
    b[58] = heads;
    b[59] = sectorsPerTrack;
    WriteBE32(&b[60], static_cast<uint32_t>(diskType));
    std::memcpy(&b[68], uniqueId.data(), 16);
    b[84] = savedState;
    WriteBE32(&b[64], Checksum(b.data(), kSize, 64));
    return b;
}

std::optional<VhdDynamicHeader> VhdDynamicHeader::Parse(const uint8_t* b)
{
    if (std::memcmp(b, kDynamicCookie, 8) != 0 || ReadBE32(b + 36) != Checksum(b, kSize, 36)) {
        return std::nullopt;
    }
    VhdDynamicHeader h;
    h.tableOffset = ReadBE64(b + 16);
    h.headerVersion = ReadBE32(b + 24);
    h.maxTableEntries = ReadBE32(b + 28);
    h.blockSize = ReadBE32(b + 32);
    std::memcpy(h.parentUniqueId.data(), b + 40, 16);
    h.parentTimeStamp = ReadBE32(b + 56);
    for (size_t i = 0; i < 256; ++i) {
        const wchar_t c = static_cast<wchar_t>(ReadBE16(b + 64 + i * 2));
        if (c == 0) {
            break;
        }
        h.parentUnicodeName.push_back(c);
    }
    for (size_t i = 0; i < h.parentLocators.size(); ++i) {
        const uint8_t* p = b + 576 + i * 24;
        h.parentLocators[i] = {ReadBE32(p), ReadBE32(p + 4), ReadBE32(p + 8), ReadBE64(p + 16)};
    }
    return h;
}

std::array<uint8_t, VhdDynamicHeader::kSize> VhdDynamicHeader::Serialize() const
{
    std::array<uint8_t, kSize> b{};
    std::memcpy(b.data(), kDynamicCookie, 8);
    WriteBE64(&b[8], UINT64_MAX);
    WriteBE64(&b[16], tableOffset);
    WriteBE32(&b[24], headerVersion);
    WriteBE32(&b[28], maxTableEntries);
    WriteBE32(&b[32], blockSize);
    std::memcpy(&b[40], parentUniqueId.data(), 16);
    WriteBE32(&b[56], parentTimeStamp);
    for (size_t i = 0; i < parentUnicodeName.size() && i < 256; ++i) {
        WriteBE16(&b[64 + i * 2], static_cast<uint16_t>(parentUnicodeName[i]));
    }
    for (size_t i = 0; i < parentLocators.size(); ++i) {
        uint8_t* p = &b[576 + i * 24];
        WriteBE32(p, parentLocators[i].platformCode);
        WriteBE32(p + 4, parentLocators[i].platformDataSpace);
        WriteBE32(p + 8, parentLocators[i].platformDataLength);
        WriteBE64(p + 16, parentLocators[i].platformDataOffset);
    }
    WriteBE32(&b[36], Checksum(b.data(), kSize, 36));
    return b;
}

// ---- Opening ---------------------------------------------------------------

std::unique_ptr<VhdDisk> VhdDisk::Open(const std::wstring& path, bool readOnly, OpenError& error, int depth)
{
    DWORD code = ERROR_SUCCESS;
    auto file = File::Open(path, readOnly ? File::Access::ReadShared : File::Access::ReadWriteExclusive, code);
    uint64_t fileSize = 0;
    if (!file || !file->GetSize(fileSize)) {
        error.Set(file ? GetLastError() : code, path);
        return nullptr;
    }
    if (fileSize < VhdFooter::kSize) {
        error.Set(ERROR_FILE_CORRUPT, path);
        return nullptr;
    }

    std::array<uint8_t, VhdFooter::kSize> bytes;
    std::optional<VhdFooter> footer;
    bool trailingFooterValid = false;
    if (file->ReadAt(fileSize - VhdFooter::kSize, bytes.data(), bytes.size())) {
        footer = VhdFooter::Parse(bytes.data());
        trailingFooterValid = footer.has_value();
    }
    // Dynamic disks keep a copy of the footer at offset 0.
    if (!footer && file->ReadAt(0, bytes.data(), bytes.size())) {
        footer = VhdFooter::Parse(bytes.data());
        if (footer && footer->diskType == VhdFooter::DiskType::Fixed) {
            footer.reset();
        }
    }
    if (!footer || footer->currentSize == 0 || footer->currentSize % kBytesPerSector != 0) {
        error.Set(ERROR_FILE_CORRUPT, path);
        return nullptr;
    }

    std::unique_ptr<VhdDisk> disk(new VhdDisk(std::move(file), *footer));
    switch (footer->diskType) {
    case VhdFooter::DiskType::Fixed:
        if (!trailingFooterValid || footer->currentSize > fileSize - VhdFooter::kSize) {
            error.Set(ERROR_FILE_CORRUPT, path);
            return nullptr;
        }
        return disk;
    case VhdFooter::DiskType::Dynamic:
    case VhdFooter::DiskType::Differencing:
        if (!disk->LoadDynamic(error, fileSize, trailingFooterValid, depth)) {
            return nullptr;
        }
        return disk;
    default:
        error.Set(ERROR_NOT_SUPPORTED, path);
        return nullptr;
    }
}

VhdDisk::VhdDisk(std::unique_ptr<File> file, const VhdFooter& footer) : m_file(std::move(file)), m_footer(footer) {}

bool VhdDisk::LoadDynamic(OpenError& error, uint64_t fileSize, bool trailingFooterValid, int depth)
{
    const std::wstring& path = m_file->Path();
    std::array<uint8_t, VhdDynamicHeader::kSize> bytes;
    std::optional<VhdDynamicHeader> header;
    if (m_footer.dataOffset <= fileSize - bytes.size() && m_file->ReadAt(m_footer.dataOffset, bytes.data(), bytes.size())) {
        header = VhdDynamicHeader::Parse(bytes.data());
    }
    const uint64_t sizeInBlocks =
        header && header->blockSize ? (m_footer.currentSize + header->blockSize - 1) / header->blockSize : 0;
    if (!header || header->blockSize < kBytesPerSector || header->blockSize % kBytesPerSector != 0 ||
        header->maxTableEntries < sizeInBlocks) {
        error.Set(ERROR_FILE_CORRUPT, path);
        return false;
    }
    m_header = *header;
    m_sectorsPerBlock = m_header.blockSize / kBytesPerSector;
    m_bitmapSize = ((m_sectorsPerBlock + 7) / 8 + kBytesPerSector - 1) / kBytesPerSector * kBytesPerSector;

    std::vector<uint8_t> table(static_cast<size_t>(m_header.maxTableEntries) * 4);
    if (!m_file->ReadAt(m_header.tableOffset, table.data(), table.size())) {
        error.Set(ERROR_FILE_CORRUPT, path);
        return false;
    }
    m_bat.resize(m_header.maxTableEntries);
    // New blocks go where the trailing footer is, but never over an existing block.
    uint64_t end = trailingFooterValid ? fileSize - VhdFooter::kSize : fileSize;
    for (uint32_t i = 0; i < m_header.maxTableEntries; ++i) {
        m_bat[i] = ReadBE32(&table[i * 4]);
        if (m_bat[i] != kUnusedEntry) {
            end = std::max(end, static_cast<uint64_t>(m_bat[i]) * kBytesPerSector + m_bitmapSize + m_header.blockSize);
        }
    }
    m_nextBlockOffset = (end + kBytesPerSector - 1) / kBytesPerSector * kBytesPerSector;

    if (m_footer.diskType == VhdFooter::DiskType::Differencing) {
        return OpenParent(error, depth);
    }
    return true;
}

bool VhdDisk::OpenParent(OpenError& error, int depth)
{
    const std::wstring& path = m_file->Path();
    if (depth >= detail::kMaxParentDepth) {
        error.Set(ERROR_FILE_CORRUPT, path);
        return false;
    }
    const std::wstring directory = DirectoryOf(FullPath(path));

    auto readLocator = [this](const VhdDynamicHeader::ParentLocator& locator) {
        std::wstring text;
        if (locator.platformDataLength == 0 || locator.platformDataLength > 65536) {
            return text;
        }
        std::vector<uint8_t> data(locator.platformDataLength);
        if (!m_file->ReadAt(locator.platformDataOffset, data.data(), data.size())) {
            return text;
        }
        for (size_t i = 0; i + 1 < data.size(); i += 2) {
            const wchar_t c = static_cast<wchar_t>(data[i] | data[i + 1] << 8);
            if (c == 0) {
                break;
            }
            text.push_back(c);
        }
        return text;
    };

    // Relative locator first (survives moving both files), then absolute,
    // then the bare parent file name next to the child.
    std::wstring parentPath;
    std::wstring firstCandidate;
    for (uint32_t code : {kPlatformW2ru, kPlatformW2ku}) {
        for (const auto& locator : m_header.parentLocators) {
            if (locator.platformCode != code || !parentPath.empty()) {
                continue;
            }
            const std::wstring text = readLocator(locator);
            if (text.empty()) {
                continue;
            }
            const std::wstring candidate = code == kPlatformW2ru ? ResolveRelative(directory, text) : text;
            if (firstCandidate.empty()) {
                firstCandidate = candidate;
            }
            if (FileExists(candidate)) {
                parentPath = candidate;
            }
        }
    }
    if (parentPath.empty() && !m_header.parentUnicodeName.empty()) {
        const std::wstring candidate = directory + m_header.parentUnicodeName;
        if (firstCandidate.empty()) {
            firstCandidate = candidate;
        }
        if (FileExists(candidate)) {
            parentPath = candidate;
        }
    }
    if (parentPath.empty()) {
        error.Set(ERROR_FILE_NOT_FOUND, firstCandidate.empty() ? path : firstCandidate);
        return false;
    }

    m_parent = detail::OpenDiskImage(parentPath, true, error, depth + 1);
    if (!m_parent) {
        return false;
    }
    // The parent must be the exact disk the child was created from.
    const auto* parentVhd = dynamic_cast<const VhdDisk*>(m_parent.get());
    if (!parentVhd || parentVhd->UniqueId() != m_header.parentUniqueId || m_parent->Size() != Size()) {
        error.Set(ERROR_FILE_CORRUPT, parentPath);
        return false;
    }
    return true;
}

// ---- I/O ---------------------------------------------------------------

const std::vector<uint8_t>* VhdDisk::GetBitmap(uint32_t block)
{
    auto it = m_bitmapCache.find(block);
    if (it != m_bitmapCache.end()) {
        return &it->second;
    }
    std::vector<uint8_t> bitmap(m_bitmapSize);
    if (!m_file->ReadAt(static_cast<uint64_t>(m_bat[block]) * kBytesPerSector, bitmap.data(), bitmap.size())) {
        return nullptr;
    }
    if (m_bitmapCache.size() >= kBitmapCacheLimit) {
        m_bitmapCache.clear();
    }
    return &(m_bitmapCache[block] = std::move(bitmap));
}

bool VhdDisk::ReadParent(uint64_t lba, uint32_t count, uint8_t* out)
{
    std::vector<uint8_t> buffer;
    if (m_parent->ReadSectors(lba, count, buffer) != IoResult::Ok) {
        return false;
    }
    std::memcpy(out, buffer.data(), buffer.size());
    return true;
}

bool VhdDisk::ReadBlockSectors(uint32_t block, uint32_t first, uint32_t count, uint64_t lba, uint8_t* out)
{
    if (m_bat[block] == kUnusedEntry) {
        if (m_parent) {
            return ReadParent(lba, count, out);
        }
        std::memset(out, 0, static_cast<size_t>(count) * kBytesPerSector);
        return true;
    }
    const uint64_t dataOffset = static_cast<uint64_t>(m_bat[block]) * kBytesPerSector + m_bitmapSize;
    if (!m_parent) {
        // Dynamic disk: unwritten sectors of an allocated block are zero, so
        // the bitmap does not need to be consulted.
        return m_file->ReadAt(dataOffset + static_cast<uint64_t>(first) * kBytesPerSector, out,
                              static_cast<size_t>(count) * kBytesPerSector);
    }
    // Differencing disk: the bitmap says, per sector, whether the data is
    // here (1) or in the parent (0). Handle runs of equal bits at once.
    const std::vector<uint8_t>* bitmap = GetBitmap(block);
    if (!bitmap) {
        return false;
    }
    uint32_t sector = first;
    const uint32_t end = first + count;
    while (sector < end) {
        const bool present = BitIsSet(*bitmap, sector);
        uint32_t runEnd = sector + 1;
        while (runEnd < end && BitIsSet(*bitmap, runEnd) == present) {
            ++runEnd;
        }
        uint8_t* dst = out + static_cast<size_t>(sector - first) * kBytesPerSector;
        const uint32_t run = runEnd - sector;
        const bool ok = present ? m_file->ReadAt(dataOffset + static_cast<uint64_t>(sector) * kBytesPerSector, dst,
                                                 static_cast<size_t>(run) * kBytesPerSector)
                                : ReadParent(lba + (sector - first), run, dst);
        if (!ok) {
            return false;
        }
        sector = runEnd;
    }
    return true;
}

IoResult VhdDisk::ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out)
{
    if (!IsInRange(lba, count)) {
        return IoResult::OutOfRange;
    }
    out.resize(static_cast<size_t>(count) * kBytesPerSector);
    if (m_footer.diskType == VhdFooter::DiskType::Fixed) {
        return m_file->ReadAt(lba * kBytesPerSector, out.data(), out.size()) ? IoResult::Ok : IoResult::ReadError;
    }
    uint8_t* dst = out.data();
    while (count > 0) {
        const auto block = static_cast<uint32_t>(lba / m_sectorsPerBlock);
        const auto first = static_cast<uint32_t>(lba % m_sectorsPerBlock);
        const uint32_t n = std::min(count, m_sectorsPerBlock - first);
        if (!ReadBlockSectors(block, first, n, lba, dst)) {
            return IoResult::ReadError;
        }
        lba += n;
        count -= n;
        dst += static_cast<size_t>(n) * kBytesPerSector;
    }
    return IoResult::Ok;
}

bool VhdDisk::AllocateBlock(uint32_t block, uint32_t first, uint32_t count, const uint8_t* data)
{
    // Append the block where the footer was, then move the footer behind it,
    // then publish the block in the BAT. The data area of the block that is
    // not written here reads back as zero (file extension).
    const uint64_t blockOffset = m_nextBlockOffset;
    std::vector<uint8_t> bitmap(m_bitmapSize, 0);
    SetBits(bitmap, first, count);
    const uint64_t blockEnd = blockOffset + m_bitmapSize + m_header.blockSize;
    const auto footer = m_footer.Serialize();
    uint8_t entry[4];
    WriteBE32(entry, static_cast<uint32_t>(blockOffset / kBytesPerSector));
    if (!m_file->WriteAt(blockOffset, bitmap.data(), bitmap.size()) ||
        !m_file->WriteAt(blockOffset + m_bitmapSize + static_cast<uint64_t>(first) * kBytesPerSector, data,
                         static_cast<size_t>(count) * kBytesPerSector) ||
        !m_file->WriteAt(blockEnd, footer.data(), footer.size()) ||
        !m_file->WriteAt(m_header.tableOffset + static_cast<uint64_t>(block) * 4, entry, sizeof(entry))) {
        return false;
    }
    m_bat[block] = static_cast<uint32_t>(blockOffset / kBytesPerSector);
    m_nextBlockOffset = blockEnd;
    if (m_bitmapCache.size() >= kBitmapCacheLimit) {
        m_bitmapCache.clear();
    }
    m_bitmapCache[block] = std::move(bitmap);
    return true;
}

bool VhdDisk::WriteBlockSectors(uint32_t block, uint32_t first, uint32_t count, const uint8_t* data)
{
    if (m_bat[block] == kUnusedEntry) {
        return AllocateBlock(block, first, count, data);
    }
    const uint64_t blockOffset = static_cast<uint64_t>(m_bat[block]) * kBytesPerSector;
    if (!m_file->WriteAt(blockOffset + m_bitmapSize + static_cast<uint64_t>(first) * kBytesPerSector, data,
                         static_cast<size_t>(count) * kBytesPerSector)) {
        return false;
    }
    // Mark the sectors as present after their data is in place.
    const std::vector<uint8_t>* cached = GetBitmap(block);
    if (!cached) {
        return false;
    }
    bool allSet = true;
    for (uint32_t s = first; s < first + count && allSet; ++s) {
        allSet = BitIsSet(*cached, s);
    }
    if (allSet) {
        return true;
    }
    std::vector<uint8_t>& bitmap = m_bitmapCache[block];
    SetBits(bitmap, first, count);
    return m_file->WriteAt(blockOffset, bitmap.data(), bitmap.size());
}

IoResult VhdDisk::WriteSectors(uint64_t lba, std::span<const uint8_t> data)
{
    if (IsReadOnly()) {
        return IoResult::WriteError;
    }
    if (data.size() % kBytesPerSector != 0 || !IsInRange(lba, data.size() / kBytesPerSector)) {
        return IoResult::OutOfRange;
    }
    if (m_footer.diskType == VhdFooter::DiskType::Fixed) {
        return m_file->WriteAt(lba * kBytesPerSector, data.data(), data.size()) ? IoResult::Ok : IoResult::WriteError;
    }
    const uint8_t* src = data.data();
    auto count = static_cast<uint32_t>(data.size() / kBytesPerSector);
    while (count > 0) {
        const auto block = static_cast<uint32_t>(lba / m_sectorsPerBlock);
        const auto first = static_cast<uint32_t>(lba % m_sectorsPerBlock);
        const uint32_t n = std::min(count, m_sectorsPerBlock - first);
        if (!WriteBlockSectors(block, first, n, src)) {
            return IoResult::WriteError;
        }
        lba += n;
        count -= n;
        src += static_cast<size_t>(n) * kBytesPerSector;
    }
    return IoResult::Ok;
}

IoResult VhdDisk::Flush() { return m_file->Flush() ? IoResult::Ok : IoResult::WriteError; }

// ---- Creation ------------------------------------------------------------

namespace {

DWORD ValidateNewSize(uint64_t size)
{
    return size == 0 || size % 512 != 0 || size > kMaxVhdSize ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
}

// Deletes the file unless Commit() was called.
class NewFileGuard {
public:
    explicit NewFileGuard(std::wstring path) : m_path(std::move(path)) {}
    ~NewFileGuard()
    {
        if (!m_committed) {
            DeleteFileW(m_path.c_str());
        }
    }
    void Commit() { m_committed = true; }

private:
    std::wstring m_path;
    bool m_committed = false;
};

} // namespace

DWORD CreateFixedVhd(const std::wstring& path, uint64_t size, const CreateProgress& progress)
{
    if (const DWORD error = ValidateNewSize(size)) {
        return error;
    }
    DWORD error = ERROR_SUCCESS;
    NewFileGuard guard(path);
    {
        auto file = File::CreateNew(path, error);
        if (!file) {
            guard.Commit(); // never delete a file we did not create
            return error;
        }
        // Write the zeroed data area ourselves so progress can be reported
        // and the operation cancelled.
        constexpr size_t kChunk = 4 * 1024 * 1024;
        const std::vector<uint8_t> zeros(kChunk, 0);
        for (uint64_t offset = 0; offset < size;) {
            const auto chunk = static_cast<size_t>(std::min<uint64_t>(kChunk, size - offset));
            if (!file->WriteAt(offset, zeros.data(), chunk)) {
                return GetLastError();
            }
            offset += chunk;
            if (progress && !progress(offset, size)) {
                return ERROR_CANCELLED;
            }
        }
        const auto footer = NewFooter(size, VhdFooter::DiskType::Fixed).Serialize();
        if (!file->WriteAt(size, footer.data(), footer.size()) || !file->Flush()) {
            return GetLastError();
        }
    }
    guard.Commit();
    return ERROR_SUCCESS;
}

DWORD CreateDynamicVhd(const std::wstring& path, uint64_t size)
{
    if (const DWORD error = ValidateNewSize(size)) {
        return error;
    }
    DWORD error = ERROR_SUCCESS;
    NewFileGuard guard(path);
    {
        auto file = File::CreateNew(path, error);
        if (!file) {
            guard.Commit();
            return error;
        }
        const VhdFooter footer = NewFooter(size, VhdFooter::DiskType::Dynamic);
        VhdDynamicHeader header;
        header.tableOffset = VhdFooter::kSize + VhdDynamicHeader::kSize;
        header.maxTableEntries = static_cast<uint32_t>((size + header.blockSize - 1) / header.blockSize);
        const size_t tableSize = (static_cast<size_t>(header.maxTableEntries) * 4 + 511) / 512 * 512;
        const std::vector<uint8_t> table(tableSize, 0xFF); // all entries unused

        const auto footerBytes = footer.Serialize();
        const auto headerBytes = header.Serialize();
        if (!file->WriteAt(0, footerBytes.data(), footerBytes.size()) ||
            !file->WriteAt(VhdFooter::kSize, headerBytes.data(), headerBytes.size()) ||
            !file->WriteAt(header.tableOffset, table.data(), table.size()) ||
            !file->WriteAt(header.tableOffset + tableSize, footerBytes.data(), footerBytes.size()) || !file->Flush()) {
            return GetLastError();
        }
    }
    guard.Commit();
    return ERROR_SUCCESS;
}

} // namespace tiscsi::disk
