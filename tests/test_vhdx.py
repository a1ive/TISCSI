# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 A1ive
"""VHDX protocol, format and independent-reader regression checks."""
import hashlib
import os
import shutil
import struct
import uuid

import vhdx_tool as vx
from initiator import asc, cdb16, sense_key


def test_vhdx(ctx):
    from run_tests import Server, TARGET, run_tool, QEMU_IMG, qemu_read, reference_content, write_plan, serve_and_check

    def digest(path):
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(vx.MiB), b""):
                h.update(chunk)
        return h.digest()

    def reject(path, label, code=None):
        before = digest(path)
        for option in ("--image", "--image-ro"):
            rc, out = run_tool(ctx.exe, "--lang", "en-US", option, path)
            ctx.check(rc == 2 and (code is None or f"({code})" in out), label + " " + option)
        ctx.check(digest(path) == before, label + ": unchanged on rejection")

    def overwrite(path, offset, value):
        with open(path, "r+b") as f:
            f.seek(offset)
            f.write(value)

    if QEMU_IMG:
        ref = reference_content()
        raw = ctx.make_image("vx-ref.raw", len(ref), ref)
        images = []
        for kind in ("fixed", "dynamic"):
            path = ctx.path("q-" + kind + ".vhdx")
            rc, out = run_tool(QEMU_IMG, "convert", "-f", "raw", "-O", "vhdx",
                               "-o", f"subformat={kind},block_size=1048576", raw, path)
            ctx.check(rc == 0, "qemu creates " + kind + " VHDX")
            images.append((kind + " VHDX", path))
        expected = serve_and_check(ctx, images, ref, write_plan())
        for label, path in images:
            ctx.check(qemu_read(path, "vhdx") == expected, label + ": qemu reads our writes")
            with Server(ctx, ["--target", TARGET, "--image-ro", path]):
                s = ctx.session()
                s.full_login(TARGET)
                from run_tests import read_all
                ctx.check(read_all(s, 0) == expected, label + ": reopen reads all data")
                st, sense, _, _ = s.write(0, bytes(512))
                ctx.check(st == 2 and sense_key(sense) == 7, label + ": read-only rejects writes")
                s.logout()

        # The final logged BAT update is a recovery oracle: restore its
        # LogGuid, damage the destination sector, let qemu replay the record.
        recovery = ctx.path("replay.vhdx")
        shutil.copyfile(images[1][1], recovery)
        h = bytearray(vx.header(recovery))
        log_offset, = struct.unpack_from("<Q", h, 72)
        records = []
        with open(recovery, "rb") as f:
            for slot in (0, 8192):
                f.seek(log_offset + slot)
                record = f.read(8192)
                if record[:4] == b"loge":
                    records.append(record)
        record = max(records, key=lambda r: struct.unpack_from("<Q", r, 16)[0])
        ctx.check(record == vx.seal(record), "VHDX journal CRC32C is valid")
        destination, = struct.unpack_from("<Q", record, 80)
        h[48:64] = record[32:48]
        overwrite(recovery, 131072, vx.seal(h))
        h[8:16] = (struct.unpack_from("<Q", h, 8)[0] - 1).to_bytes(8, "little")
        overwrite(recovery, 65536, vx.seal(h))
        overwrite(recovery, destination, bytes(4096))
        reject(recovery, "pending log requires replay", 50)
        rc, out = run_tool(QEMU_IMG, "check", "-r", "all", recovery)
        ctx.check(rc == 0, "qemu accepts and replays TISCSI's journal: " + out.strip())
        ctx.check(qemu_read(recovery, "vhdx") == expected, "qemu replays TISCSI's BAT log and restores all data")
    else:
        print("  SKIP VHDX qemu interoperability (qemu-img not found)")

    for sector in (512, 4096):
        block = vx.MiB
        size = 4 * block + sector * 3
        parent = ctx.path(f"parent-{sector}.vhdx")
        child = ctx.path(f"child-{sector}.avhdx")
        parent_bytes = bytearray(size)
        parent_bytes[:block] = os.urandom(block)
        parent_bytes[2*block:3*block] = os.urandom(block)
        parent_bytes[-sector:] = os.urandom(sector)
        vx.create(parent, size=size, sector=sector, block=block,
                  writes=[(0, parent_bytes[:block]), (2*block, parent_bytes[2*block:3*block]),
                          (size-sector, parent_bytes[-sector:])])
        child_data = os.urandom(sector)
        vx.create(child, size=size, sector=sector, block=block, parent=parent,
                  writes=[(sector, child_data), (block-sector, child_data)],
                  states={2: 2})
        expected = bytearray(parent_bytes)
        expected[sector:2*sector] = child_data
        expected[block-sector:block] = child_data
        expected[2*block:3*block] = bytes(block)
        parent_hash = digest(parent)
        child_id = vx.header(child)[32:48]
        grandchild = ctx.path(f"grandchild-{sector}.vhdx")
        vx.create(grandchild, size=size, sector=sector, block=block, parent=child)
        # Read-only multilevel chain leaves DataWriteGuid intact.
        for path in (child, grandchild):
            before = digest(path)
            with Server(ctx, ["--target", TARGET, "--image-ro", path]):
                s = ctx.session()
                s.full_login(TARGET)
                ctx.check(s.capacity()[1:] == (size, sector), f"{sector} sector capacity and geometry")
                actual = bytearray()
                for offset in range(0, size, block):
                    length = min(block, size-offset)
                    st, data, _, _ = s.read(offset//sector, length//sector, block_size=sector)
                    actual.extend(data)
                    ctx.check(st == 0, "read partial child or inherited block")
                ctx.check(actual == expected, "partial bitmap / parent chain / ZERO override")
                st, sense, _, _ = s.write(0, bytes(sector), block_size=sector)
                ctx.check(st == 2 and sense_key(sense) == 7, "child read-only sense")
                s.logout()
            ctx.check(digest(path) == before, "read-only chain is byte-for-byte unchanged")
        # Writable open with no write must preserve child linkage.
        with Server(ctx, ["--target", TARGET, "--image", child]):
            pass
        ctx.check(vx.header(child)[32:48] == child_id, "idle writable open preserves DataWriteGuid")

        writes = [(0, os.urandom(sector)), (block-sector, os.urandom(3*sector)),
                  (2*block+sector, os.urandom(sector)), (size-sector, os.urandom(sector))]
        with Server(ctx, ["--target", TARGET, "--image", child]):
            s = ctx.session()
            s.full_login(TARGET)
            for offset, data in writes:
                ctx.check(s.write(offset//sector, data, block_size=sector)[0] == 0,
                          "child writes partial, absent, ZERO, cross-block and last sectors")
                expected[offset:offset+len(data)] = data
            ctx.check(s.scsi(cdb16(0x91, 0, 0))[0] == 0, "VHDX SYNCHRONIZE CACHE")
            for offset in range(0, size, block):
                length = min(block, size-offset)
                st, data, _, _ = s.read(offset//sector, length//sector, block_size=sector)
                ctx.check(st == 0 and data == expected[offset:offset+length], "written child retains untouched sectors")
            st, sense, _, _ = s.read(size//sector, 1, block_size=sector)
            ctx.check(st == 2 and asc(sense) == 0x21, "VHDX end-of-disk range check")
            s.logout()
        ctx.check(digest(parent) == parent_hash, "parent is byte-for-byte unchanged after child writes")
        ctx.check(vx.header(child)[32:48] != child_id, "writes change child's DataWriteGuid")
        ctx.check(vx.read(child, 0, size, parent) == expected, "spec reader sees persisted child contents")
        reject(grandchild, "stale grandchild linkage")
        with Server(ctx, ["--target", TARGET, "--image-ro", child]):
            s = ctx.session()
            s.full_login(TARGET)
            st, data, _, _ = s.read((size-sector)//sector, 1, block_size=sector)
            ctx.check(st == 0 and data == expected[-sector:], "child reopens after writes")
            s.logout()
        wrong = ctx.path(f"wrong-{sector}.vhdx")
        vx.create(wrong, size=size, sector=sector, block=block, parent=parent, parent_id=uuid.uuid4().bytes_le)
        reject(wrong, "wrong parent DataWriteGuid")
        orphan = ctx.path(f"orphan-{sector}.vhdx")
        shutil.copyfile(child, orphan)
        renamed_parent = parent + ".moved"
        os.rename(parent, renamed_parent)
        try:
            reject(orphan, "missing parent", 2)
        finally:
            os.rename(renamed_parent, parent)

        partial = ctx.path(f"missing-bitmap-{sector}.vhdx")
        vx.create(partial, size=size, sector=sector, block=block, parent=parent, writes=[(0, child_data)])
        ratio = (1 << 23) * sector // block
        overwrite(partial, 2*vx.MiB + ratio*8, bytes(8))
        reject(partial, "partial child without sector bitmap")

    # BAT indexing around sector bitmap slots, with virtual capacity >4 GiB.
    for sector in (512, 4096):
        boundary = (1 << 23) * sector
        parent = ctx.path(f"large-parent-{sector}.vhdx")
        child = ctx.path(f"large-child-{sector}.vhdx")
        pattern = os.urandom(sector)
        vx.create(parent, size=boundary+vx.MiB, sector=sector, block=vx.MiB,
                  writes=[(boundary-sector, pattern), (boundary+sector, pattern)])
        vx.create(child, size=boundary+vx.MiB, sector=sector, block=vx.MiB, parent=parent,
                  writes=[(boundary, pattern)])
        with Server(ctx, ["--target", TARGET, "--image", child]):
            s = ctx.session()
            s.full_login(TARGET)
            ctx.check(s.capacity()[1:] == (boundary+vx.MiB, sector), "large VHDX capacity")
            st, data, _, _ = s.read(boundary//sector-1, 3, block_size=sector)
            ctx.check(st == 0 and data == pattern*3, "partial child across BAT bitmap slot")
            replacement = os.urandom(sector*3)
            ctx.check(s.write(boundary//sector-1, replacement, block_size=sector)[0] == 0,
                      "write across BAT bitmap slot")
            s.logout()
        ctx.check(vx.read(child, boundary-sector, 3*sector, parent) == replacement,
                  "large child persistence across bitmap slot")

    maximum = ctx.path("64tib.vhdx")
    vx.create(maximum, size=64 << 40, block=256*vx.MiB)
    with Server(ctx, ["--target", TARGET, "--image-ro", maximum]):
        s = ctx.session()
        s.full_login(TARGET)
        ctx.check(s.capacity()[1:] == (64 << 40, 512), "64 TiB VHDX reports full capacity")
        st, data, _, _ = s.read(((64 << 40)//512)-1, 1)
        ctx.check(st == 0 and data == bytes(512), "64 TiB last sector")
        s.logout()

    # The zero-producing states must not inherit data from a parent.
    states_parent = ctx.path("states-parent.vhdx")
    states_child = ctx.path("states.vhdx")
    vx.create(states_parent, size=4*vx.MiB, block=vx.MiB,
              writes=[(i*vx.MiB, bytes([0x61+i])*512) for i in range(4)])
    vx.create(states_child, size=4*vx.MiB, block=vx.MiB, parent=states_parent, states={0: 1, 1: 2, 2: 3})
    with Server(ctx, ["--target", TARGET, "--image", states_child]):
        s = ctx.session()
        s.full_login(TARGET)
        for block in range(4):
            st, data, _, _ = s.read(block*vx.MiB//512, 1)
            expected = bytes(512) if block != 3 else bytes([0x64])*512
            ctx.check(st == 0 and data == expected, f"BAT state {block}: zero or parent read")
            ctx.check(s.write(block*vx.MiB//512+1, bytes([0x91])*512)[0] == 0,
                      f"BAT state {block}: allocation")
            st, data, _, _ = s.read(block*vx.MiB//512, 2)
            ctx.check(st == 0 and data == expected+bytes([0x91])*512, "allocation preserves defined contents")
        s.logout()

    base = ctx.path("valid.vhdx")
    vx.create(base, writes=[(0, bytes([0x5A])*512)])
    for label, mutation in (
        ("signature", lambda p: overwrite(p, 0, b"badxfile")),
        ("both header CRCs", lambda p: (overwrite(p, 65540, bytes(4)), overwrite(p, 131076, bytes(4)))),
        ("both region CRCs", lambda p: (overwrite(p, 196612, bytes(4)), overwrite(p, 262148, bytes(4)))),
        ("BAT overlaps metadata", lambda p: overwrite(p, 2*vx.MiB, struct.pack("<Q", 3*vx.MiB | 6))),
        ("BAT outside file", lambda p: overwrite(p, 2*vx.MiB, struct.pack("<Q", 99*vx.MiB | 6))),
        ("reserved BAT state", lambda p: overwrite(p, 2*vx.MiB, struct.pack("<Q", 4))),
        ("partial block without parent", lambda p: overwrite(p, 2*vx.MiB, struct.pack("<Q", 4*vx.MiB | 7))),
        ("invalid logical sector", lambda p: overwrite(p, 3*vx.MiB+65536+8+8+16, struct.pack("<I", 1024))),
        ("invalid block size", lambda p: overwrite(p, 3*vx.MiB+65536, struct.pack("<I", 3*vx.MiB))),
        ("metadata item overlaps table", lambda p: overwrite(p, 3*vx.MiB+48, struct.pack("<I", 32))),
        ("truncated payload", lambda p: os.truncate(p, os.path.getsize(p)-512)),
    ):
        path = ctx.path("bad-" + label.replace(" ", "-") + ".vhdx")
        shutil.copyfile(base, path)
        mutation(path)
        reject(path, label)
    for offset, label in ((65540, "first header"), (131076, "second header"),
                          (196612, "first region table"), (262148, "second region table")):
        path = ctx.path(label.replace(" ", "-") + ".VHDX")
        shutil.copyfile(base, path)
        overwrite(path, offset, bytes(4))
        with Server(ctx, ["--target", TARGET, "--image-ro", path]):
            s = ctx.session()
            s.full_login(TARGET)
            st, data, _, _ = s.read(0, 1)
            ctx.check(st == 0 and data == bytes([0x5A])*512, "fallback after corrupt " + label)
            s.logout()

    def rewrite_record(path, offset, length, mutate):
        with open(path, "rb") as f:
            f.seek(offset)
            record = bytearray(f.read(length))
        mutate(record)
        overwrite(path, offset, vx.seal(record))

    for required in (False, True):
        path = ctx.path(f"unknown-region-{required}.vhdx")
        shutil.copyfile(base, path)
        def add_region(table):
            struct.pack_into("<I", table, 8, 3)
            table[80:96] = uuid.UUID("e4082e27-47e2-4471-866a-19472bfc5010").bytes_le
            struct.pack_into("<QII", table, 96, 6*vx.MiB, vx.MiB, int(required))
        os.truncate(path, 7*vx.MiB)
        for offset in (196608, 262144):
            rewrite_record(path, offset, 65536, add_region)
        if required:
            reject(path, "unknown required region", 50)
        else:
            with Server(ctx, ["--target", TARGET, "--image", path]):
                s = ctx.session()
                s.full_login(TARGET)
                ctx.check(s.write(4096, bytes([0x66])*512)[0] == 0, "unknown optional region permits allocation")
                s.logout()
            with open(path, "rb") as f:
                f.seek(6*vx.MiB)
                ctx.check(f.read(vx.MiB) == bytes(vx.MiB), "unknown optional region is preserved")

    for required in (False, True):
        path = ctx.path(f"unknown-metadata-{required}.vhdx")
        shutil.copyfile(base, path)
        # Unknown metadata is not protected by a table CRC.
        overwrite(path, 3*vx.MiB+10, struct.pack("<H", 6))
        overwrite(path, 3*vx.MiB+32+5*32, uuid.uuid4().bytes_le +
                  struct.pack("<IIII", 65536+40, 4, 4 if required else 0, 0))
        overwrite(path, 3*vx.MiB+65536+40, b"test")
        if required:
            reject(path, "unknown required metadata", 50)
        else:
            with Server(ctx, ["--target", TARGET, "--image-ro", path]):
                pass
    equal = ctx.path("equal-header-sequences.vhdx")
    shutil.copyfile(base, equal)
    overwrite(equal, 65536, vx.header(equal))
    reject(equal, "equal header sequences")

    duplicate = ctx.path("duplicate-metadata.vhdx")
    shutil.copyfile(base, duplicate)
    overwrite(duplicate, 3*vx.MiB+64, vx.PARAM)
    reject(duplicate, "duplicate metadata item")

    cycle = ctx.path("cycle.vhdx")
    vx.create(cycle)
    vx.create(cycle, parent=cycle)
    reject(cycle, "cyclic parent chain")
