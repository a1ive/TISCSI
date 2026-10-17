# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 A1ive
"""Builds raw images of LDM (dynamic) disks for the tests, and reads volumes
back from them.

The structures follow DiskAccessLibrary's LogicalDiskManager reading code
(PRIVHEAD, TOCBLOCK, VMDB, VBLK records). Real Windows-made dynamic disks are
covered by the elevated test in run_tests.py; these images exercise the
layouts Windows client editions cannot create (RAID-5), degraded volumes,
multi-fragment records and damaged header copies, without administrator
rights.
"""
import random
import struct
import uuid
import zlib

SECTOR = 512
PRIVATE_SECTORS = 2048   # private region (LDM database), 1 MiB
CONFIG_START = 17        # relative to the private region
CONFIG_SECTORS = 1024
LOG_START = CONFIG_START + CONFIG_SECTORS
LOG_SECTORS = 224
PRIMARY_TOC = 1
SECONDARY_TOC = 2045
PRIMARY_PRIVHEAD = 2047  # the last sector of the private region
SECONDARY_PRIVHEAD = 1856
BLOCK_SIZE = 128         # VBLK size
MBR_PUBLIC_START = 63

GPT_LDM_METADATA = uuid.UUID("5808C8AA-7E8F-42E0-85D2-E1E90434CFB3")
GPT_LDM_DATA = uuid.UUID("AF9B60A0-1431-4F62-BC68-3311714A69AD")


# ---- Encoding helpers ---------------------------------------------------------

