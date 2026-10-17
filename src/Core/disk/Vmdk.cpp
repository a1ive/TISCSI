// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/disk/Vmdk.h"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace tiscsi::disk {
namespace {

constexpr uint32_t kSparseMagic = 0x564D444B; // "KDMV"
constexpr uint32_t kFlagRedundantGrainTable = 1u << 1;
constexpr uint32_t kFlagCompressedGrains = 1u << 16;
constexpr uint64_t kGdAtEnd = UINT64_MAX;
constexpr size_t kMaxDescriptorSize = 1024 * 1024;
constexpr size_t kTableCacheLimit = 1024;

uint32_t ReadLE32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
uint64_t ReadLE64(const uint8_t* p) { return ReadLE32(p) | static_cast<uint64_t>(ReadLE32(p + 4)) << 32; }
uint16_t ReadLE16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

void WriteLE32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view Unquote(std::string_view s)
{
    s = Trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

template <typename T>
bool ParseNumber(std::string_view s, T& value, int base = 10)
{
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    return ec == std::errc() && end == s.data() + s.size();
}

std::wstring DecodeFileName(const std::string& name, const std::string& encoding)
{
    if (name.empty()) {
        return {};
    }
    // Descriptors are UTF-8 unless they say otherwise; fall back to the ANSI code page.
    const UINT codePage = encoding.empty() || _stricmp(encoding.c_str(), "UTF-8") == 0 ? CP_UTF8 : CP_ACP;
    const int length = MultiByteToWideChar(codePage, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(codePage, 0, name.data(), static_cast<int>(name.size()), result.data(), length);
    return result;
}

bool SamePath(const std::wstring& a, const std::wstring& b)
{
    return CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, a.c_str(), -1, b.c_str(), -1) == CSTR_EQUAL;
}

} // namespace

// ---- Descriptor ------------------------------------------------------------

std::optional<VmdkDescriptor> VmdkDescriptor::Parse(std::string_view text)
{
    VmdkDescriptor d;
    bool sawHeader = false;
    while (!text.empty()) {
        const size_t newline = text.find('\n');
        std::string_view line = Trim(text.substr(0, newline));
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        if (line.find("Disk DescriptorFile") != std::string_view::npos) {
            sawHeader = true;
        }
        if (line.empty() || line.front() == '#' || line.front() == '\0') {
            continue;
        }

        if (line.starts_with("RW ") || line.starts_with("RDONLY ") || line.starts_with("NOACCESS ")) {
            // <access> <sectors> <type> ["file name" [offset]]
            Extent extent;
            std::string_view rest = line;
            auto token = [&rest]() {
                rest = Trim(rest);
                std::string_view t;
                if (!rest.empty() && rest.front() == '"') {
                    const size_t close = rest.find('"', 1);
                    t = rest.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1);
                    rest.remove_prefix(close == std::string_view::npos ? rest.size() : close + 1);
                } else {
                    const size_t space = rest.find_first_of(" \t");
                    t = rest.substr(0, space);
                    rest.remove_prefix(space == std::string_view::npos ? rest.size() : space);
                }
                return t;
            };
            extent.access = std::string(token());
            if (!ParseNumber(token(), extent.sectors)) {
                return std::nullopt;
            }
            extent.type = std::string(token());
            extent.fileName = std::string(token());
            const std::string_view offset = token();
            if (!offset.empty() && !ParseNumber(offset, extent.offset)) {
                return std::nullopt;
            }
            d.extents.push_back(std::move(extent));
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = Trim(line.substr(0, equals));
        const std::string_view value = Unquote(line.substr(equals + 1));
        if (key == "CID") {
            ParseNumber(value, d.cid, 16);
            sawHeader = true;
        } else if (key == "parentCID") {
            ParseNumber(value, d.parentCid, 16);
        } else if (key == "createType") {
            d.createType = std::string(value);
            sawHeader = true;
        } else if (key == "parentFileNameHint") {
            d.parentFileNameHint = std::string(value);
        } else if (key == "encoding") {
            d.encoding = std::string(value);
        }
    }
    if (!sawHeader || d.extents.empty()) {
        return std::nullopt;
    }
    return d;
}

// ---- Hosted sparse extent --------------------------------------------------

bool VmdkSparseExtent::IsSparseHeader(const uint8_t* bytes) { return ReadLE32(bytes) == kSparseMagic; }

std::optional<VmdkSparseExtent::Header> VmdkSparseExtent::Header::Parse(const uint8_t* b)
{
    if (!IsSparseHeader(b)) {
        return std::nullopt;
    }
    Header h;
    h.version = ReadLE32(b + 4);
    h.flags = ReadLE32(b + 8);
    h.capacity = ReadLE64(b + 12);
    h.grainSize = ReadLE64(b + 20);
    h.descriptorOffset = ReadLE64(b + 28);
    h.descriptorSize = ReadLE64(b + 36);
    h.numGTEsPerGT = ReadLE32(b + 44);
    h.rgdOffset = ReadLE64(b + 48);
    h.gdOffset = ReadLE64(b + 56);
    h.overHead = ReadLE64(b + 64);
    h.compressAlgorithm = ReadLE16(b + 77);
    return h;
}

std::unique_ptr<VmdkSparseExtent> VmdkSparseExtent::Open(File& file, const Header& header, OpenError& error)
{
    if ((header.flags & kFlagCompressedGrains) || header.gdOffset == kGdAtEnd || header.version > 3) {
        error.Set(ERROR_NOT_SUPPORTED, file.Path()); // streamOptimized and newer formats
        return nullptr;
    }
    // grainSize must be a power of two of at least 8 sectors (4 KiB).
    if (header.grainSize < 8 || (header.grainSize & (header.grainSize - 1)) != 0 || header.numGTEsPerGT == 0 ||
        header.capacity == 0 || header.gdOffset == 0) {
        error.Set(ERROR_FILE_CORRUPT, file.Path());
        return nullptr;
    }
    std::unique_ptr<VmdkSparseExtent> extent(new VmdkSparseExtent(file, header));
    const uint64_t grains = (header.capacity + header.grainSize - 1) / header.grainSize;
    const uint64_t tables = (grains + header.numGTEsPerGT - 1) / header.numGTEsPerGT;
    if (tables > (1u << 26)) {
        error.Set(ERROR_FILE_CORRUPT, file.Path());
        return nullptr;
    }

    auto readDirectory = [&](uint64_t offset, std::vector<uint32_t>& directory) {
        std::vector<uint8_t> bytes(static_cast<size_t>(tables) * 4);
        if (!file.ReadAt(offset * 512, bytes.data(), bytes.size())) {
            return false;
        }
        directory.resize(static_cast<size_t>(tables));
        for (size_t i = 0; i < directory.size(); ++i) {
            directory[i] = ReadLE32(&bytes[i * 4]);
        }
        return true;
    };
    if (!readDirectory(header.gdOffset, extent->m_gd) ||
        ((header.flags & kFlagRedundantGrainTable) && header.rgdOffset != 0 &&
         !readDirectory(header.rgdOffset, extent->m_rgd))) {
        error.Set(ERROR_FILE_CORRUPT, file.Path());
        return nullptr;
    }

    uint64_t fileSize = 0;
    if (!file.GetSize(fileSize)) {
        error.Set(GetLastError(), file.Path());
        return nullptr;
    }
    extent->m_nextFreeSector = std::max((fileSize + 511) / 512, header.overHead);
    return extent;
}

bool VmdkSparseExtent::GetGrainTable(uint32_t gdIndex, std::vector<uint32_t>*& table)
{
    table = nullptr;
    if (gdIndex >= m_gd.size()) {
        return false;
    }
    if (m_gd[gdIndex] == 0) {
        return true; // not allocated
    }
    auto it = m_tableCache.find(gdIndex);
    if (it != m_tableCache.end()) {
        table = &it->second;
        return true;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(m_header.numGTEsPerGT) * 4);
    if (!m_file.ReadAt(static_cast<uint64_t>(m_gd[gdIndex]) * 512, bytes.data(), bytes.size())) {
        return false;
    }
    std::vector<uint32_t> entries(m_header.numGTEsPerGT);
    for (size_t i = 0; i < entries.size(); ++i) {
        entries[i] = ReadLE32(&bytes[i * 4]);
    }
    if (m_tableCache.size() >= kTableCacheLimit) {
        m_tableCache.clear();
    }
    table = &(m_tableCache[gdIndex] = std::move(entries));
    return true;
}

bool VmdkSparseExtent::LookupGrain(uint64_t grain, uint32_t& entry)
{
    std::vector<uint32_t>* table = nullptr;
    if (!GetGrainTable(static_cast<uint32_t>(grain / m_header.numGTEsPerGT), table)) {
        return false;
    }
    entry = table ? (*table)[grain % m_header.numGTEsPerGT] : 0;
    return true;
}

bool VmdkSparseExtent::Read(uint64_t sector, uint32_t count, uint8_t* out, const FillFn& fill)
{
    while (count > 0) {
        const uint64_t grain = sector / m_header.grainSize;
        const auto within = static_cast<uint32_t>(sector % m_header.grainSize);
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(count, m_header.grainSize - within));
        uint32_t entry = 0;
        if (!LookupGrain(grain, entry)) {
            return false;
        }
        bool ok;
        if (entry == 0) {
            ok = fill(sector, n, out); // not allocated: parent or zeros
        } else if (entry == 1) {
            std::memset(out, 0, static_cast<size_t>(n) * 512); // zeroed grain
            ok = true;
        } else {
            ok = m_file.ReadAt((static_cast<uint64_t>(entry) + within) * 512, out, static_cast<size_t>(n) * 512);
        }
        if (!ok) {
            return false;
        }
        sector += n;
        count -= n;
        out += static_cast<size_t>(n) * 512;
    }
    return true;
}

