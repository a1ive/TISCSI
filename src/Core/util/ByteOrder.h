// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Big-endian (network / SCSI byte order) field access on byte buffers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tiscsi::util {

inline uint16_t ReadBE16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

inline uint32_t ReadBE24(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 16 | p[1] << 8 | p[2]; }

inline uint32_t ReadBE32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

inline uint64_t ReadBE64(const uint8_t* p) { return static_cast<uint64_t>(ReadBE32(p)) << 32 | ReadBE32(p + 4); }

inline void WriteBE16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

inline void WriteBE24(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 16);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v);
}

inline void WriteBE32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline void WriteBE64(uint8_t* p, uint64_t v)
{
    WriteBE32(p, static_cast<uint32_t>(v >> 32));
    WriteBE32(p + 4, static_cast<uint32_t>(v));
}

// Writes `text` into a fixed-width field, padded with spaces (SCSI ASCII fields).
inline void WriteAsciiField(uint8_t* p, std::string_view text, size_t width)
{
    for (size_t i = 0; i < width; ++i) {
        p[i] = i < text.size() ? static_cast<uint8_t>(text[i]) : static_cast<uint8_t>(' ');
    }
}

} // namespace tiscsi::util
