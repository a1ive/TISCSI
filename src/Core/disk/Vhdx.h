// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Microsoft [MS-VHDX] v8.0: fixed, dynamic and differencing images.

#pragma once

#include "Core/disk/DiskImage.h"
#include "Core/disk/File.h"

#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace tiscsi::disk {

class VhdxDisk final : public Disk {
public:
    static std::unique_ptr<VhdxDisk> Open(const std::wstring& path, bool readOnly, OpenError& error, int depth = 0);

    uint32_t BytesPerSector() const override { return m_sectorSize; }
    uint64_t Size() const override { return m_size; }
    bool IsReadOnly() const override { return !m_file->IsWritable(); }
    std::wstring Description() const override { return m_file->Path(); }

    IoResult ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out) override;
    IoResult WriteSectors(uint64_t lba, std::span<const uint8_t> data) override;
    IoResult Flush() override;

private:
    explicit VhdxDisk(std::unique_ptr<File> file) : m_file(std::move(file)) {}
    bool Load(OpenError& error, int depth);
    bool OpenParent(const std::vector<uint8_t>& locator, OpenError& error, int depth);
    bool Read(uint64_t offset, size_t length, uint8_t* out);
    bool UpdateHeaders();
    bool BeginWrite();
    bool CommitBat(size_t index, uint64_t entry);
    bool Allocate(uint64_t block, uint32_t first, std::span<const uint8_t> data);
    size_t BatIndex(uint64_t block) const;

    std::unique_ptr<File> m_file;
    std::unique_ptr<VhdxDisk> m_parent;
    std::array<uint8_t, 4096> m_header{};
    uint64_t m_headerOffset = 0;
    uint64_t m_size = 0;
    uint64_t m_fileSize = 0;
    uint64_t m_batOffset = 0;
    uint32_t m_sectorSize = 0;
    uint32_t m_blockSize = 0;
    uint32_t m_chunkRatio = 0;
    std::vector<uint64_t> m_bat;
    std::vector<uint8_t> m_bitmap;
    uint64_t m_bitmapChunk = UINT64_MAX;
    bool m_writeStarted = false;
    bool m_failed = false; // A failed metadata write requires closing and recovery.
    // Fresh log storage on the first allocation of each writable session.
    // A checkpoint is made durable before publishing its location in the header.
    uint64_t m_logOffset = 0;
    uint64_t m_logSequence = 0;
    std::array<uint8_t, 16> m_logGuid{};
};

} // namespace tiscsi::disk