bool VmdkSparseExtent::AllocateGrainTable(uint32_t gdIndex)
{
    // Grain tables of VMware-created extents are preallocated; this handles
    // the rest. Tables are appended (zeroed) and then published in the
    // directory, and in the redundant directory if there is one.
    const uint64_t tableSectors = (static_cast<uint64_t>(m_header.numGTEsPerGT) * 4 + 511) / 512;
    const std::vector<uint8_t> zeros(static_cast<size_t>(tableSectors) * 512, 0);
    auto allocate = [&](std::vector<uint32_t>& directory, uint64_t directoryOffset) {
        const uint64_t tableSector = m_nextFreeSector;
        uint8_t entry[4];
        WriteLE32(entry, static_cast<uint32_t>(tableSector));
        if (!m_file.WriteAt(tableSector * 512, zeros.data(), zeros.size()) ||
            !m_file.WriteAt(directoryOffset * 512 + static_cast<uint64_t>(gdIndex) * 4, entry, sizeof(entry))) {
            return false;
        }
        directory[gdIndex] = static_cast<uint32_t>(tableSector);
        m_nextFreeSector += tableSectors;
        return true;
    };
    if (!m_rgd.empty() && m_rgd[gdIndex] == 0 && !allocate(m_rgd, m_header.rgdOffset)) {
        return false;
    }
    return allocate(m_gd, m_header.gdOffset);
}

