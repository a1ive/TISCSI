# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 A1ive
"""Small [MS-VHDX] fixture writer and sector reader, independent of TISCSI.

Creates clean dynamic and differencing images, including partial blocks,
4K sectors and sparse virtual sizes. qemu-img supplies the fixed/dynamic
interoperability oracle; this helper supplies layouts qemu cannot create.
"""
import os
import struct
import uuid

MiB = 1 << 20
BAT = uuid.UUID("2dc27766-f623-4200-9d64-115e9bfd4a08").bytes_le
META = uuid.UUID("8b7ca206-4790-4b9a-b8fe-575f050f886e").bytes_le
PARAM = uuid.UUID("caa16737-fa36-4d43-b3b6-33f0aa44e76b").bytes_le
SIZE = uuid.UUID("2fa54224-cd1b-4876-b211-5dbed83bf4b8").bytes_le
DISK_ID = uuid.UUID("beca12ab-b2e6-4523-93ef-c309e000c746").bytes_le
SECTOR = uuid.UUID("8141bf1d-a96f-4709-ba47-f233a8faab5f").bytes_le
PHYSICAL = uuid.UUID("cda348c7-445d-4471-9cc9-e9885251c556").bytes_le
PARENT = uuid.UUID("a8d35f2d-b30b-454d-abf7-d3d84834ab0c").bytes_le
LOCATOR = uuid.UUID("b04aefb7-d19e-4a81-b789-25b8e9445913").bytes_le


def _crc_table():
    table = []
    for i in range(256):
        for _ in range(8):
            i = (i >> 1) ^ (0x82F63B78 if i & 1 else 0)
        table.append(i)
    return table


_CRC = _crc_table()


def checksum(data):
    c = 0xFFFFFFFF
    for b in data:
        c = _CRC[(c ^ b) & 255] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def seal(data):
    data = bytearray(data)
    struct.pack_into("<I", data, 4, 0)
    struct.pack_into("<I", data, 4, checksum(data))
    return data


def header(path):
    with open(path, "rb") as f:
        result = []
        for offset in (65536, 131072):
            f.seek(offset)
            h = f.read(4096)
            if h[:4] == b"head" and seal(h) == h:
                result.append(h)
        return max(result, key=lambda h: struct.unpack_from("<Q", h, 8)[0])


def metadata(path):
    with open(path, "rb") as f:
        f.seek(196608)
        rt = f.read(65536)
        regions = {}
        for i in range(struct.unpack_from("<I", rt, 8)[0]):
            p = 16 + 32 * i
            regions[rt[p:p+16]] = struct.unpack_from("<QI", rt, p + 16)
        meta = regions[META][0]
        f.seek(meta)
        table = f.read(65536)
        items = {}
        for i in range(struct.unpack_from("<H", table, 10)[0]):
            p = 32 + 32 * i
            offset, length = struct.unpack_from("<II", table, p + 16)
            f.seek(meta + offset)
            items[table[p:p+16]] = f.read(length)
        return regions, items


