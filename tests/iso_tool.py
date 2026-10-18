# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 A1ive
"""Minimal ISO 9660 image writer for tests, written from ECMA-119.

Makes a single-directory image (level 1 names, no Joliet or Rock Ridge) that
Windows and Linux mount, so tests need no mkisofs.
"""
import struct
import time

SECTOR = 2048


def _both16(value):
    return struct.pack("<H", value) + struct.pack(">H", value)


def _both32(value):
    return struct.pack("<I", value) + struct.pack(">I", value)


def _text(value, width):
    return value.encode("ascii").ljust(width, b" ")[:width]


def _record_date():
    t = time.gmtime()
    return bytes([t.tm_year - 1900, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 0])


def _volume_date():
    return time.strftime("%Y%m%d%H%M%S00", time.gmtime()).encode("ascii") + b"\0"


def _directory_record(name, extent, length, directory):
    record = bytearray(33 + len(name) + (1 - len(name) % 2))
    record[0] = len(record)
    record[2:10] = _both32(extent)
    record[10:18] = _both32(length)
    record[18:25] = _record_date()
    record[25] = 0x02 if directory else 0x00
    record[28:32] = _both16(1)
    record[32] = len(name)
    record[33:33 + len(name)] = name
    return bytes(record)


def make_iso(path, files, volume_id="TISCSI", size=None):
    """Writes an image with `files` ({"NAME.EXT": bytes}) in the root
    directory. `size` pads the image (sparse where the file system allows)."""
    root_extent = 20
    extent = root_extent + 1
    entries = []
    for name, data in sorted(files.items()):
        entries.append((name.upper().encode("ascii") + b";1", extent, data))
        extent += max(1, -(-len(data) // SECTOR))
    total = extent

    root = _directory_record(b"\0", root_extent, SECTOR, True)
    directory = root + _directory_record(b"\1", root_extent, SECTOR, True)
    for name, start, data in entries:
        directory += _directory_record(name, start, len(data), False)
    assert len(directory) <= SECTOR, "too many files for one directory sector"

    pvd = bytearray(SECTOR)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[8:40] = _text("", 32)
    pvd[40:72] = _text(volume_id, 32)
    pvd[80:88] = _both32(total)
    pvd[120:124] = _both16(1)
    pvd[124:128] = _both16(1)
    pvd[128:132] = _both16(SECTOR)
    pvd[132:140] = _both32(10)                # path table size
    pvd[140:144] = struct.pack("<I", 18)      # type L path table
    pvd[148:152] = struct.pack(">I", 19)      # type M path table
    pvd[156:190] = root
    pvd[190:702] = _text("", 512)             # volume set, publisher, preparer, application
    pvd[702:813] = _text("", 111)             # copyright, abstract, bibliographic files
    pvd[813:830] = _volume_date()
    pvd[830:847] = _volume_date()
    pvd[847:864] = b"0" * 16 + b"\0"
    pvd[864:881] = b"0" * 16 + b"\0"
    pvd[881] = 1

    terminator = bytearray(SECTOR)
    terminator[0:7] = b"\xffCD001\x01"
    path_l = bytearray(SECTOR)
    path_l[0:10] = bytes([1, 0]) + struct.pack("<IH", root_extent, 1) + b"\0\0"
    path_m = bytearray(SECTOR)
    path_m[0:10] = bytes([1, 0]) + struct.pack(">IH", root_extent, 1) + b"\0\0"

    with open(path, "wb") as f:
        f.write(bytes(16 * SECTOR) + pvd + terminator + path_l + path_m + directory.ljust(SECTOR, b"\0"))
        for _, start, data in entries:
            f.seek(start * SECTOR)
            f.write(data)
        f.truncate(max(total * SECTOR, size or 0))
    return path
