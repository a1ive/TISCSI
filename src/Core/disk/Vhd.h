// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Microsoft Virtual Hard Disk (VHD) images: fixed, dynamic and differencing,
// as described in the "Virtual Hard Disk Image Format Specification" v1.0.
// All on-disk integers are big-endian.

#pragma once

#include "Core/disk/DiskImage.h"
#include "Core/disk/File.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tiscsi::disk {

using Guid16 = std::array<uint8_t, 16>;

struct VhdFooter {
    static constexpr size_t kSize = 512;
    enum class DiskType : uint32_t { Fixed = 2, Dynamic = 3, Differencing = 4 };

    uint32_t features = 2; // reserved bit, always set
    uint32_t fileFormatVersion = 0x00010000;
    uint64_t dataOffset = UINT64_MAX; // dynamic header offset; all ones for fixed disks
    uint32_t timeStamp = 0;           // seconds since 2000-01-01 00:00:00 UTC
    uint32_t creatorApplication = 0;
    uint32_t creatorVersion = 0;
    uint32_t creatorHostOs = 0;
    uint64_t originalSize = 0;
    uint64_t currentSize = 0;
    uint16_t cylinders = 0;
    uint8_t heads = 0;
    uint8_t sectorsPerTrack = 0;
    DiskType diskType = DiskType::Fixed;
    Guid16 uniqueId{};
    uint8_t savedState = 0;

    // Validates the cookie and checksum.
    static std::optional<VhdFooter> Parse(const uint8_t* bytes);
    std::array<uint8_t, kSize> Serialize() const;
};

struct VhdDynamicHeader {
    static constexpr size_t kSize = 1024;
    static constexpr uint32_t kDefaultBlockSize = 2 * 1024 * 1024;

    struct ParentLocator {
        uint32_t platformCode = 0;
        uint32_t platformDataSpace = 0;
        uint32_t platformDataLength = 0;
        uint64_t platformDataOffset = 0;
    };

    uint64_t tableOffset = 0;
    uint32_t headerVersion = 0x00010000;
    uint32_t maxTableEntries = 0;
    uint32_t blockSize = kDefaultBlockSize;
    Guid16 parentUniqueId{};
    uint32_t parentTimeStamp = 0;
    std::wstring parentUnicodeName;
    std::array<ParentLocator, 8> parentLocators{};

    static std::optional<VhdDynamicHeader> Parse(const uint8_t* bytes);
    std::array<uint8_t, kSize> Serialize() const;
};

class VhdDisk final : public Disk {
public:
    static constexpr uint32_t kBytesPerSector = 512;

    static std::unique_ptr<VhdDisk> Open(const std::wstring& path, bool readOnly, OpenError& error, int depth = 0);

    uint32_t BytesPerSector() const override { return kBytesPerSector; }
    uint64_t Size() const override { return m_footer.currentSize; }
    bool IsReadOnly() const override { return !m_file->IsWritable(); }
    std::wstring Description() const override { return m_file->Path(); }

    IoResult ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out) override;
    IoResult WriteSectors(uint64_t lba, std::span<const uint8_t> data) override;
    IoResult Flush() override;

    VhdFooter::DiskType Type() const { return m_footer.diskType; }
    const Guid16& UniqueId() const { return m_footer.uniqueId; }

private:
    VhdDisk(std::unique_ptr<File> file, const VhdFooter& footer);

    bool LoadDynamic(OpenError& error, uint64_t fileSize, bool trailingFooterValid, int depth);
    bool OpenParent(OpenError& error, int depth);

    bool ReadParent(uint64_t lba, uint32_t count, uint8_t* out);
    bool ReadBlockSectors(uint32_t block, uint32_t first, uint32_t count, uint64_t lba, uint8_t* out);
    bool WriteBlockSectors(uint32_t block, uint32_t first, uint32_t count, const uint8_t* data);
    bool AllocateBlock(uint32_t block, uint32_t first, uint32_t count, const uint8_t* data);
    const std::vector<uint8_t>* GetBitmap(uint32_t block);

    std::unique_ptr<File> m_file;
    VhdFooter m_footer;

    // Dynamic and differencing disks
    VhdDynamicHeader m_header;
    std::vector<uint32_t> m_bat; // block sector offsets, kUnusedEntry if not allocated
    uint32_t m_sectorsPerBlock = 0;
    uint32_t m_bitmapSize = 0;     // bytes, sector aligned
    uint64_t m_nextBlockOffset = 0; // where the trailing footer is and new blocks go
    std::unique_ptr<Disk> m_parent;
    std::unordered_map<uint32_t, std::vector<uint8_t>> m_bitmapCache;
};

constexpr uint64_t kMaxVhdSize = 2040ull << 30; // specification limit

// Reports bytes written so far; return false to cancel.
using CreateProgress = std::function<bool(uint64_t done, uint64_t total)>;

// Create a new image (fails if the file exists). `size` must be a non-zero
// multiple of 512 not above kMaxVhdSize. Returns ERROR_SUCCESS or a Win32
// error (ERROR_CANCELLED if cancelled); a partially created file is deleted.
DWORD CreateFixedVhd(const std::wstring& path, uint64_t size, const CreateProgress& progress = {});
DWORD CreateDynamicVhd(const std::wstring& path, uint64_t size);

} // namespace tiscsi::disk