bool VmdkSparseExtent::Write(uint64_t sector, uint32_t count, const uint8_t* data, const FillFn& fill)
{
    const size_t grainBytes = static_cast<size_t>(m_header.grainSize) * 512;
    while (count > 0) {
        const uint64_t grain = sector / m_header.grainSize;
        const auto within = static_cast<uint32_t>(sector % m_header.grainSize);
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(count, m_header.grainSize - within));
        uint32_t entry = 0;
        if (!LookupGrain(grain, entry)) {
            return false;
        }
        if (entry >= 2) {
            if (!m_file.WriteAt((static_cast<uint64_t>(entry) + within) * 512, data, static_cast<size_t>(n) * 512)) {
                return false;
            }
        } else {
            // Allocate a new grain at the end of the file: the previous
            // content (parent data or zeros) overlaid with the new sectors.
            const auto gdIndex = static_cast<uint32_t>(grain / m_header.numGTEsPerGT);
            const auto gtIndex = static_cast<uint32_t>(grain % m_header.numGTEsPerGT);
            if (m_gd[gdIndex] == 0 && !AllocateGrainTable(gdIndex)) {
                return false;
            }
            std::vector<uint8_t> buffer(grainBytes, 0);
            const uint64_t grainStart = grain * m_header.grainSize;
            if (entry == 0 && !fill(grainStart, static_cast<uint32_t>(m_header.grainSize), buffer.data())) {
                return false;
            }
            std::memcpy(buffer.data() + static_cast<size_t>(within) * 512, data, static_cast<size_t>(n) * 512);
            const uint64_t grainSector = m_nextFreeSector;
            uint8_t newEntry[4];
            WriteLE32(newEntry, static_cast<uint32_t>(grainSector));
            if (!m_file.WriteAt(grainSector * 512, buffer.data(), buffer.size()) ||
                !m_file.WriteAt(static_cast<uint64_t>(m_gd[gdIndex]) * 512 + gtIndex * 4ull, newEntry, sizeof(newEntry)) ||
                (!m_rgd.empty() &&
                 !m_file.WriteAt(static_cast<uint64_t>(m_rgd[gdIndex]) * 512 + gtIndex * 4ull, newEntry, sizeof(newEntry)))) {
                return false;
            }
            m_nextFreeSector += m_header.grainSize;
            std::vector<uint32_t>* table = nullptr;
            if (GetGrainTable(gdIndex, table) && table) {
                (*table)[gtIndex] = static_cast<uint32_t>(grainSector);
            }
        }
        sector += n;
        count -= n;
        data += static_cast<size_t>(n) * 512;
    }
    return true;
}

