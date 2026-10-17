// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// VMware Virtual Disk (VMDK) images, per the "Virtual Disk Format 5.0"
// specification: a text descriptor listing extents of type FLAT / VMFS
// (raw data), SPARSE (hosted sparse extent, little-endian header with grain
// directory and grain tables) and ZERO. This covers monolithicSparse,
// monolithicFlat, twoGbMaxExtentSparse, twoGbMaxExtentFlat and vmfs disks,
// including delta (child) disks. Compressed streamOptimized images and ESX
// sparse formats (VMFSSPARSE, SESPARSE) are not supported.

#pragma once

#include "Core/disk/DiskImage.h"
#include "Core/disk/File.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tiscsi::disk {

struct VmdkDescriptor {
    struct Extent {
        std::string access; // RW, RDONLY, NOACCESS
        uint64_t sectors = 0;
        std::string type; // FLAT, SPARSE, ZERO, VMFS, ...
        std::string fileName;
        uint64_t offset = 0; // FLAT / VMFS: start sector within the file
    };

    uint32_t cid = 0xFFFFFFFF;
    uint32_t parentCid = 0xFFFFFFFF; // 0xFFFFFFFF: no parent
    std::string createType;
    std::string parentFileNameHint;
    std::string encoding;
    std::vector<Extent> extents;

    static std::optional<VmdkDescriptor> Parse(std::string_view text);
};

// Hosted sparse extent (magic "KDMV").
class VmdkSparseExtent {
public:
    // Supplies data for grains that are not allocated in this extent (the
    // parent disk's data, or zeros): fill(extentSector, count, out).
    using FillFn = std::function<bool(uint64_t sector, uint32_t count, uint8_t* out)>;

    struct Header {
        uint32_t version = 0;
        uint32_t flags = 0;
        uint64_t capacity = 0; // sectors
        uint64_t grainSize = 0; // sectors
        uint64_t descriptorOffset = 0;
        uint64_t descriptorSize = 0;
        uint32_t numGTEsPerGT = 0;
        uint64_t rgdOffset = 0;
        uint64_t gdOffset = 0;
        uint64_t overHead = 0;
        uint16_t compressAlgorithm = 0;

        static std::optional<Header> Parse(const uint8_t* bytes);
    };

    static bool IsSparseHeader(const uint8_t* bytes);
    // `file` must outlive the extent.
    static std::unique_ptr<VmdkSparseExtent> Open(File& file, const Header& header, OpenError& error);

    bool Read(uint64_t sector, uint32_t count, uint8_t* out, const FillFn& fill);
    bool Write(uint64_t sector, uint32_t count, const uint8_t* data, const FillFn& fill);

private:
    VmdkSparseExtent(File& file, const Header& header) : m_file(file), m_header(header) {}

    // Grain table for a grain directory index, nullptr if not allocated.
    bool GetGrainTable(uint32_t gdIndex, std::vector<uint32_t>*& table);
    bool AllocateGrainTable(uint32_t gdIndex);
    bool LookupGrain(uint64_t grain, uint32_t& entry);

    File& m_file;
    Header m_header;
    std::vector<uint32_t> m_gd;
    std::vector<uint32_t> m_rgd; // empty if there is no redundant directory
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_tableCache;
    uint64_t m_nextFreeSector = 0;
};

class VmdkDisk final : public Disk {
public:
    static constexpr uint32_t kBytesPerSector = 512;

    static std::unique_ptr<VmdkDisk> Open(const std::wstring& path, bool readOnly, OpenError& error, int depth = 0);

    uint32_t BytesPerSector() const override { return kBytesPerSector; }
    uint64_t Size() const override { return m_totalSectors * kBytesPerSector; }
    bool IsReadOnly() const override { return m_readOnly; }
    std::wstring Description() const override { return m_path; }

    IoResult ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out) override;
    IoResult WriteSectors(uint64_t lba, std::span<const uint8_t> data) override;
    IoResult Flush() override;

    uint32_t ContentId() const { return m_cid; }

private:
    enum class ExtentType { Flat, Sparse, Zero };
    struct Extent {
        ExtentType type = ExtentType::Zero;
        uint64_t start = 0; // first disk sector
        uint64_t sectors = 0;
        bool writable = false;
        File* file = nullptr; // owned by m_files
        uint64_t flatOffset = 0;
        std::unique_ptr<VmdkSparseExtent> sparse;
    };

    VmdkDisk() = default;
    bool OpenExtents(const VmdkDescriptor& descriptor, std::unique_ptr<File> mainFile,
                     std::optional<VmdkSparseExtent::Header> mainHeader, bool readOnly, OpenError& error);
    bool OpenParent(const VmdkDescriptor& descriptor, OpenError& error, int depth);
    VmdkSparseExtent::FillFn MakeFill(uint64_t extentStart);

    std::wstring m_path;
    std::vector<std::unique_ptr<File>> m_files;
    std::vector<Extent> m_extents;
    std::unique_ptr<Disk> m_parent;
    uint64_t m_totalSectors = 0;
    bool m_readOnly = true;
    uint32_t m_cid = 0xFFFFFFFF;
};

} // namespace tiscsi::disk