def var_ulong(value):
    data = value.to_bytes((value.bit_length() + 7) // 8, "big") if value else b""
    return bytes([len(data)]) + data


def var_string(text):
    data = text.encode("ascii")
    return bytes([len(data)]) + data


def xor_bytes(a, b):
    return (int.from_bytes(a, "little") ^ int.from_bytes(b, "little")).to_bytes(len(a), "little")


def header_checksum(sector):
    return (sum(sector[:8]) + sum(sector[12:512])) & 0xFFFFFFFF


def fixed(text, length):
    return text.encode("ascii").ljust(length, b"\0")[:length]


class Extent:
    def __init__(self, disk, offset, size, column=0, offset_in_column=0):
        self.disk = disk               # index into the group's disks
        self.offset = offset           # 512-byte sectors from the start of the public region
        self.size = size               # 512-byte sectors
        self.column = column
        self.offset_in_column = offset_in_column


class Plex:
    def __init__(self, layout, extents, stripe=0):
        self.layout = layout           # "concat", "stripe" or "raid5"
        self.extents = extents
        self.stripe = stripe           # sectors per stripe

    def columns(self):
        if self.layout == "concat":
            return [sorted(self.extents, key=lambda e: e.offset_in_column)]
        count = max(e.column for e in self.extents) + 1
        return [sorted((e for e in self.extents if e.column == c), key=lambda e: e.offset_in_column)
                for c in range(count)]

    def sectors(self):
        columns = self.columns()
        if self.layout == "concat":
            return sum(e.size for e in columns[0])
        column = min(sum(e.size for e in c) for c in columns)
        column -= column % self.stripe
        return column * (len(columns) - 1 if self.layout == "raid5" else len(columns))


class Volume:
    def __init__(self, name, plexes, mount_hint=""):
        self.name = name
        self.plexes = plexes
        self.guid = uuid.uuid4()
        self.mount_hint = mount_hint

    def sectors(self):
        return min(p.sectors() for p in self.plexes)


# ---- Layout mapping (volume sector -> member disk sector) --------------------

def _column_pieces(column, sector, count):
    """Yields (extent, offset in extent, count) for a column range."""
    start = 0
    for extent in column:
        end = start + extent.size
        if count and sector < end:
            offset = sector - start
            n = min(count, extent.size - offset)
            yield extent, offset, n
            sector += n
            count -= n
        start = end
    if count:
        raise ValueError("range beyond the column")


def plex_pieces(plex, sector, count):
    """Yields (column index, column sector, count, parity column) per stripe piece."""
    columns = plex.columns()
    if plex.layout == "concat":
        yield 0, sector, count, None
        return
    n = len(columns)
    while count:
        stripe = sector // plex.stripe
        chunk = min(count, plex.stripe - sector % plex.stripe)
        if plex.layout == "stripe":
            yield stripe % n, stripe // n * plex.stripe + sector % plex.stripe, chunk, None
        else:
            row = stripe // (n - 1)
            yield stripe % n, row * plex.stripe + sector % plex.stripe, chunk, n - 1 - row % n
        sector += chunk
        count -= chunk


# ---- Disk group ---------------------------------------------------------------

class Group:
    """A disk group: `disks` image paths (with `gpt` flags) and its volumes."""

    def __init__(self, name, disk_sectors, gpt=None):
        self.name = name
        self.guid = uuid.uuid4()
        self.disk_sectors = disk_sectors
        self.gpt = gpt or []
        self.disk_guids = [uuid.uuid4() for _ in disk_sectors]
        self.volumes = []

    def public_start(self, disk):
        return 34 + PRIVATE_SECTORS if self.gpt[disk] else MBR_PUBLIC_START

    def private_start(self, disk):
        return 34 if self.gpt[disk] else self.disk_sectors[disk] - PRIVATE_SECTORS

    def public_size(self, disk):
        if self.gpt[disk]:
            return self.disk_sectors[disk] - 34 - self.public_start(disk)
        return self.private_start(disk) - MBR_PUBLIC_START

    # ---- Records ----

    def records(self):
        """(id, record bytes) for every database record."""
        records = []
        next_id = [1]

        def new_id():
            next_id[0] += 1
            return next_id[0]

        def record(flags, revision, kind, rid, name, body):
            data = var_ulong(rid) + var_string(name) + body
            header = struct.pack(">HBBI", 0, flags, (revision << 4) | kind, len(data))
            return header + data

        records.append(record(0, 3, 5, new_id(), self.name,
                              var_string(str(self.guid)) + bytes(4) + struct.pack(">Q", 1)))
        disk_ids = []
        for index, guid in enumerate(self.disk_guids):
            rid = new_id()
            disk_ids.append(rid)
            if index % 2 == 0:  # both disk record revisions
                body = var_string(str(guid))
                revision = 3
            else:
                body = guid.bytes + uuid.uuid4().bytes
                revision = 4
            body += var_string(f"Harddisk{index}") + struct.pack(">IQ", 0, 1)
            records.append(record(0, revision, 4, rid, f"Disk{index + 1}", body))
        for number, volume in enumerate(self.volumes):
            volume_id = new_id()
            component_records = []
            for plex in volume.plexes:
                component_id = new_id()
                layout = {"stripe": 1, "concat": 2, "raid5": 3}[plex.layout]
                body = var_string("ACTIVE") + bytes([layout]) + struct.pack(">I", 0) + var_ulong(len(plex.extents))
                body += struct.pack(">Q", 1) + bytes(8) + var_ulong(volume_id) + var_ulong(0)
                flags = 0
                if plex.layout != "concat":
                    flags = 0x10
                    body += var_ulong(plex.stripe) + var_ulong(len(plex.columns()))
                component_records.append(record(flags, 3, 2, component_id, f"{volume.name}-01", body))
                # Extent records in reverse order: readers must sort by offset in column.
                for e_index, extent in reversed(list(enumerate(plex.extents))):
                    body = struct.pack(">IQQQ", 0, 1, extent.offset, extent.offset_in_column)
                    body += var_ulong(extent.size) + var_ulong(component_id) + var_ulong(disk_ids[extent.disk])
                    flags = 0
                    if plex.layout != "concat":
                        flags = 0x08
                        body += var_ulong(extent.column)
                    component_records.append(record(flags, 3, 3, new_id(), f"Disk{extent.disk + 1}-{e_index:02}", body))
            body = var_string("raid5" if any(p.layout == "raid5" for p in volume.plexes) else "gen")
            body += var_string("8000000000000000") + fixed("ACTIVE", 14) + bytes([3 if len(volume.plexes) == 1 else 1])
            body += var_ulong(5 + number) + struct.pack(">I", 0x11) + var_ulong(len(volume.plexes))
            body += struct.pack(">QQ", 1, 0) + var_ulong(volume.sectors()) + bytes(4) + bytes([0x07])
            body += volume.guid.bytes
            flags = 0
            if volume.mount_hint:
                flags = 0x02
                body += var_string(volume.mount_hint)
            records.append(record(flags, 5, 1, volume_id, volume.name, body))
            records.extend(component_records)
        return records

    def config_region(self):
        """The VMDB header and VBLKs. Multi-block records are spread out of
        order to check that readers reassemble them by group number."""
        total_blocks = CONFIG_SECTORS * SECTOR // BLOCK_SIZE
        blocks = [bytes(BLOCK_SIZE)] * total_blocks
        payload = BLOCK_SIZE - 16
        sequence = 4
        far = total_blocks - 1
        for group_number, data in enumerate(self.records(), start=1):
            pieces = [data[i:i + payload] for i in range(0, len(data), payload)]
            slots = []
            for _ in pieces:
                slots.append(sequence)
                sequence += 1
            if len(pieces) > 1:
                # The first fragment goes to the end of the region, after
                # the others in VBLK order.
                slots[0] = far
                far -= 1
            for number, (piece, slot) in enumerate(zip(pieces, slots)):
                blocks[slot] = (b"VBLK" + struct.pack(">IIHH", slot, group_number, number, len(pieces)) +
                                piece.ljust(payload, b"\0"))
        vmdb = bytearray(512)
        vmdb[0:4] = b"VMDB"
        struct.pack_into(">III", vmdb, 4, total_blocks, BLOCK_SIZE, 512)
        struct.pack_into(">HHH", vmdb, 0x10, 1, 4, 10)  # clean, version 4.10
        vmdb[0x16:0x16 + 31] = fixed(self.name, 31)
        vmdb[0x35:0x35 + 64] = fixed(str(self.guid), 64)
        struct.pack_into(">QQ", vmdb, 0x75, 100, 100)
        region = bytearray(b"".join(blocks))
        region[0:512] = vmdb
        return bytes(region)

    def private_header(self, disk):
        sector = bytearray(SECTOR)
        sector[0:8] = b"PRIVHEAD"
        struct.pack_into(">HH", sector, 0x0C, 2, 12)
        struct.pack_into(">QQQ", sector, 0x18, 7, PRIMARY_PRIVHEAD, SECONDARY_PRIVHEAD)
        sector[0x30:0x70] = fixed(str(self.disk_guids[disk]), 0x40)
        sector[0x70:0xB0] = fixed(str(uuid.uuid4()), 0x40)
        sector[0xB0:0xF0] = fixed(str(self.guid), 0x40)
        sector[0xF0:0x10F] = fixed(self.name, 31)
        struct.pack_into(">IIHH", sector, 0x10F, SECTOR, 0, 0, 0)
        struct.pack_into(">QQQQQQ", sector, 0x11B, self.public_start(disk), self.public_size(disk),
                         self.private_start(disk), PRIVATE_SECTORS, PRIMARY_TOC, SECONDARY_TOC)
        struct.pack_into(">IIQQ", sector, 0x14B, 1, 1, CONFIG_SECTORS, LOG_SECTORS)
        struct.pack_into(">I", sector, 0x08, header_checksum(sector))
        return bytes(sector)

    @staticmethod
    def toc_block():
        sector = bytearray(SECTOR)
        sector[0:8] = b"TOCBLOCK"
        struct.pack_into(">Q", sector, 0x0C, 7)
        offset = 0x24
        for name, start, size in (("config", CONFIG_START, CONFIG_SECTORS), ("log", LOG_START, LOG_SECTORS)):
            sector[offset:offset + 8] = fixed(name, 8)
            struct.pack_into(">HQQHH", sector, offset + 8, 0, start, size, 6, 1)
            offset += 34
        struct.pack_into(">I", sector, 0x08, header_checksum(sector))
        return bytes(sector)

    def write_partition_table(self, f, disk):
        total = self.disk_sectors[disk]
        mbr = bytearray(SECTOR)
        if self.gpt[disk]:
            mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, min(total - 1, 0xFFFFFFFF))
            entries = bytearray(128 * 128)
            for index, (kind, first, last, name) in enumerate((
                    (GPT_LDM_METADATA, 34, 34 + PRIVATE_SECTORS - 1, "LDM metadata partition"),
                    (GPT_LDM_DATA, self.public_start(disk), total - 34, "LDM data partition"))):
                entry = kind.bytes_le + uuid.uuid4().bytes_le + struct.pack("<QQQ", first, last, 0)
                entries[index * 128:index * 128 + 128] = entry + name.encode("utf-16-le").ljust(72, b"\0")
            header = bytearray(92)
            header[0:8] = b"EFI PART"
            struct.pack_into("<IIIIQQQQ", header, 8, 0x10000, 92, 0, 0, 1, total - 1, 34, total - 34)
            header[56:72] = uuid.uuid4().bytes_le
            struct.pack_into("<QIII", header, 72, 2, 128, 128, zlib.crc32(entries))
            struct.pack_into("<I", header, 16, zlib.crc32(header))
            f.seek(SECTOR)
            f.write(bytes(header).ljust(SECTOR, b"\0"))
            f.write(entries)
        else:
            mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 0, 0, 0x42, 0, 0, 0, MBR_PUBLIC_START, total - MBR_PUBLIC_START)
            f.seek(6 * SECTOR)
            f.write(self.private_header(disk))  # Windows keeps a copy at sector 6
        mbr[510:512] = b"\x55\xaa"
        f.seek(0)
        f.write(mbr)

    def write_disks(self, paths):
        config = self.config_region()
        for disk, path in enumerate(paths):
            with open(path, "wb") as f:
                f.truncate(self.disk_sectors[disk] * SECTOR)
                self.write_partition_table(f, disk)
                base = self.private_start(disk)
                privhead = self.private_header(disk)
                for lba in (PRIMARY_PRIVHEAD, SECONDARY_PRIVHEAD):
                    f.seek((base + lba) * SECTOR)
                    f.write(privhead)
                for lba in (PRIMARY_TOC, SECONDARY_TOC):
                    f.seek((base + lba) * SECTOR)
                    f.write(self.toc_block())
                f.seek((base + CONFIG_START) * SECTOR)
                f.write(config)

    # ---- Volume content ----

    def disk_offset(self, disk, extent, sector_in_extent):
        return (self.public_start(disk) + extent.offset + sector_in_extent) * SECTOR

    def _column_io(self, files, column, sector, data=None, count=None):
        """Reads (data is None) or writes a column range."""
        out = bytearray()
        position = 0
        count = count if data is None else len(data) // SECTOR
        for extent, offset, n in _column_pieces(column, sector, count):
            f = files[extent.disk]
            f.seek(self.disk_offset(extent.disk, extent, offset))
            if data is None:
                out += f.read(n * SECTOR)
            else:
                f.write(data[position:position + n * SECTOR])
                position += n * SECTOR
        return bytes(out)

    def write_volume(self, files, volume, data):
        """Writes the volume's data, mirror copies and RAID-5 parity."""
        for plex in volume.plexes:
            columns = plex.columns()
            position = 0
            for column, column_sector, count, parity in plex_pieces(plex, 0, len(data) // SECTOR):
                piece = data[position:position + count * SECTOR]
                self._column_io(files, columns[column], column_sector, piece)
                position += count * SECTOR
            if plex.layout == "raid5":
                self.rewrite_parity(files, plex)

    def rewrite_parity(self, files, plex):
        columns = plex.columns()
        n = len(columns)
        rows = plex.sectors() // (n - 1) // plex.stripe
        for row in range(rows):
            parity_column = n - 1 - row % n
            parity = bytes(plex.stripe * SECTOR)
            for c in range(n):
                if c != parity_column:
                    parity = xor_bytes(parity, self._column_io(files, columns[c], row * plex.stripe, count=plex.stripe))
            self._column_io(files, columns[parity_column], row * plex.stripe, parity)

    def read_plex(self, files, plex, sectors):
        columns = plex.columns()
        out = bytearray()
        for column, column_sector, count, _ in plex_pieces(plex, 0, sectors):
            out += self._column_io(files, columns[column], column_sector, count=count)
        return bytes(out)

    def parity_ok(self, files, plex):
        columns = plex.columns()
        n = len(columns)
        rows = plex.sectors() // (n - 1) // plex.stripe
        for row in range(rows):
            acc = bytes(plex.stripe * SECTOR)
            for c in range(n):
                acc = xor_bytes(acc, self._column_io(files, columns[c], row * plex.stripe, count=plex.stripe))
            if any(acc):
                return False
        return True


def open_files(paths, mode="rb"):
    return [open(p, mode) for p in paths]


def random_bytes(seed, length):
    return random.Random(seed).randbytes(length)


def build_test_group(paths):
    """Three 32 MiB disks (the third GPT) with one volume of each layout:
    Volume1 simple, Volume2 spanned, Volume3 striped, Volume4 mirrored,
    Volume5 RAID-5 (whose record spans several VBLKs). Returns (group,
    {name: content})."""
    group = Group("TESTDg0", [65536] * 3, gpt=[False, False, True])
    group.volumes = [
        Volume("Volume1", [Plex("concat", [Extent(0, 0, 8192)])]),
        Volume("Volume2", [Plex("concat", [Extent(0, 8192, 4096, 0, 0), Extent(1, 0, 6144, 0, 4096)])]),
        Volume("Volume3", [Plex("stripe", [Extent(0, 12288, 8192, 0), Extent(1, 6144, 8192, 1),
                                           Extent(2, 0, 8192, 2)], stripe=128)]),
        Volume("Volume4", [Plex("concat", [Extent(1, 14336, 6144)]), Plex("concat", [Extent(2, 8192, 6144)])]),
        Volume("Volume5", [Plex("raid5", [Extent(0, 20480, 8192, 0), Extent(1, 20480, 8192, 1),
                                          Extent(2, 14336, 8192, 2)], stripe=128)],
               mount_hint="X" * 200),
    ]
    group.write_disks(paths)
    contents = {}
    files = open_files(paths, "r+b")
    try:
        for index, volume in enumerate(group.volumes):
            contents[volume.name] = random_bytes(index, volume.sectors() * SECTOR)
            group.write_volume(files, volume, contents[volume.name])
    finally:
        for f in files:
            f.close()
    return group, contents


def damage_primary_headers(group, paths):
    """Breaks the primary PRIVHEAD of disk 0 (checksum) and the primary
    TOCBLOCK of disk 1, so readers must use the secondary copies."""
    with open(paths[0], "r+b") as f:
        f.seek((group.private_start(0) + PRIMARY_PRIVHEAD) * SECTOR + 0xF0)
        f.write(b"Z")
        f.seek(6 * SECTOR + 0xF0)  # and the copy at sector 6
        f.write(b"Z")
    with open(paths[1], "r+b") as f:
        f.seek((group.private_start(1) + PRIMARY_TOC) * SECTOR + 0x30)
        f.write(b"\x01")