// ---- Disk ------------------------------------------------------------------

std::unique_ptr<VmdkDisk> VmdkDisk::Open(const std::wstring& path, bool readOnly, OpenError& error, int depth)
{
    DWORD code = ERROR_SUCCESS;
    auto file = File::Open(path, readOnly ? File::Access::ReadShared : File::Access::ReadWriteExclusive, code);
    uint64_t fileSize = 0;
    if (!file || !file->GetSize(fileSize)) {
        error.Set(file ? GetLastError() : code, path);
        return nullptr;
    }

    std::optional<VmdkSparseExtent::Header> sparseHeader;
    std::optional<VmdkDescriptor> descriptor;
    uint8_t first[512] = {};
    if (fileSize >= sizeof(first) && file->ReadAt(0, first, sizeof(first)) && VmdkSparseExtent::IsSparseHeader(first)) {
        sparseHeader = VmdkSparseExtent::Header::Parse(first);
        if (sparseHeader->descriptorOffset != 0 && sparseHeader->descriptorSize != 0 &&
            sparseHeader->descriptorSize * 512 <= kMaxDescriptorSize) {
            // monolithicSparse: descriptor embedded in the extent
            std::string text(static_cast<size_t>(sparseHeader->descriptorSize) * 512, '\0');
            if (file->ReadAt(sparseHeader->descriptorOffset * 512, text.data(), text.size())) {
                descriptor = VmdkDescriptor::Parse(text.substr(0, text.find('\0')));
            }
        } else {
            // A bare sparse extent without descriptor: one extent, this file.
            VmdkDescriptor d;
            d.extents.push_back({"RW", sparseHeader->capacity, "SPARSE", "", 0});
            descriptor = std::move(d);
        }
    } else if (fileSize > 0 && fileSize <= kMaxDescriptorSize) {
        std::string text(static_cast<size_t>(fileSize), '\0');
        if (file->ReadAt(0, text.data(), text.size())) {
            descriptor = VmdkDescriptor::Parse(text);
        }
    }
    if (!descriptor) {
        error.Set(ERROR_FILE_CORRUPT, path);
        return nullptr;
    }

    std::unique_ptr<VmdkDisk> disk(new VmdkDisk());
    disk->m_path = path;
    disk->m_cid = descriptor->cid;
    if (!disk->OpenExtents(*descriptor, std::move(file), sparseHeader, readOnly, error) ||
        !disk->OpenParent(*descriptor, error, depth)) {
        return nullptr;
    }
    return disk;
}

