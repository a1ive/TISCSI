// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/disk/RawDiskImage.h"

#include <utility>

namespace tiscsi::disk {

std::unique_ptr<RawDiskImage> RawDiskImage::Open(const std::wstring& path, bool readOnly, OpenError& error,
                                                 uint32_t bytesPerSector)
{
    DWORD code = ERROR_SUCCESS;
    auto file = File::Open(path, readOnly ? File::Access::ReadShared : File::Access::ReadWriteExclusive, code);
    uint64_t size = 0;
    if (!file || !file->GetSize(size)) {
        error.Set(file ? GetLastError() : code, path);
        return nullptr;
    }
    // A trailing partial sector is not addressable.
    const uint64_t usable = size / bytesPerSector * bytesPerSector;
    if (usable == 0) {
        error.Set(ERROR_INVALID_DATA, path);
        return nullptr;
    }
    return std::unique_ptr<RawDiskImage>(new RawDiskImage(std::move(file), usable, bytesPerSector));
}

RawDiskImage::RawDiskImage(std::unique_ptr<File> file, uint64_t size, uint32_t bytesPerSector)
    : m_file(std::move(file)), m_size(size), m_bytesPerSector(bytesPerSector)
{
}

IoResult RawDiskImage::ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out)
{
    if (!IsInRange(lba, count)) {
        return IoResult::OutOfRange;
    }
    out.resize(static_cast<size_t>(count) * m_bytesPerSector);
    return m_file->ReadAt(lba * m_bytesPerSector, out.data(), out.size()) ? IoResult::Ok : IoResult::ReadError;
}

IoResult RawDiskImage::WriteSectors(uint64_t lba, std::span<const uint8_t> data)
{
    if (IsReadOnly()) {
        return IoResult::WriteError;
    }
    if (data.size() % m_bytesPerSector != 0 || !IsInRange(lba, data.size() / m_bytesPerSector)) {
        return IoResult::OutOfRange;
    }
    return m_file->WriteAt(lba * m_bytesPerSector, data.data(), data.size()) ? IoResult::Ok : IoResult::WriteError;
}

IoResult RawDiskImage::Flush() { return m_file->Flush() ? IoResult::Ok : IoResult::WriteError; }

} // namespace tiscsi::disk
