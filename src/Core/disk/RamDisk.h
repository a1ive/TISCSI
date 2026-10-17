// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Volatile disk backed by process memory.

#pragma once

#include "Core/disk/Disk.h"

#include <memory>

namespace tiscsi::disk {

class RamDisk final : public Disk {
public:
    static constexpr uint32_t kBytesPerSector = 512;

    // `size` is rounded down to whole sectors. Returns nullptr if the memory
    // cannot be allocated.
    static std::unique_ptr<RamDisk> Create(uint64_t size);

    ~RamDisk() override;

    uint32_t BytesPerSector() const override { return kBytesPerSector; }
    uint64_t Size() const override { return m_size; }
    bool IsReadOnly() const override { return false; }
    std::wstring Description() const override;

    IoResult ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out) override;
    IoResult WriteSectors(uint64_t lba, std::span<const uint8_t> data) override;

private:
    RamDisk(uint8_t* memory, uint64_t size) : m_memory(memory), m_size(size) {}

    uint8_t* m_memory;
    uint64_t m_size;
};

} // namespace tiscsi::disk