bool VmdkDisk::OpenExtents(const VmdkDescriptor& descriptor, std::unique_ptr<File> mainFile,
                           std::optional<VmdkSparseExtent::Header> mainHeader, bool readOnly, OpenError& error)
{
    const std::wstring directory = DirectoryOf(FullPath(m_path));
    const std::wstring mainFullPath = FullPath(m_path);
    File* main = mainFile.get();
    m_files.push_back(std::move(mainFile));
    m_readOnly = readOnly;

    for (const VmdkDescriptor::Extent& line : descriptor.extents) {
        Extent extent;
        extent.start = m_totalSectors;
        extent.sectors = line.sectors;
        extent.writable = !readOnly && line.access == "RW";
        if (line.access != "RW") {
            m_readOnly = true;
        }
        if (line.access == "NOACCESS") {
            error.Set(ERROR_NOT_SUPPORTED, m_path);
            return false;
        }

        if (line.type == "ZERO") {
            extent.type = ExtentType::Zero;
            extent.writable = false;
        } else if (line.type == "FLAT" || line.type == "VMFS" || line.type == "SPARSE") {
            // Open the extent file, reusing the descriptor's own handle when
            // the extent is the same file (monolithicSparse).
            File* file = main;
            std::wstring extentPath = m_path;
            // An embedded descriptor describes its own file, even if renamed.
            const bool embedded = mainHeader && descriptor.extents.size() == 1 && line.type == "SPARSE";
            if (!line.fileName.empty() && !embedded) {
                extentPath = ResolveRelative(directory, DecodeFileName(line.fileName, descriptor.encoding));
                if (!SamePath(FullPath(extentPath), mainFullPath)) {
                    DWORD code = ERROR_SUCCESS;
                    auto opened = File::Open(
                        extentPath, extent.writable ? File::Access::ReadWriteExclusive : File::Access::ReadShared, code);
                    if (!opened) {
                        error.Set(code, extentPath);
                        return false;
                    }
                    file = opened.get();
                    m_files.push_back(std::move(opened));
                }
            }
            if (extent.writable && !file->IsWritable()) {
                extent.writable = false;
                m_readOnly = true;
            }
            extent.file = file;
            if (line.type == "SPARSE") {
                extent.type = ExtentType::Sparse;
                std::optional<VmdkSparseExtent::Header> header;
                if (file == main && mainHeader) {
                    header = mainHeader;
                } else {
                    uint8_t bytes[512];
                    if (file->ReadAt(0, bytes, sizeof(bytes))) {
                        header = VmdkSparseExtent::Header::Parse(bytes);
                    }
                }
                if (!header) {
                    error.Set(ERROR_FILE_CORRUPT, extentPath);
                    return false;
                }
                extent.sparse = VmdkSparseExtent::Open(*file, *header, error);
                if (!extent.sparse) {
                    return false;
                }
            } else {
                extent.type = ExtentType::Flat;
                extent.flatOffset = line.offset;
            }
        } else {
            error.Set(ERROR_NOT_SUPPORTED, m_path); // VMFSSPARSE, SESPARSE, raw device mappings, ...
            return false;
        }
        m_totalSectors += extent.sectors;
        m_extents.push_back(std::move(extent));
    }
    if (m_totalSectors == 0) {
        error.Set(ERROR_FILE_CORRUPT, m_path);
        return false;
    }
    return true;
}

bool VmdkDisk::OpenParent(const VmdkDescriptor& descriptor, OpenError& error, int depth)
{
    if (descriptor.parentCid == 0xFFFFFFFF || descriptor.parentFileNameHint.empty()) {
        return true;
    }
    if (depth >= detail::kMaxParentDepth) {
        error.Set(ERROR_FILE_CORRUPT, m_path);
        return false;
    }
    const std::wstring parentPath = ResolveRelative(DirectoryOf(FullPath(m_path)),
                                                    DecodeFileName(descriptor.parentFileNameHint, descriptor.encoding));
    m_parent = detail::OpenDiskImage(parentPath, true, error, depth + 1);
    if (!m_parent) {
        return false;
    }
    // The child records the parent's content ID; a mismatch means the parent changed.
    const auto* parentVmdk = dynamic_cast<const VmdkDisk*>(m_parent.get());
    if (parentVmdk && parentVmdk->ContentId() != descriptor.parentCid) {
        error.Set(ERROR_FILE_CORRUPT, parentPath);
        return false;
    }
    return true;
}

