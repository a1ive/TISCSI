// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Optical disc image (.iso): a flat file of 2048-byte sectors, served as the
// disc in a read-only CD/DVD-ROM drive.

#pragma once

#include "Core/disk/RawDiskImage.h"

#include <memory>
#include <string>
#include <utility>

namespace tiscsi::disk {

class IsoImage final : public Disk {
public:
    static constexpr uint32_t kBytesPerSector = 2048;

    // Always opened read-only (shared reads): the medium cannot be written.
    static std::unique_ptr<IsoImage> Open(const std::wstring& path, OpenError& error)
    {
        auto image = RawDiskImage::Open(path, true, error, kBytesPerSector);
        return image ? std::unique_ptr<IsoImage>(new IsoImage(std::move(image))) : nullptr;
    }

    uint32_t BytesPerSector() const override { return kBytesPerSector; }
    uint64_t Size() const override { return m_image->Size(); }
    bool IsReadOnly() const override { return true; }
    bool IsOpticalDisc() const override { return true; }
    std::wstring Description() const override { return m_image->Description(); }

    IoResult ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out) override
    {
        return m_image->ReadSectors(lba, count, out);
    }
    IoResult WriteSectors(uint64_t, std::span<const uint8_t>) override { return IoResult::WriteError; }

private:
    explicit IsoImage(std::unique_ptr<RawDiskImage> image) : m_image(std::move(image)) {}

    std::unique_ptr<RawDiskImage> m_image;
};

} // namespace tiscsi::disk
