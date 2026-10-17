// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/disk/RamDisk.h"

#include <windows.h>

#include <cstring>
#include <format>

namespace tiscsi::disk {

std::unique_ptr<RamDisk> RamDisk::Create(uint64_t size)
{
    size = size / kBytesPerSector * kBytesPerSector;
    if (size == 0 || size > SIZE_MAX) {
        return nullptr;
    }
    // VirtualAlloc returns zeroed pages and fails cleanly when out of memory.
    void* memory = VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory) {
        return nullptr;
    }
    return std::unique_ptr<RamDisk>(new RamDisk(static_cast<uint8_t*>(memory), size));
}

RamDisk::~RamDisk() { VirtualFree(m_memory, 0, MEM_RELEASE); }

std::wstring RamDisk::Description() const { return std::format(L"RAM disk ({} MiB)", m_size >> 20); }

IoResult RamDisk::ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out)
{
    if (!IsInRange(lba, count)) {
        return IoResult::OutOfRange;
    }
    const size_t length = static_cast<size_t>(count) * kBytesPerSector;
    out.assign(m_memory + lba * kBytesPerSector, m_memory + lba * kBytesPerSector + length);
    return IoResult::Ok;
}

IoResult RamDisk::WriteSectors(uint64_t lba, std::span<const uint8_t> data)
{
    if (data.size() % kBytesPerSector != 0 || !IsInRange(lba, data.size() / kBytesPerSector)) {
        return IoResult::OutOfRange;
    }
    std::memcpy(m_memory + lba * kBytesPerSector, data.data(), data.size());
    return IoResult::Ok;
}

} // namespace tiscsi::disk