VmdkSparseExtent::FillFn VmdkDisk::MakeFill(uint64_t extentStart)
{
    return [this, extentStart](uint64_t sector, uint32_t count, uint8_t* out) {
        std::memset(out, 0, static_cast<size_t>(count) * kBytesPerSector);
        if (!m_parent) {
            return true;
        }
        // The last grain may extend past the end of the parent.
        const uint64_t lba = extentStart + sector;
        const uint64_t parentSectors = m_parent->TotalSectors();
        if (lba >= parentSectors) {
            return true;
        }
        const auto n = static_cast<uint32_t>(std::min<uint64_t>(count, parentSectors - lba));
        std::vector<uint8_t> buffer;
        if (m_parent->ReadSectors(lba, n, buffer) != IoResult::Ok) {
            return false;
        }
        std::memcpy(out, buffer.data(), buffer.size());
        return true;
    };
}

IoResult VmdkDisk::ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out)
{
    if (!IsInRange(lba, count)) {
        return IoResult::OutOfRange;
    }
    out.resize(static_cast<size_t>(count) * kBytesPerSector);
    uint8_t* dst = out.data();
    for (Extent& extent : m_extents) {
        if (count == 0) {
            break;
        }
        if (lba >= extent.start + extent.sectors) {
            continue;
        }
        const uint64_t offset = lba - extent.start;
        const auto n = static_cast<uint32_t>(std::min<uint64_t>(count, extent.sectors - offset));
        const size_t bytes = static_cast<size_t>(n) * kBytesPerSector;
        bool ok = true;
        switch (extent.type) {
        case ExtentType::Zero:
            std::memset(dst, 0, bytes);
            break;
        case ExtentType::Flat:
            ok = extent.file->ReadAt((extent.flatOffset + offset) * kBytesPerSector, dst, bytes);
            break;
        case ExtentType::Sparse:
            ok = extent.sparse->Read(offset, n, dst, MakeFill(extent.start));
            break;
        }
        if (!ok) {
            return IoResult::ReadError;
        }
        lba += n;
        count -= n;
        dst += bytes;
    }
    return IoResult::Ok;
}

IoResult VmdkDisk::WriteSectors(uint64_t lba, std::span<const uint8_t> data)
{
    if (m_readOnly) {
        return IoResult::WriteError;
    }
    if (data.size() % kBytesPerSector != 0 || !IsInRange(lba, data.size() / kBytesPerSector)) {
        return IoResult::OutOfRange;
    }
    const uint8_t* src = data.data();
    auto count = static_cast<uint32_t>(data.size() / kBytesPerSector);
    for (Extent& extent : m_extents) {
        if (count == 0) {
            break;
        }
        if (lba >= extent.start + extent.sectors) {
            continue;
        }
        const uint64_t offset = lba - extent.start;
        const auto n = static_cast<uint32_t>(std::min<uint64_t>(count, extent.sectors - offset));
        const size_t bytes = static_cast<size_t>(n) * kBytesPerSector;
        bool ok = extent.writable;
        if (ok && extent.type == ExtentType::Flat) {
            ok = extent.file->WriteAt((extent.flatOffset + offset) * kBytesPerSector, src, bytes);
        } else if (ok && extent.type == ExtentType::Sparse) {
            ok = extent.sparse->Write(offset, n, src, MakeFill(extent.start));
        } else {
            ok = false;
        }
        if (!ok) {
            return IoResult::WriteError;
        }
        lba += n;
        count -= n;
        src += bytes;
    }
    return IoResult::Ok;
}

IoResult VmdkDisk::Flush()
{
    bool ok = true;
    for (const auto& file : m_files) {
        ok = file->Flush() && ok;
    }
    return ok ? IoResult::Ok : IoResult::WriteError;
}

} // namespace tiscsi::disk
