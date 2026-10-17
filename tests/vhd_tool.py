# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 A1ive
"""Independent VHD helpers for tests, written from the VHD specification.

Used where no third-party tool is available without administrator rights:
creating differencing disks and reading a differencing chain back.
"""
import os
import struct
import uuid

SECTOR = 512
UNUSED = 0xFFFFFFFF


def _checksum(data, offset):
    return ~sum(b for i, b in enumerate(data) if not offset <= i < offset + 4) & 0xFFFFFFFF


def read_footer(path):
    with open(path, "rb") as f:
        f.seek(-512, os.SEEK_END)
        b = f.read(512)
    assert b[:8] == b"conectix", "missing VHD footer"
    assert struct.unpack(">I", b[64:68])[0] == _checksum(b, 64), "bad footer checksum"
    return {
        "data_offset": struct.unpack(">Q", b[16:24])[0],
        "current_size": struct.unpack(">Q", b[48:56])[0],
        "disk_type": struct.unpack(">I", b[60:64])[0],
        "unique_id": b[68:84],
        "raw": b,
    }


def read_dynamic_header(path, offset):
    with open(path, "rb") as f:
        f.seek(offset)
        h = f.read(1024)
    assert h[:8] == b"cxsparse", "missing dynamic header"
    assert struct.unpack(">I", h[36:40])[0] == _checksum(h, 36), "bad dynamic header checksum"
    table_offset, = struct.unpack(">Q", h[16:24])
    entries, block_size = struct.unpack(">II", h[28:36])
    locators = []
    for i in range(8):
        code, space, length, _, data_offset = struct.unpack(">IIIIQ", h[576 + i * 24:600 + i * 24])
        locators.append((code, space, length, data_offset))
    return {"table_offset": table_offset, "entries": entries, "block_size": block_size,
            "parent_id": h[40:56], "parent_name": h[64:576].decode("utf-16-be").rstrip("\0"),
            "locators": locators}


def create_differencing(child_path, parent_path, parent_id=None):
    """Writes an empty differencing VHD for `parent_path` (a dynamic or fixed VHD)."""
    parent = read_footer(parent_path)
    size = parent["current_size"]
    block_size = 2 * 1024 * 1024
    entries = (size + block_size - 1) // block_size
    table_offset = 512 + 1024
    table_size = (entries * 4 + 511) // 512 * 512
    rel = ".\\" + os.path.basename(parent_path)
    absolute = os.path.abspath(parent_path)
    loc_rel = rel.encode("utf-16-le")
    loc_abs = absolute.encode("utf-16-le")
    rel_offset = table_offset + table_size
    abs_offset = rel_offset + (len(loc_rel) + 511) // 512 * 512
    data_end = abs_offset + (len(loc_abs) + 511) // 512 * 512

    footer = bytearray(parent["raw"])
    footer[16:24] = struct.pack(">Q", 512)          # data offset
    footer[60:64] = struct.pack(">I", 4)            # differencing
    footer[68:84] = uuid.uuid4().bytes              # new unique id
    footer[64:68] = b"\0\0\0\0"
    footer[64:68] = struct.pack(">I", _checksum(footer, 64))

    header = bytearray(1024)
    header[0:8] = b"cxsparse"
    header[8:16] = b"\xff" * 8
    header[16:24] = struct.pack(">Q", table_offset)
    header[24:36] = struct.pack(">III", 0x00010000, entries, block_size)
    header[40:56] = parent_id if parent_id is not None else parent["unique_id"]
    name = os.path.basename(parent_path).encode("utf-16-be")
    header[64:64 + len(name)] = name
    for i, (code, data, offset) in enumerate(((b"W2ru", loc_rel, rel_offset), (b"W2ku", loc_abs, abs_offset))):
        struct.pack_into(">4sIIIQ", header, 576 + i * 24, code, (len(data) + 511) // 512, len(data), 0, offset)
    header[36:40] = struct.pack(">I", _checksum(header, 36))

    with open(child_path, "wb") as f:
        f.write(footer)
        f.write(header)
        f.write(b"\xff" * table_size)
        f.write(loc_rel.ljust((len(loc_rel) + 511) // 512 * 512, b"\0"))
        f.write(loc_abs.ljust((len(loc_abs) + 511) // 512 * 512, b"\0"))
        assert f.tell() == data_end
        f.write(footer)


def read_disk(path):
    """Full contents of a fixed / dynamic / differencing VHD (follows parents)."""
    footer = read_footer(path)
    size = footer["current_size"]
    if footer["disk_type"] == 2:
        with open(path, "rb") as f:
            return f.read(size)
    header = read_dynamic_header(path, footer["data_offset"])
    parent = None
    if footer["disk_type"] == 4:
        parent_path = os.path.join(os.path.dirname(path), header["parent_name"])
        assert read_footer(parent_path)["unique_id"] == header["parent_id"], "parent id mismatch"
        parent = read_disk(parent_path)
    block_size = header["block_size"]
    sectors_per_block = block_size // SECTOR
    bitmap_size = ((sectors_per_block + 7) // 8 + 511) // 512 * 512
    out = bytearray(size)
    with open(path, "rb") as f:
        f.seek(header["table_offset"])
        bat = struct.unpack(f">{header['entries']}I", f.read(header["entries"] * 4))
        for block, entry in enumerate(bat):
            start = block * block_size
            length = min(block_size, size - start)
            if length <= 0:
                break
            if entry == UNUSED:
                if parent is not None:
                    out[start:start + length] = parent[start:start + length]
                continue
            f.seek(entry * SECTOR)
            bitmap = f.read(bitmap_size)
            data = f.read(block_size)
            for s in range(length // SECTOR):
                present = bitmap[s // 8] & (0x80 >> (s % 8))
                chunk = slice(start + s * SECTOR, start + (s + 1) * SECTOR)
                if present or parent is None:
                    out[chunk] = data[s * SECTOR:(s + 1) * SECTOR]
                else:
                    out[chunk] = parent[chunk]
    return bytes(out)