def create(path, size=16*MiB, sector=512, block=2*MiB, parent=None, writes=(), states=None, parent_id=None):
    """Writes are (byte offset, bytes); a child uses partial payload blocks.

    Explicit states supply whole-block ZERO/UNDEFINED/UNMAPPED fixtures.
    """
    states = states or {}
    payload_count = (size + block - 1) // block
    ratio = (1 << 23) * sector // block
    chunks = (payload_count + ratio - 1) // ratio
    entries = chunks * (ratio + 1) if parent else payload_count + (payload_count - 1) // ratio
    bat_length = ((entries * 8 + MiB - 1) // MiB) * MiB
    bat = bytearray(bat_length)
    items = [(PARAM, struct.pack("<II", block, 2 if parent else 0), 4),
             (SIZE, struct.pack("<Q", size), 6),
             (DISK_ID, uuid.uuid4().bytes_le, 6),
             (SECTOR, struct.pack("<I", sector), 6),
             (PHYSICAL, struct.pack("<I", 4096), 6)]
    if parent:
        identity = parent_id or header(parent)[32:48]
        fields = [("parent_linkage", "{" + str(uuid.UUID(bytes_le=identity)) + "}"),
                  ("relative_path", os.path.relpath(parent, os.path.dirname(path)))]
        locator = bytearray(20 + 12 * len(fields))
        locator[:16] = LOCATOR
        struct.pack_into("<H", locator, 18, len(fields))
        for i, (key, value) in enumerate(fields):
            key, value = key.encode("utf-16-le"), value.encode("utf-16-le")
            ko = len(locator)
            locator.extend(key)
            vo = len(locator)
            locator.extend(value)
            struct.pack_into("<IIHH", locator, 20 + 12 * i, ko, vo, len(key), len(value))
        items.append((PARENT, locator, 4))
    meta = bytearray(MiB)
    meta[:8] = b"metadata"
    struct.pack_into("<H", meta, 10, len(items))
    pos = 65536
    for i, (identifier, value, flags) in enumerate(items):
        p = 32 + 32 * i
        meta[p:p+16] = identifier
        struct.pack_into("<III", meta, p + 16, pos, len(value), flags)
        meta[pos:pos+len(value)] = value
        pos += len(value)
    regions = bytearray(65536)
    regions[:4] = b"regi"
    struct.pack_into("<I", regions, 8, 2)
    for i, (identifier, offset, length) in enumerate(((BAT, 2*MiB, bat_length), (META, 2*MiB+bat_length, MiB))):
        p = 16 + 32 * i
        regions[p:p+16] = identifier
        struct.pack_into("<QII", regions, p + 16, offset, length, 1)
    regions = seal(regions)
    blocks = {}
    bitmaps = {}
    for offset, value in writes:
        assert offset % sector == 0 and len(value) % sector == 0 and offset + len(value) <= size
        while value:
            number, first = divmod(offset, block)
            take = min(len(value), block - first)
            blocks.setdefault(number, bytearray(block))[first:first+take] = value[:take]
            if parent:
                bitmap = bitmaps.setdefault(number // ratio, bytearray(MiB))
                for s in range(offset // sector, (offset + take) // sector):
                    b = s % (1 << 23)
                    bitmap[b // 8] |= 1 << (b % 8)
            offset += take
            value = value[take:]
    with open(path, "wb") as f:
        f.write(b"vhdxfile")
        h = bytearray(4096)
        h[:4] = b"head"
        h[16:32] = uuid.uuid4().bytes_le
        h[32:48] = uuid.uuid4().bytes_le
        struct.pack_into("<HHIQ", h, 64, 0, 1, MiB, MiB)
        for seq in (1, 2):
            struct.pack_into("<Q", h, 8, seq)
            f.seek(seq * 65536)
            f.write(seal(h))
        for offset in (196608, 262144):
            f.seek(offset)
            f.write(regions)
        f.seek(2*MiB+bat_length)
        f.write(meta)
        for number, value in sorted(blocks.items()):
            offset = f.tell()
            struct.pack_into("<Q", bat, 8 * (number + number // ratio), offset | (7 if parent else 6))
            f.write(value)
        for chunk, bitmap in sorted(bitmaps.items()):
            offset = f.tell()
            struct.pack_into("<Q", bat, 8 * ((chunk + 1) * (ratio + 1) - 1), offset | 6)
            f.write(bitmap)
        for number, state in states.items():
            struct.pack_into("<Q", bat, 8 * (number + number // ratio), state)
        f.seek(2*MiB)
        f.write(bat)


def read(path, offset, length, parent=None):
    """Sector-at-a-time oracle, deliberately separate from production runs."""
    regions, items = metadata(path)
    block, flags = struct.unpack("<II", items[PARAM])
    sector, = struct.unpack("<I", items[SECTOR])
    ratio = (1 << 23) * sector // block
    result = bytearray()
    with open(path, "rb") as f:
        f.seek(regions[BAT][0])
        bat = f.read(regions[BAT][1])
        for current in range(offset, offset + length, sector):
            number, first = divmod(current, block)
            entry, = struct.unpack_from("<Q", bat, 8 * (number + number // ratio))
            state = entry & 7
            own = state == 6
            if state == 7:
                bitmap, = struct.unpack_from("<Q", bat, 8 * ((number // ratio + 1) * (ratio + 1) - 1))
                bit = current // sector % (1 << 23)
                f.seek((bitmap >> 20 << 20) + bit // 8)
                own = bool(f.read(1)[0] & (1 << (bit % 8)))
            if own:
                f.seek((entry >> 20 << 20) + first)
                result.extend(f.read(sector))
            elif flags & 2 and state in (0, 7):
                assert parent
                result.extend(read(parent, current, sector))
            else:
                result.extend(bytes(sector))
    return bytes(result)
