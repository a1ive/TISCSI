// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Raw (flat, .img) disk image file.

#pragma once

#include "Core/disk/DiskImage.h"
#include "Core/disk/File.h"

#include <memory>
#include <string>

namespace tiscsi::disk {

class RawDiskImage final : public Disk {
public:
    static constexpr uint32_t kDefaultBytesPerSector = 512;

    // Read-write images are opened exclusively (no other process may open the
    // file), read-only images allow shared reads.
    static std::unique_ptr<RawDiskImage> Open(const std::wstring& path, bool readOnly, OpenError& error,
                                              uint32_t bytesPerSector = kDefaultBytesPerSector);

    uint32_t BytesPerSector() const override { return m_bytesPerSector; }
    uint64_t Size() const override { return m_size; }
    bool IsReadOnly() const override { return !m_file->IsWritable(); }
    std::wstring Description() const override { return m_file->Path(); }

    IoResult ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out) override;
    IoResult WriteSectors(uint64_t lba, std::span<const uint8_t> data) override;
    IoResult Flush() override;

private:
    RawDiskImage(std::unique_ptr<File> file, uint64_t size, uint32_t bytesPerSector);

    std::unique_ptr<File> m_file;
    uint64_t m_size;
    uint32_t m_bytesPerSector;
};

} // namespace tiscsi::disk
