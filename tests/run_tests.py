# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 A1ive
"""End-to-end tests: starts TISCSICli and talks to it with tests/initiator.py.

usage: python tests/run_tests.py [--exe PATH] [--port N] [-k NAME]

Exits with a non-zero status if any check fails. Windows only (uses
Ctrl+Break to stop the server gracefully).
"""
import argparse
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iso_tool  # noqa: E402
import ldm_tool  # noqa: E402
import vhd_tool  # noqa: E402
from test_vhdx import test_vhdx  # noqa: E402
from initiator import DEFAULT_INITIATOR, Session, asc, cdb10, cdb16, sense_key  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = "127.0.0.1"
TARGET = "iqn.2026-10.test:disk"
DEFAULT_TARGET = "iqn.1991-05.com.microsoft:target1"


class Context:
    def __init__(self, exe, port, workdir):
        self.exe = exe
        self.port = port
        self.workdir = workdir
        self.failures = 0
        self.checks = 0

    def check(self, condition, what):
        self.checks += 1
        print(("  PASS " if condition else "  FAIL ") + what)
        if not condition:
            self.failures += 1

    def path(self, name):
        return os.path.join(self.workdir, name)

    def make_image(self, name, size, content=None):
        path = self.path(name)
        with open(path, "wb") as f:
            if content:
                f.write(content)
            f.truncate(size)
        return path

    def session(self, isid=0x23D000000001):
        return Session(HOST, self.port, isid=isid)


class Server:
    """TISCSICli child process, stopped with Ctrl+Break."""

    def __init__(self, ctx, args, lang="en-US"):
        self.ctx = ctx
        command = [ctx.exe, "--listen", HOST, "--port", str(ctx.port), "--verbosity", "warning", "--lang", lang]
        self.process = subprocess.Popen(command + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        self.output = ""
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("server exited: " + self.process.stdout.read().decode("utf-8", "replace"))
            try:
                socket.create_connection((HOST, ctx.port), timeout=1).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("server did not start listening")

    def stop(self, timeout=20):
        """Returns (stopped cleanly, seconds taken)."""
        start = time.time()
        os.kill(self.process.pid, signal.CTRL_BREAK_EVENT)
        try:
            out, _ = self.process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            out, _ = self.process.communicate()
            self.output = out.decode("utf-8", "replace")
            return False, time.time() - start
        self.output = out.decode("utf-8", "replace")
        return self.process.returncode == 0, time.time() - start

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if self.process.poll() is None:
            ok, _ = self.stop()
            self.ctx.check(ok, "server stops cleanly")


# ---------------------------------------------------------------------------

def test_cli_errors(ctx):
    def run(*args):
        result = subprocess.run([ctx.exe, "--lang", "en-US", *args], capture_output=True, timeout=10)
        return result.returncode, result.stderr.decode("utf-8", "replace") + result.stdout.decode("utf-8", "replace")

    rc, out = run("--help")
    ctx.check(rc == 0 and "Usage: TISCSICli" in out, "--help prints usage")
    result = subprocess.run([ctx.exe, "--help", "--lang=ja-JP"], capture_output=True, timeout=10)
    ctx.check(result.returncode == 0 and "使い方" in result.stdout.decode("utf-8", "replace"),
              "--lang=ja-JP prints Japanese usage (UTF-8 when redirected)")
    for tag, word in (("zh-CN", "用法"), ("zh-TW", "說明"), ("ko-KR", "사용법"), ("tr-TR", "Kullanım")):
        result = subprocess.run([ctx.exe, "--help", "--lang", tag], capture_output=True, timeout=10)
        ctx.check(result.returncode == 0 and word in result.stdout.decode("utf-8", "replace"),
                  f"--lang {tag} prints localized usage")
    result = subprocess.run([ctx.exe, "--lang", "zh-TW", "--ram", "12X"], capture_output=True, timeout=10)
    ctx.check(result.returncode == 1 and "大小無效" in result.stderr.decode("utf-8", "replace"),
              "errors are localized (zh-TW)")
    result = subprocess.run([ctx.exe, "--lang", "tr-TR", "--ram", "12X"], capture_output=True, timeout=10)
    ctx.check(result.returncode == 1 and "Geçersiz boyut" in result.stderr.decode("utf-8", "replace"),
              "errors are localized (tr-TR)")
    rc, out = run("--bogus")
    ctx.check(rc == 1 and "Unknown option: --bogus" in out, "unknown option is rejected")
    rc, out = run("--ram", "12X")
    ctx.check(rc == 1 and "Invalid size" in out, "invalid size is rejected")
    rc, out = run("--target", "not-an-iqn", "--ram", "1M")
    ctx.check(rc == 1 and "Invalid iSCSI target name" in out, "invalid target name is rejected")
    rc, out = run("--target", TARGET)
    ctx.check(rc == 1 and "has no disks" in out, "target without disks is rejected")
    rc, out = run("--image", ctx.path("missing.img"))
    ctx.check(rc == 2 and "Cannot open the disk image" in out, "missing image is reported")


def test_protocol(ctx):
    ram_size = 64 * 1024 * 1024
    image = ctx.make_image("readonly.img", 32 * 1024 * 1024)
    with Server(ctx, ["--target", TARGET, "--ram", "64M", "--image-ro", image]):
        _protocol_checks(ctx, ram_size, os.path.getsize(image))


def test_synchronize_cache(ctx):
    size = 1024 * 1024
    raw = ctx.make_image("sync-cache.img", size)
    readonly = ctx.make_image("sync-cache-ro.img", size)
    optical = ctx.make_image("sync-cache.iso", size)
    pattern = os.urandom(4096)
    with Server(ctx, ["--target", TARGET, "--ram", "1M", "--image", raw,
                      "--image-ro", readonly, "--image", optical]):
        s = ctx.session()
        ctx.check(s.full_login(TARGET)[0] == 0, "SYNCHRONIZE CACHE login")
        ctx.check(s.write(7, pattern, lun=1)[0] == 0, "write data before SYNCHRONIZE CACHE")
        for opcode, encode, name in ((0x35, cdb10, "SYNCHRONIZE CACHE(10)"),
                                     (0x91, cdb16, "SYNCHRONIZE CACHE(16)")):
            for lun in range(4):
                for lba, blocks in ((0, 0), (7, 8), (7, 0)):
                    st, data, r2ts, _ = s.scsi(encode(opcode, lba, blocks), lun=lun)
                    ctx.check(st == 0 and not data and r2ts == 0,
                              f"{name} LUN {lun}, LBA {lba}, blocks {blocks}: GOOD without data transfer")
            st, data, _, _ = s.scsi(encode(opcode, 0, 0), lun=4)
            ctx.check(st == 2 and sense_key(data) == 5 and asc(data) == 0x25,
                      name + " rejects missing LUN")
        st, data, _, _ = s.read(7, 8, lun=1)
        ctx.check(st == 0 and data == pattern, "SYNCHRONIZE CACHE preserves written data")
        s.logout()
    with open(raw, "rb") as f:
        ctx.check(f.read() == bytes(7 * 512) + pattern + bytes(size - 15 * 512),
                  "image contains the expected data after SYNCHRONIZE CACHE")


def test_verify(ctx):
    def cdb(op, lba, blocks, flags=0):
        if op == 0x2F:
            return struct.pack(">BBIBHB", op, flags, lba, 0, blocks, 0)
        if op == 0xAF:
            return struct.pack(">BBIIBB", op, flags, lba, blocks, 0, 0)
        return struct.pack(">BBQIBB", op, flags, lba, blocks, 0, 0)

    def check_sense(result, key, code, what, lba=None):
        st, data, _, _ = result
        good = st == 2 and sense_key(data) == key and asc(data) == code
        good = good and len(data) == 20 and data[15] == 0 and int.from_bytes(data[:2], "big") == 18
        if lba is not None:
            good = good and data[2] & 0x80 and int.from_bytes(data[5:9], "big") == lba
        ctx.check(good, what)

    size = 8 * 1024 * 1024
    lba, blocks = 7, 2051  # cross the bounded-read boundary, including a partial last chunk
    pattern = os.urandom(blocks * 512)
    optical_pattern = os.urandom(blocks * 2048)
    raw = ctx.make_image("verify.img", size, bytes(lba * 512) + pattern)
    readonly = ctx.make_image("verify-ro.img", size, bytes(lba * 512) + pattern)
    optical = ctx.make_image("verify.iso", size, bytes(lba * 2048) + optical_pattern)
    with Server(ctx, ["--target", TARGET, "--ram", "64M", "--image", raw,
                      "--image-ro", readonly, "--image", optical]):
        s = ctx.session()
        ctx.check(s.full_login(TARGET, [("InitialR2T", "Yes"), ("ImmediateData", "No"),
                                       ("MaxBurstLength", "65536")])[0] == 0, "VERIFY login")
        ctx.check(s.write(lba, pattern)[0] == 0, "prepare VERIFY data on RAM disk")
        for lun, block_size, content, disk_size in ((0, 512, pattern, 64 * 1024 * 1024),
                                                   (1, 512, pattern, size), (2, 512, pattern, size),
                                                   (3, 2048, optical_pattern, size)):
            total = disk_size // block_size
            for op, name in ((0x2F, "VERIFY(10)"), (0xAF, "VERIFY(12)"), (0x8F, "VERIFY(16)")):
                label = f"{name} LUN {lun}"
                st, data, _, _ = s.scsi(cdb(op, lba, blocks), lun=lun)
                ctx.check(st == 0 and not data, label + " reads the range without Data-In")
                st, data, r2ts, _ = s.scsi(cdb(op, lba, blocks, 2), lun=lun, write_data=content)
                ctx.check(st == 0 and not data and r2ts > 1, label + " compares all blocks through multiple R2Ts")
                for offset in (0, len(content) - 1):
                    bad = bytearray(content)
                    bad[offset] ^= 0xFF
                    check_sense(s.scsi(cdb(op, lba, blocks, 2), lun=lun, write_data=bad), 0x0E, 0x1D,
                                label + f" detects mismatch at byte {offset}", lba + offset // block_size)
                ctx.check(s.scsi(cdb(op, total - 1, 1, 0x10), lun=lun)[0] == 0,
                          label + " accepts last block and DPO")
                for start, count in ((total, 1), (total - 1, 2), (0xFFFFFFFF, 2)):
                    check_sense(s.scsi(cdb(op, start, count), lun=lun), 5, 0x21,
                                label + f" rejects out-of-range {start}+{count}")
                for mode in (0, 1, 3):
                    if lun == 3 and mode == 3:
                        continue  # MMC bit 2 means BLKVFY, not SBC repeat comparison
                    ctx.check(s.scsi(cdb(op, total, 0, mode << 1), lun=lun)[0] == 0,
                              label + f" zero length, BYTCHK={mode}, is a no-op")
                for flags in (4, 0x20, 0x40, 0xE0, 8, 1):
                    check_sense(s.scsi(cdb(op, lba, 1, flags), lun=lun), 5, 0x24,
                                label + f" rejects unsupported flags 0x{flags:02x}")
                for supplied in (None, content[:block_size - 1], content[:block_size + 1]):
                    check_sense(s.scsi(cdb(op, lba, 1, 2), lun=lun, write_data=supplied), 5, 0x24,
                                label + " rejects missing/short/excess comparison data")
                check_sense(s.scsi(cdb(op, lba, 1), lun=lun, write_data=content[:block_size]), 5, 0x24,
                            label + " rejects Data-Out with BYTCHK=0")
                if lun != 3:
                    zero = bytes(block_size)
                    ctx.check(s.scsi(cdb(op, total - blocks, blocks, 6), lun=lun, write_data=zero)[0] == 0,
                              label + " compares one repeated block across chunks")
                    check_sense(s.scsi(cdb(op, lba - 1, 2, 6), lun=lun, write_data=zero), 0x0E, 0x1D,
                                label + " repeated block detects second-block mismatch", lba)
                    check_sense(s.scsi(cdb(op, lba, 2, 6), lun=lun, write_data=bytes(block_size * 2)), 5, 0x24,
                                label + " repeated comparison requires exactly one block")
                else:
                    check_sense(s.scsi(cdb(op, lba, 1, 6), lun=lun), 5, 0x24,
                                label + " rejects MMC blank verification")
            st, data, _, _ = s.read(lba, blocks, lun=lun, block_size=block_size)
            ctx.check(st == 0 and data == content, f"VERIFY did not modify LUN {lun}")
        for op in (0xAF, 0x8F):
            ctx.check(s.scsi(cdb(op, 0, 65537))[0] == 0, "32-bit verification length exceeds 65535 blocks")
            check_sense(s.scsi(cdb(op, 0, 0xFFFFFFFF)), 5, 0x21, "large count is checked without allocation")
        check_sense(s.scsi(cdb(0x8F, 1 << 32, 1)), 5, 0x21, "VERIFY(16) preserves high LBA bits")
        check_sense(s.scsi(cdb(0x8F, (1 << 64) - 1, 2)), 5, 0x21, "VERIFY(16) range cannot wrap")
        s.logout()
        s = ctx.session(isid=0x23D000000002)
        ctx.check(s.full_login(TARGET, [("InitialR2T", "No"), ("ImmediateData", "Yes")])[0] == 0,
                  "VERIFY immediate/unsolicited login")
        for op in (0x2F, 0xAF, 0x8F):
            ctx.check(s.scsi(cdb(op, lba, blocks, 2), write_data=pattern)[0] == 0,
                      "VERIFY compares immediate, unsolicited and R2T data")
        s.logout()
    with open(raw, "rb") as f:
        ctx.check(f.read() == bytes(lba * 512) + pattern + bytes(size - (lba + blocks) * 512),
                  "VERIFY leaves the writable image unchanged on disk")


def _protocol_checks(ctx, lun0_size, lun1_size):
    # Discovery
    s = ctx.session(isid=0x23D000000010)
    status, tsih, _ = s.full_login(None, [("MaxRecvDataSegmentLength", "65536")])
    ctx.check(status == 0 and tsih != 0, f"discovery login (TSIH={tsih})")
    rbhs, resp = s.text([("SendTargets", "All")])
    ctx.check(TARGET in resp.get("TargetName", []), "SendTargets lists the target")
    ctx.check(struct.unpack(">I", rbhs[20:24])[0] == 0xFFFFFFFF, "final Text Response has the reserved TTT")
    st, _, _, _ = s.scsi(bytes(6))
    ctx.check(st == "reject", "SCSI command is rejected in a discovery session")
    code, closed = s.logout()
    ctx.check(code == 0 and closed, "discovery logout closes the connection")

    # Unknown target
    s = ctx.session(isid=0x23D000000011)
    _, status, _, _ = s.login([("InitiatorName", s.initiator), ("TargetName", "iqn.2026-10.test:nope")], 1, 3)
    ctx.check(status == 0x0203, "unknown target -> Not Found (0x0203)")
    ctx.check(s.is_closed_by_target(), "connection is closed after a failed login")
    s.close()

    # Session A: InitialR2T=Yes, ImmediateData=No (pure R2T writes)
    s = ctx.session(isid=0x23D000000001)
    status, tsih, security_ok = s.full_login(TARGET, [
        ("MaxRecvDataSegmentLength", "65536"), ("InitialR2T", "Yes"), ("ImmediateData", "No"),
        ("MaxBurstLength", "65536"), ("FirstBurstLength", "65536"), ("MaxOutstandingR2T", "1"),
        ("MaxConnections", "1"), ("DefaultTime2Wait", "2"), ("DefaultTime2Retain", "0")], security_stage=True)
    ctx.check(security_ok, "security negotiation stage")
    ctx.check(status == 0 and tsih != 0, f"normal login (TSIH={tsih})")
    ctx.check(s.params.get("InitialR2T") == "Yes" and s.params.get("ImmediateData") == "No",
              "negotiated InitialR2T=Yes, ImmediateData=No")
    ctx.check(s.params.get("MaxBurstLength") == "65536", "negotiated MaxBurstLength=65536")

    st, _, _, _ = s.scsi(bytes(6))
    ctx.check(st == 0, "TEST UNIT READY")
    st, data, _, _ = s.scsi(bytes([0x12, 0, 0, 0, 96, 0]), read_len=96)
    ctx.check(st == 0 and data[8:14] == b"TISCSI" and data[16:27] == b"SCSI Disk 0", "standard INQUIRY")
    ctx.check(b"\x09\x60" in data[58:74], "INQUIRY carries the iSCSI version descriptor")
    st, data, _, _ = s.scsi(bytes([0x12, 1, 0x00, 0, 255, 0]), read_len=255)
    ctx.check(st == 0 and list(data[4:4 + data[3]]) == [0x00, 0x80, 0x83], "VPD: supported pages")
    st, data, _, _ = s.scsi(bytes([0x12, 1, 0x80, 0, 255, 0]), read_len=255)
    ctx.check(st == 0 and len(data[4:]) == 8, "VPD: unit serial number")
    st, data, _, _ = s.scsi(bytes([0x12, 1, 0x83, 0, 255, 0]), read_len=255)
    ctx.check(st == 0 and TARGET.encode() in data and data[5] & 0x0F == 3, "VPD: device identification")
    st, data, _, _ = s.scsi(bytes([0x12, 0, 0, 0, 36, 0]), read_len=36)
    ctx.check(st == 0 and len(data) == 36, "INQUIRY is truncated to the allocation length")
    st, data, _, _ = s.scsi(bytes([0xA0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0]), read_len=256)
    ctx.check(st == 0 and struct.unpack(">I", data[0:4])[0] == 16, "REPORT LUNS: 2 LUNs")
    st, data, _, _ = s.scsi(bytes([0x25] + [0] * 9), read_len=8)
    last, bs = struct.unpack(">II", data)
    ctx.check(st == 0 and (last + 1) * bs == lun0_size and bs == 512, "READ CAPACITY (10)")
    st, size, _ = s.capacity(lun=1)
    ctx.check(st == 0 and size == lun1_size, "READ CAPACITY (16) on LUN 1")
    st, data, _, _ = s.scsi(bytes([0x1A, 0, 0x3F, 0, 255, 0]), read_len=255)
    ctx.check(st == 0 and data[0] + 1 == len(data) and data[3] == 8, "MODE SENSE (6), all pages")
    st, data, _, _ = s.scsi(bytes([0x1A, 0, 0x08, 0, 255, 0]), lun=1, read_len=255)
    ctx.check(st == 0 and data[2] & 0x80, "MODE SENSE (6) reports write protect on the read-only LUN")

    pattern = os.urandom(256 * 1024)
    lba = 100
    st, _, r2ts, _ = s.scsi(cdb10(0x2A, lba, len(pattern) // 512), write_data=pattern)
    ctx.check(st == 0 and r2ts == 4, f"WRITE (10) 256 KiB through R2Ts ({r2ts})")
    st, data, _, _ = s.scsi(cdb10(0x28, lba, len(pattern) // 512), read_len=len(pattern))
    ctx.check(st == 0 and data == pattern, "READ (10) 256 KiB matches (several Data-In PDUs)")
    st, data, _, _ = s.scsi(bytes([0x08, 0, 0, lba, 0, 0]), read_len=256 * 512)
    ctx.check(st == 0 and data == pattern[:256 * 512], "READ (6) with length 0 reads 256 blocks")
    st, data, _, rb = s.scsi(bytes([0x08, 0, 0, lba, 1, 0]), read_len=4096)
    ctx.check(st == 0 and len(data) == 512 and rb[1] & 0x02 and struct.unpack(">I", rb[44:48])[0] == 3584,
              "residual underflow is reported")
    st, data, _, _ = s.scsi(cdb16(0x88, lun0_size // 512, 1), read_len=512)
    ctx.check(st == 2 and sense_key(data) == 5 and asc(data) == 0x21, "READ past the end -> LBA out of range")
    st, data, _, _ = s.scsi(cdb10(0x2A, 0, 1), lun=1, write_data=bytes(512))
    ctx.check(st == 2 and sense_key(data) == 7, "WRITE to the read-only LUN -> DATA PROTECT")
    st, data, _, _ = s.scsi(bytes(6), lun=5)
    ctx.check(st == 2 and sense_key(data) == 5 and asc(data) == 0x25, "command on a missing LUN -> invalid LUN")
    st, data, _, _ = s.scsi(bytes([0xFF] + [0] * 5))
    ctx.check(st == 2 and asc(data) == 0x20, "unknown SCSI opcode -> invalid command")
    st, _, _, _ = s.scsi(bytes([0x35] + [0] * 9))
    ctx.check(st == 0, "SYNCHRONIZE CACHE (10)")
    rbhs, data, itt = s.nop(b"hello")
    ctx.check(rbhs[0] & 0x3F == 0x20 and data == b"hello" and struct.unpack(">I", rbhs[16:20])[0] == itt,
              "NOP-Out ping is echoed")
    rbhs, data = s.raw_request(0x42, 0x81)  # task management: abort task
    ctx.check(rbhs[0] & 0x3F == 0x3F and rbhs[2] == 0x05 and data[:1] == b"\x42", "task management is rejected")

    itts = [s.send_command_only(cdb10(0x28, lba + i * 8, 8), 4096) for i in range(8)]
    ok = True
    for i, itt in enumerate(itts):
        st, data, _, _ = s.collect(itt)
        ok = ok and st == 0 and data == pattern[i * 4096:(i + 1) * 4096]
    ctx.check(ok, "8 pipelined READs complete with the right data")
    code, closed = s.logout()
    ctx.check(code == 0 and closed, "logout closes the session")

    # Session B: InitialR2T=No, ImmediateData=Yes (unsolicited data)
    s = ctx.session(isid=0x23D000000002)
    status, tsih, _ = s.full_login(TARGET, [
        ("MaxRecvDataSegmentLength", "16384"), ("InitialR2T", "No"), ("ImmediateData", "Yes"),
        ("MaxBurstLength", "262144"), ("FirstBurstLength", "65536"), ("MaxOutstandingR2T", "1")])
    ctx.check(status == 0 and s.params.get("InitialR2T") == "No" and s.params.get("ImmediateData") == "Yes",
              "login with InitialR2T=No, ImmediateData=Yes")
    big = os.urandom(512 * 1024)
    st, _, r2ts, _ = s.scsi(cdb10(0x2A, 4096, len(big) // 512), write_data=big)
    ctx.check(st == 0 and r2ts >= 1, f"WRITE 512 KiB: immediate + unsolicited + R2T ({r2ts} R2Ts)")
    small = os.urandom(4096)
    st, _, r2ts, _ = s.scsi(cdb10(0x2A, 8192, 8), write_data=small)
    ctx.check(st == 0 and r2ts == 0, "small WRITE entirely in immediate data")
    st, data, _, _ = s.scsi(cdb10(0x28, 4096, len(big) // 512), read_len=len(big))
    ctx.check(st == 0 and data == big, "READ back 512 KiB in 16 KiB Data-In PDUs")
    st, data, _, _ = s.scsi(cdb10(0x28, 8192, 8), read_len=4096)
    ctx.check(st == 0 and data == small, "READ back the small write")

    # Session reinstatement
    s2 = ctx.session(isid=0x23D000000002)
    status, tsih2, _ = s2.full_login(TARGET, [("MaxRecvDataSegmentLength", "65536")])
    ctx.check(status == 0 and tsih2 != tsih, "session reinstatement with the same ISID")
    ctx.check(s.is_closed_by_target(), "the old session's connection is closed")
    st, data, _, _ = s2.scsi(cdb10(0x28, 8192, 8), read_len=4096)
    ctx.check(st == 0 and data == small, "data persists across sessions")
    code, closed = s2.logout()
    ctx.check(code == 0 and closed, "logout")


def test_exclusive_image(ctx):
    image = ctx.make_image("exclusive.img", 1024 * 1024)
    with Server(ctx, ["--image", image]):
        result = subprocess.run([ctx.exe, "--port", str(ctx.port + 1), "--image", image, "--lang", "en-US"],
                                capture_output=True, timeout=10)
        ctx.check(result.returncode == 2 and b"(32)" in result.stderr,
                  "a read-write image cannot be opened by a second instance")


def test_stop_with_sessions(ctx):
    server = Server(ctx, ["--ram", "1M"])
    s = ctx.session()
    status, _, _ = s.full_login(DEFAULT_TARGET)
    ctx.check(status == 0, "login to the default target name")
    idle = socket.create_connection((HOST, ctx.port))  # connected, never logs in
    time.sleep(0.3)
    ok, seconds = server.stop()
    ctx.check(ok and seconds < 5, f"server stops with live connections ({seconds:.2f}s)")
    ctx.check(s.is_closed_by_target(), "logged-in connection is closed on stop")
    s.close()
    idle.close()


# ---- Disk image formats ----------------------------------------------------

IMAGE_SIZE = 16 * 1024 * 1024
MiB = 1024 * 1024


def find_tool(env, candidates):
    path = os.environ.get(env)
    if path:
        return path if os.path.exists(path) else None
    return next((c for c in candidates if os.path.exists(c)), None)


QEMU_IMG = find_tool("TISCSI_QEMU_IMG", [r"C:\Program Files\qemu\qemu-img.exe"])
VDISKMANAGER = find_tool("TISCSI_VDISKMANAGER", [
    r"C:\Program Files\VMware\VMware Workstation\vmware-vdiskmanager.exe",
    r"C:\Program Files (x86)\VMware\VMware Workstation\vmware-vdiskmanager.exe"])


def reference_content():
    """16 MiB with data at the start and in the middle, zeros elsewhere."""
    data = bytearray(IMAGE_SIZE)
    data[0:MiB] = os.urandom(MiB)
    data[5 * MiB:5 * MiB + MiB // 2] = os.urandom(MiB // 2)
    return bytes(data)


def write_plan():
    """Writes that hit allocated and unallocated blocks/grains, block
    boundaries and the last sectors."""
    return [
        (64, os.urandom(64 * 1024)),
        (int(1.75 * MiB) // 512, os.urandom(int(1.5 * MiB))),
        (10 * MiB // 512, os.urandom(4096)),
        (IMAGE_SIZE // 512 - 8, os.urandom(4096)),
    ]


def apply_writes(content, writes):
    out = bytearray(content)
    for lba, data in writes:
        out[lba * 512:lba * 512 + len(data)] = data
    return bytes(out)


def run_tool(*args):
    result = subprocess.run(list(args), capture_output=True, timeout=120)
    return result.returncode, (result.stdout + result.stderr).decode("utf-8", "replace")


def qemu_read(path, fmt):
    raw = path + ".out.raw"
    if os.path.exists(raw):
        os.remove(raw)
    rc, out = run_tool(QEMU_IMG, "convert", "-f", fmt, "-O", "raw", path, raw)
    if rc != 0:
        raise RuntimeError(out)
    with open(raw, "rb") as f:
        return f.read()


def read_all(s, lun, size=IMAGE_SIZE):
    data = bytearray()
    for offset in range(0, size, MiB):
        st, chunk, _, _ = s.read(offset // 512, min(MiB, size - offset) // 512, lun=lun)
        if st != 0:
            return None
        data += chunk
    return bytes(data)


def serve_and_check(ctx, images, expected_before, writes):
    """Serves `images` (list of (label, path)) read-write as LUNs of one
    target; checks capacity and content, applies `writes`, reads back."""
    args = ["--target", TARGET]
    for _, path in images:
        args += ["--image", path]
    expected_after = apply_writes(expected_before, writes)
    with Server(ctx, args):
        s = ctx.session()
        s.full_login(TARGET, [("MaxRecvDataSegmentLength", "262144")])
        for lun, (label, _) in enumerate(images):
            st, size, _ = s.capacity(lun)
            ctx.check(st == 0 and size == IMAGE_SIZE, f"{label}: capacity {size}")
            ctx.check(read_all(s, lun) == expected_before, f"{label}: content read over iSCSI")
            ok = all(s.write(lba, data, lun=lun)[0] == 0 for lba, data in writes)
            ctx.check(ok and read_all(s, lun) == expected_after, f"{label}: writes read back over iSCSI")
        s.logout()
    return expected_after


def test_image_formats_qemu(ctx):
    if not QEMU_IMG:
        print("  SKIP qemu-img not found (set TISCSI_QEMU_IMG)")
        return
    ref = reference_content()
    ref_path = ctx.path("ref.raw")
    with open(ref_path, "wb") as f:
        f.write(ref)
    formats = [
        ("VHD fixed (qemu)", "vpc", "subformat=fixed,force_size=on", "q-fixed.vhd"),
        ("VHD dynamic (qemu)", "vpc", "subformat=dynamic,force_size=on", "q-dynamic.vhd"),
        ("VMDK monolithicSparse (qemu)", "vmdk", "subformat=monolithicSparse", "q-msparse.vmdk"),
        ("VMDK monolithicFlat (qemu)", "vmdk", "subformat=monolithicFlat", "q-mflat.vmdk"),
        ("VMDK twoGbMaxExtentSparse (qemu)", "vmdk", "subformat=twoGbMaxExtentSparse", "q-2gsparse.vmdk"),
        ("VMDK twoGbMaxExtentFlat (qemu)", "vmdk", "subformat=twoGbMaxExtentFlat", "q-2gflat.vmdk"),
    ]
    images = []
    for label, fmt, options, name in formats:
        rc, out = run_tool(QEMU_IMG, "convert", "-f", "raw", "-O", fmt, "-o", options, ref_path, ctx.path(name))
        ctx.check(rc == 0, f"qemu-img creates {label}")
        images.append((label, ctx.path(name)))

    writes = write_plan()
    expected = serve_and_check(ctx, images, ref, writes)
    for (label, fmt, _, name) in formats:
        ctx.check(qemu_read(ctx.path(name), fmt) == expected, f"{label}: qemu-img reads our writes back")
        if fmt == "vmdk":
            rc, out = run_tool(QEMU_IMG, "check", "-f", "vmdk", ctx.path(name))
            ctx.check(rc == 0, f"{label}: qemu-img check reports no errors")

    # VMDK delta disk on top of a sparse base
    base = ctx.path("q-base.vmdk")
    child = ctx.path("q-child.vmdk")
    run_tool(QEMU_IMG, "convert", "-f", "raw", "-O", "vmdk", ref_path, base)
    rc, out = run_tool(QEMU_IMG, "create", "-f", "vmdk", "-b", "q-base.vmdk", "-F", "vmdk", child)
    ctx.check(rc == 0, "qemu-img creates a VMDK delta disk")
    expected = serve_and_check(ctx, [("VMDK delta (qemu)", child)], ref, write_plan())
    ctx.check(qemu_read(child, "vmdk") == expected, "VMDK delta: qemu-img reads child + parent")
    ctx.check(qemu_read(base, "vmdk") == ref, "VMDK delta: parent is unchanged")

    # Compressed streamOptimized images are not supported
    stream = ctx.path("q-stream.vmdk")
    run_tool(QEMU_IMG, "convert", "-f", "raw", "-O", "vmdk", "-o", "subformat=streamOptimized", ref_path, stream)
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--port", str(ctx.port), "--image-ro", stream)
    ctx.check(rc == 2 and "not supported" in out, "streamOptimized VMDK is refused as unsupported")


def test_create_vhd(ctx):
    for kind, extra in (("fixed", []), ("dynamic", ["--dynamic"])):
        path = ctx.path(f"new-{kind}.vhd")
        rc, out = run_tool(ctx.exe, "--lang", "en-US", "--create-vhd", path, "--size", "16M", *extra)
        ctx.check(rc == 0 and os.path.exists(path), f"--create-vhd creates a {kind} VHD")
        footer = vhd_tool.read_footer(path)
        ctx.check(footer["current_size"] == IMAGE_SIZE and footer["disk_type"] == (2 if kind == "fixed" else 3),
                  f"{kind} VHD footer: size and type")
        if kind == "dynamic":
            ctx.check(os.path.getsize(path) < 64 * 1024, "dynamic VHD starts small")
        expected = serve_and_check(ctx, [(f"created {kind} VHD", path)], bytes(IMAGE_SIZE), write_plan())
        ctx.check(vhd_tool.read_disk(path) == expected, f"created {kind} VHD: spec reader sees our writes")
        if QEMU_IMG:
            rc, out = run_tool(QEMU_IMG, "info", "-f", "vpc", path)
            ctx.check(rc == 0 and "virtual size: 16 MiB" in out, f"created {kind} VHD: qemu-img info")
            ctx.check(qemu_read(path, "vpc") == expected, f"created {kind} VHD: qemu-img reads our writes")

    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--create-vhd", ctx.path("new-fixed.vhd"), "--size", "16M")
    ctx.check(rc == 2 and "Cannot create" in out, "--create-vhd refuses to overwrite a file")
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--create-vhd", ctx.path("x.vhd"), "--size", "1000")
    ctx.check(rc == 1 and "Invalid VHD size" in out, "--size must be a multiple of 512")
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--create-vhd", ctx.path("x.vhd"))
    ctx.check(rc == 1 and "requires --size" in out, "--create-vhd requires --size")


def test_differencing_vhd(ctx):
    if not QEMU_IMG:
        print("  SKIP qemu-img not found (set TISCSI_QEMU_IMG)")
        return
    ref = reference_content()
    ref_path = ctx.path("diff-ref.raw")
    with open(ref_path, "wb") as f:
        f.write(ref)
    parent = ctx.path("parent.vhd")
    run_tool(QEMU_IMG, "convert", "-f", "raw", "-O", "vpc", "-o", "subformat=dynamic,force_size=on", ref_path, parent)
    child = ctx.path("child.vhd")
    vhd_tool.create_differencing(child, parent)
    expected = serve_and_check(ctx, [("differencing VHD", child)], ref, write_plan())
    ctx.check(vhd_tool.read_disk(child) == expected, "differencing VHD: spec reader sees child + parent")
    ctx.check(qemu_read(parent, "vpc") == ref, "differencing VHD: parent is unchanged")

    wrong = ctx.path("wrong-id.vhd")
    vhd_tool.create_differencing(wrong, parent, parent_id=bytes(16))
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--port", str(ctx.port), "--image-ro", wrong)
    ctx.check(rc == 2 and "parent.vhd" in out, "differencing VHD with a mismatched parent ID is refused")
    orphan = ctx.path("orphan.vhd")
    vhd_tool.create_differencing(orphan, parent)
    os.rename(parent, parent + ".moved")
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--port", str(ctx.port), "--image-ro", orphan)
    ctx.check(rc == 2 and "parent.vhd" in out, "missing parent is reported with its path")
    os.rename(parent + ".moved", parent)


def test_image_formats_vmware(ctx):
    if not VDISKMANAGER:
        print("  SKIP vmware-vdiskmanager not found (set TISCSI_VDISKMANAGER)")
        return
    types = [
        ("VMDK single growable (VMware)", 0, True),
        ("VMDK split growable (VMware)", 1, True),
        ("VMDK preallocated (VMware)", 2, False),
        ("VMDK split preallocated (VMware)", 3, False),
        ("VMDK preallocated ESX (VMware)", 4, False),
        # type 6 (thin ESX) can only be created on a remote ESX host
    ]
    images = []
    for label, disk_type, _ in types:
        path = ctx.path(f"vmw-{disk_type}.vmdk")
        rc, out = run_tool(VDISKMANAGER, "-q", "-c", "-s", "16MB", "-a", "lsilogic", "-t", str(disk_type), path)
        ctx.check(rc == 0, f"vmware-vdiskmanager creates {label}")
        images.append((label, path))
    expected = serve_and_check(ctx, images, bytes(IMAGE_SIZE), write_plan())
    for (label, disk_type, sparse), (_, path) in zip(types, images):
        if sparse:
            rc, out = run_tool(VDISKMANAGER, "-R", path)
            ctx.check(rc == 0, f"{label}: vmware-vdiskmanager -R finds no inconsistencies")
        flat = ctx.path(f"vmw-{disk_type}-conv.vmdk")
        rc, out = run_tool(VDISKMANAGER, "-q", "-r", path, "-t", "2", flat)
        converted = b""
        if rc == 0:
            with open(ctx.path(f"vmw-{disk_type}-conv-flat.vmdk"), "rb") as f:
                converted = f.read()
        ctx.check(converted == expected, f"{label}: VMware reads our writes back")

    stream = ctx.path("vmw-5.vmdk")
    run_tool(VDISKMANAGER, "-q", "-c", "-s", "16MB", "-a", "lsilogic", "-t", "5", stream)
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--port", str(ctx.port), "--image-ro", stream)
    ctx.check(rc == 2 and "not supported" in out, "VMware streamOptimized VMDK is refused as unsupported")


# ---- Optical disc images (.iso) ---------------------------------------------

CD_SECTORS = 1000
DVD_SECTORS = 80 * 60 * 75 + 1  # one sector more than an 80-minute CD


def make_sparse(path, size):
    """Makes `path` a sparse file of `size` bytes, creating it if needed (no
    administrator rights needed)."""
    import ctypes
    import msvcrt
    from ctypes import wintypes
    with open(path, "r+b" if os.path.exists(path) else "wb") as f:
        handle = msvcrt.get_osfhandle(f.fileno())
        returned = wintypes.DWORD()
        FSCTL_SET_SPARSE = 0x900C4
        ctypes.windll.kernel32.DeviceIoControl(wintypes.HANDLE(handle), FSCTL_SET_SPARSE, None, 0, None, 0,
                                               ctypes.byref(returned), None)
        f.truncate(size)
    return path


def make_iso(ctx, name, sectors):
    """Image of `sectors` 2048-byte sectors with an ISO 9660 volume descriptor
    at sector 16, plus a partial trailing sector that must be ignored."""
    content = bytearray(os.urandom(sectors * 2048 + 100))
    content[16 * 2048:16 * 2048 + 6] = b"\x01CD001"
    path = ctx.path(name)
    with open(path, "wb") as f:
        f.write(content)
    return path, bytes(content[:sectors * 2048])


def msf(lba):
    frames = lba + 150
    return bytes([frames // 4500, frames // 75 % 60, frames % 75])


def mmc_cdb(opcode, byte1=0, byte2=0, byte6=0, alloc=0xFFFF, byte9=0):
    return bytes([opcode, byte1, byte2, 0, 0, 0, byte6]) + struct.pack(">H", alloc) + bytes([byte9])


def feature_codes(data):
    codes = []
    pos = 8
    while pos + 4 <= len(data):
        codes.append(struct.unpack(">H", data[pos:pos + 2])[0])
        pos += 4 + data[pos + 3]
    return codes


def mode_pages(data, header_length):
    """Page codes of a MODE SENSE response after its header and block descriptors."""
    if header_length == 4:
        pos = 4 + data[3]
    else:
        pos = 8 + struct.unpack(">H", data[6:8])[0]
    pages = []
    while pos + 2 <= len(data):
        pages.append(data[pos] & 0x3F)
        pos += 2 + data[pos + 1]
    return pages


def test_optical_images(ctx):
    iso, content = make_iso(ctx, "disc.iso", CD_SECTORS)
    dvd = make_sparse(ctx.path("dvd.ISO"), DVD_SECTORS * 2048)
    # --image (read-write) still serves an .iso read-only.
    server = Server(ctx, ["--target", TARGET, "--ram", "1M", "--image", iso, "--image-ro", dvd])
    with server:
        s = ctx.session()
        status, _, _ = s.full_login(TARGET, [("MaxRecvDataSegmentLength", "65536")])
        ctx.check(status == 0, "login to a target with CD and DVD images")

        st, data, _, _ = s.scsi(bytes([0x12, 0, 0, 0, 96, 0]), lun=1, read_len=96)
        ctx.check(st == 0 and data[0] & 0x1F == 5 and data[1] & 0x80 and data[16:30] == b"SCSI DVD-ROM 1",
                  "INQUIRY: CD/DVD-ROM device, removable medium")
        st, data, _, _ = s.scsi(bytes([0x12, 0, 0, 0, 96, 0]), lun=0, read_len=96)
        ctx.check(st == 0 and data[0] & 0x1F == 0 and not data[1] & 0x80, "INQUIRY: the RAM disk stays a disk")
        st, data, _, _ = s.scsi(bytes([0x12, 1, 0xB0, 0, 255, 0]), lun=1, read_len=255)
        ctx.check(st == 2 and asc(data) == 0x24, "VPD Block Limits is not provided by a CD/DVD-ROM drive")

        st, data, _, _ = s.scsi(bytes([0x25] + [0] * 9), lun=1, read_len=8)
        last, bs = struct.unpack(">II", data)
        ctx.check(st == 0 and last + 1 == CD_SECTORS and bs == 2048,
                  "READ CAPACITY (10): 2048-byte sectors, partial sector ignored")
        st, data, _, _ = s.scsi(cdb10(0x28, 16, 1), lun=1, read_len=2048)
        ctx.check(st == 0 and data[1:6] == b"CD001" and data == content[16 * 2048:17 * 2048],
                  "READ (10) returns the ISO 9660 volume descriptor")
        cdb12 = struct.pack(">BBIIBB", 0xA8, 0, 900, 100, 0, 0)
        st, data, _, _ = s.scsi(cdb12, lun=1, read_len=100 * 2048)
        ctx.check(st == 0 and data == content[900 * 2048:], "READ (12) up to the last sector")
        st, data, _, _ = s.scsi(cdb10(0x28, CD_SECTORS, 1), lun=1, read_len=2048)
        ctx.check(st == 2 and asc(data) == 0x21, "READ past the end of the disc -> LBA out of range")
        st, data, _, _ = s.scsi(cdb10(0x2A, 0, 1), lun=1, write_data=bytes(2048))
        ctx.check(st == 2 and sense_key(data) == 7, "WRITE -> DATA PROTECT")

        st, data, _, _ = s.scsi(mmc_cdb(0x43), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and data == bytes([0, 18, 1, 1, 0, 0x14, 1, 0, 0, 0, 0, 0, 0, 0x14, 0xAA, 0]) +
                  struct.pack(">I", CD_SECTORS), "READ TOC: track 1 and lead-out (LBA)")
        st, data, _, _ = s.scsi(mmc_cdb(0x43, byte1=0x02), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and data[8:12] == b"\0" + msf(0) and data[16:20] == b"\0" + msf(CD_SECTORS),
                  "READ TOC: MSF addresses")
        st, data, _, _ = s.scsi(mmc_cdb(0x43, byte6=0xAA), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and len(data) == 12 and data[6] == 0xAA, "READ TOC from the lead-out")
        st, data, _, _ = s.scsi(mmc_cdb(0x43, byte2=1), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and data[:4] == bytes([0, 10, 1, 1]) and data[6] == 1, "READ TOC: session information")
        st, data, _, _ = s.scsi(mmc_cdb(0x43, byte9=0x40), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and data[:2] == bytes([0, 10]), "READ TOC: format in the old position (byte 9)")
        st, data, _, _ = s.scsi(mmc_cdb(0x43, byte2=2), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and data[:2] == bytes([0, 46]) and data[4 + 3::11][:4] == bytes([0xA0, 0xA1, 0xA2, 1]) and
                  data[4 + 22 + 8:4 + 22 + 11] == msf(CD_SECTORS), "READ TOC: raw TOC")
        st, data, _, _ = s.scsi(mmc_cdb(0x43, byte2=4), lun=1, read_len=0xFFFF)
        ctx.check(st == 2 and asc(data) == 0x24, "READ TOC: ATIP -> invalid field")
        st, data, _, _ = s.scsi(mmc_cdb(0x43, byte6=2), lun=1, read_len=0xFFFF)
        ctx.check(st == 2 and asc(data) == 0x24, "READ TOC: missing track -> invalid field")
        st, data, _, _ = s.scsi(mmc_cdb(0x43, alloc=4), lun=1, read_len=4)
        ctx.check(st == 0 and data == bytes([0, 18, 1, 1]), "READ TOC is truncated to the allocation length")

        st, data, _, _ = s.scsi(mmc_cdb(0x46), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and struct.unpack(">IH", data[0:4] + data[6:8]) == (len(data) - 4, 0x0008) and
                  feature_codes(data) == [0x00, 0x01, 0x03, 0x10, 0x1E, 0x1F], "GET CONFIGURATION: all features, CD")
        ctx.check(data[12:14] == b"\x00\x10" and data[14] == 0 and data[16:18] == b"\x00\x08" and data[18] == 1,
                  "GET CONFIGURATION: profile list (DVD-ROM, CD-ROM current)")
        st, data, _, _ = s.scsi(mmc_cdb(0x46, byte1=1), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and feature_codes(data) == [0x00, 0x01, 0x03, 0x10, 0x1E],
                  "GET CONFIGURATION: current features")
        st, data, _, _ = s.scsi(bytes([0x46, 2, 0, 0x10, 0, 0, 0, 0xFF, 0xFF, 0]), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and feature_codes(data) == [0x10] and data[12:16] == struct.pack(">I", 2048),
                  "GET CONFIGURATION: one feature (Random Readable)")
        st, data, _, _ = s.scsi(bytes([0x46, 0, 0, 0x04, 0, 0, 0, 0xFF, 0xFF, 0]), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and feature_codes(data) == [0x10, 0x1E, 0x1F], "GET CONFIGURATION: starting feature")
        st, data, _, _ = s.scsi(mmc_cdb(0x46), lun=2, read_len=0xFFFF)
        ctx.check(st == 0 and data[6:8] == b"\x00\x10", "GET CONFIGURATION: a larger image is a DVD")

        st, data, _, _ = s.scsi(bytes([0x4A, 1, 0, 0, 0x10, 0, 0, 0, 8, 0]), lun=1, read_len=8)
        ctx.check(st == 0 and data == bytes([0, 4, 4, 0x10, 0, 2, 0, 0]), "GET EVENT STATUS NOTIFICATION: media present")
        st, data, _, _ = s.scsi(bytes([0x4A, 1, 0, 0, 0x02, 0, 0, 0, 8, 0]), lun=1, read_len=8)
        ctx.check(st == 0 and data == bytes([0, 0, 0x80, 0x10]), "GET EVENT STATUS NOTIFICATION: unsupported class")
        st, data, _, _ = s.scsi(bytes([0x4A, 0, 0, 0, 0x10, 0, 0, 0, 8, 0]), lun=1, read_len=8)
        ctx.check(st == 2 and asc(data) == 0x24, "GET EVENT STATUS NOTIFICATION: asynchronous -> invalid field")

        st, data, _, _ = s.scsi(mmc_cdb(0x5A, byte2=0x2A), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and struct.unpack(">H", data[0:2])[0] + 2 == len(data) and data[6:8] == b"\0\0" and
                  data[8] == 0x2A and data[3] == 0, "MODE SENSE (10): capabilities page, no block descriptor")
        st, data, _, _ = s.scsi(mmc_cdb(0x5A, byte2=0x3F), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and mode_pages(data, 8) == [0x01, 0x2A], "MODE SENSE (10): all pages")
        st, data, _, _ = s.scsi(bytes([0x1A, 0, 0x3F, 0, 255, 0]), lun=1, read_len=255)
        ctx.check(st == 0 and data[0] + 1 == len(data) and mode_pages(data, 4) == [0x01, 0x2A],
                  "MODE SENSE (6): all pages")
        st, data, _, _ = s.scsi(mmc_cdb(0x5A, byte2=0x08), lun=1, read_len=0xFFFF)
        ctx.check(st == 2 and asc(data) == 0x24, "MODE SENSE: caching page -> invalid field")
        st, data, _, _ = s.scsi(mmc_cdb(0x5A, byte2=0x3F), lun=0, read_len=0xFFFF)
        ctx.check(st == 0 and struct.unpack(">H", data[6:8])[0] == 8 and mode_pages(data, 8) == [0x08, 0x1C],
                  "MODE SENSE (10) on a disk: block descriptor and pages")

        st, data, _, _ = s.scsi(mmc_cdb(0x51), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and len(data) == 34 and data[2] == 0x0E and data[4] == 1, "READ DISC INFORMATION")
        st, data, _, _ = s.scsi(bytes([0x52, 1, 0, 0, 0, 1, 0, 0xFF, 0xFF, 0]), lun=1, read_len=0xFFFF)
        ctx.check(st == 0 and data[2] == 1 and struct.unpack(">I", data[24:28])[0] == CD_SECTORS,
                  "READ TRACK INFORMATION: track 1")
        st, data, _, _ = s.scsi(bytes([0x52, 0, 0, 0, 0x03, 0xE8, 0, 0xFF, 0xFF, 0]), lun=1, read_len=0xFFFF)
        ctx.check(st == 2 and asc(data) == 0x24, "READ TRACK INFORMATION: LBA past the end -> invalid field")

        dvd_structure = bytes([0xAD, 0, 0, 0, 0, 0, 0, 0x00, 0x08, 0x04, 0, 0])
        st, data, _, _ = s.scsi(dvd_structure, lun=1, read_len=2052)
        ctx.check(st == 2 and sense_key(data) == 5 and asc(data) == 0x30, "READ DVD STRUCTURE on a CD -> incompatible")
        st, data, _, _ = s.scsi(dvd_structure, lun=2, read_len=2052)
        ctx.check(st == 0 and len(data) == 2052 and data[4] == 0x01 and
                  struct.unpack(">II", data[8:16]) == (0x30000, 0x30000 + DVD_SECTORS - 1),
                  "READ DVD STRUCTURE: physical format of a DVD")
        st, data, _, _ = s.scsi(dvd_structure[:7] + b"\x01" + dvd_structure[8:], lun=2, read_len=2052)
        ctx.check(st == 0 and data == bytes([0, 6, 0, 0, 0, 0, 0, 0]), "READ DVD STRUCTURE: no copy protection")

        st, data, _, _ = s.scsi(bytes([0xBD, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0]), lun=1, read_len=8)
        ctx.check(st == 0 and len(data) == 8, "MECHANISM STATUS")
        st, _, _, _ = s.scsi(bytes([0x1E, 0, 0, 0, 1, 0]), lun=1)
        st2, _, _, _ = s.scsi(bytes([0x1B, 0, 0, 0, 2, 0]), lun=1)
        ctx.check(st == 0 and st2 == 0, "PREVENT ALLOW MEDIUM REMOVAL and eject are accepted")
        st, data, _, _ = s.scsi(cdb10(0x28, 16, 1), lun=1, read_len=2048)
        ctx.check(st == 0 and data[1:6] == b"CD001", "the disc is still readable after an eject request")
        st, data, _, _ = s.scsi(mmc_cdb(0x43), lun=0, read_len=0xFFFF)
        ctx.check(st == 2 and asc(data) == 0x20, "READ TOC on a disk -> invalid command")
        s.logout()
    ctx.check(re.search(r"LUN 1: .*disc\.iso, .*\(read-only\)", server.output) is not None,
              "the .iso LUN is listed as read-only")

    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--port", str(ctx.port), "--image",
                       make_sparse(ctx.path("tiny.iso"), 2000))
    ctx.check(rc == 2 and "Cannot open" in out, "an .iso smaller than one sector is refused")

    # --cdrom serves any file as a disc: no VHD parsing for a .vhd name.
    bin_path, bin_content = make_iso(ctx, "disc.bin", 200)
    vhd_path, vhd_content = make_iso(ctx, "not-a-disk.vhd", 300)
    server = Server(ctx, ["--target", TARGET, "--cdrom", bin_path, "--cdrom=" + vhd_path, "--image-ro", bin_path])
    with server:
        s = ctx.session()
        status, _, _ = s.full_login(TARGET)
        ok = status == 0
        for lun, sectors, content in ((0, 200, bin_content), (1, 300, vhd_content)):
            st, data, _, _ = s.scsi(bytes([0x12, 0, 0, 0, 96, 0]), lun=lun, read_len=96)
            ok = ok and st == 0 and data[0] & 0x1F == 5
            st, data, _, _ = s.scsi(bytes([0x25] + [0] * 9), lun=lun, read_len=8)
            ok = ok and st == 0 and struct.unpack(">II", data) == (sectors - 1, 2048)
            st, data, _, _ = s.scsi(cdb10(0x28, sectors - 10, 10), lun=lun, read_len=10 * 2048)
            ok = ok and st == 0 and data == content[-10 * 2048:]
        ctx.check(ok, "--cdrom serves a .bin and a .vhd file as CD/DVD-ROM drives")
        st, data, _, _ = s.scsi(bytes([0x12, 0, 0, 0, 96, 0]), lun=2, read_len=96)
        st2, data2, _, _ = s.scsi(bytes([0x25] + [0] * 9), lun=2, read_len=8)
        ctx.check(st == 0 and data[0] & 0x1F == 0 and st2 == 0 and data2[4:8] == b"\0\0\x02\0",
                  "the same file with --image-ro is a disk")
        s.logout()
    ctx.check(re.search(r"LUN 1: .*not-a-disk\.vhd, .*\(read-only\)", server.output) is not None,
              "the --cdrom LUN is listed as read-only")
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--cdrom")
    ctx.check(rc == 1 and "--cdrom" in out, "--cdrom needs a path")


def powershell(script, timeout=180):
    command = "[Console]::OutputEncoding = [Text.Encoding]::UTF8; $ErrorActionPreference = 'Stop'; " + script
    result = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", command],
                            capture_output=True, timeout=timeout)
    return result.returncode, (result.stdout + result.stderr).decode("utf-8", "replace").strip()


def find_cd_volume(label, timeout=30):
    """Drive letter ("E:") of the CD/DVD volume with this label, or None."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        _, out = powershell("(Get-CimInstance Win32_LogicalDisk -Filter \"DriveType=5 AND VolumeName='"
                            + label + "'\").DeviceID")
        if re.fullmatch(r"[A-Z]:", out):
            return out
        time.sleep(1)
    return None


# ---- Configuration files ------------------------------------------------------

def run_cli(ctx, *args, cwd=None):
    result = subprocess.run([ctx.exe, "--lang", "en-US", *args], capture_output=True, timeout=60, cwd=cwd)
    return result.returncode, (result.stdout + result.stderr).decode("utf-8", "replace")


def test_config_files(ctx):
    folder = ctx.path("config")
    os.makedirs(os.path.join(folder, "images"), exist_ok=True)
    first = os.urandom(512)
    ctx.make_image(os.path.join("config", "images", "a b.img"), 4 * MiB, first)
    other = os.urandom(512)
    ctx.make_image(os.path.join("config", "images", "磁盘.img"), 2 * MiB, other)
    a_target, b_target = "iqn.2026-10.test:config-a", "iqn.2026-10.test:config-b"

    # Hand-written: comments, odd spacing and case, quotes, relative paths.
    conf = os.path.join(folder, "server.conf")
    with open(conf, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(f"# comment\n; another\n[SERVER]\nport=1\n  verbosity = warning\n\n"
                f"[target {a_target}]\nImage = \"images\\a b.img\"\nram = 1M\n"
                f"[target {b_target}]\nimage-ro = images\\磁盘.img\n")
    # --listen / --port given by Server override the file's.
    with Server(ctx, ["--config", conf]) as server:
        s = ctx.session()
        status, _, _ = s.full_login(a_target)
        st, size, _ = s.capacity()
        st2, data, _, _ = s.read(0, 1)
        st3, ram_size, _ = s.capacity(lun=1)
        ctx.check(status == 0 and size == 4 * MiB and data == first and ram_size == MiB,
                  "--config serves the file's targets (relative path with a space, RAM disk)")
        s.logout()
        s = ctx.session()
        status, _, _ = s.full_login(b_target)
        st, data, _, _ = s.read(0, 1)
        st2, _, _, _ = s.write(0, bytes(512))
        ctx.check(status == 0 and data == other and st2 == 2, "image-ro with a non-ASCII name is served read-only")
        s.logout()

    # Encodings: UTF-16 with BOM, and the ANSI code page if it has the name.
    text = f"[target {b_target}]\r\nimage = images\\磁盘.img\r\n"
    with open(os.path.join(folder, "utf16.conf"), "wb") as f:
        f.write(b"\xff\xfe" + text.encode("utf-16-le"))
    with Server(ctx, ["--config", os.path.join(folder, "utf16.conf")]):
        s = ctx.session()
        ctx.check(s.full_login(b_target)[0] == 0 and s.read(0, 1)[1] == other, "a UTF-16 configuration file is read")
        s.logout()
    try:
        ansi = text.encode("mbcs", "strict")
    except UnicodeEncodeError:
        ansi = None
    if ansi and ansi.decode("mbcs") == text:
        with open(os.path.join(folder, "ansi.conf"), "wb") as f:
            f.write(ansi)
        with Server(ctx, ["--config", os.path.join(folder, "ansi.conf")]):
            s = ctx.session()
            ctx.check(s.full_login(b_target)[0] == 0, "an ANSI configuration file is read")
            s.logout()

    # --save-config: relative to the file's folder, read back the same.
    saved = os.path.join(folder, "saved.conf")
    rc, out = run_cli(ctx, "--target", a_target, "--image", os.path.join(folder, "images", "a b.img"),
                      "--ram", "3M", "--target", b_target, "--cdrom", os.path.join(folder, "images", "磁盘.img"),
                      "--port", "3999", "--wait", "15", "--skip-unavailable", "--save-config", saved)
    content = open(saved, "rb").read() if os.path.exists(saved) else b""
    ctx.check(rc == 0 and content.startswith(b"\xef\xbb\xbf"), "--save-config writes UTF-8 with a BOM")
    text = content.decode("utf-8-sig").replace("\r\n", "\n")
    ctx.check("port = 3999\n" in text and "wait = 15\n" in text and "skip-unavailable = yes\n" in text and
              f"[target {a_target}]\nimage = images\\a b.img\nram = 3M\n" in text and
              f"[target {b_target}]\ncdrom = images\\磁盘.img\n" in text,
              "--save-config writes the options, with paths relative to the file")
    resaved = os.path.join(folder, "resaved.conf")
    rc, _ = run_cli(ctx, "--config", saved, "--save-config", resaved)
    ctx.check(rc == 0 and open(resaved, "rb").read() == content, "a saved configuration reads back the same")
    rc, out = run_cli(ctx, "--disk", "0", "--save-config", os.path.join(folder, "disk.conf"))
    disk_text = open(os.path.join(folder, "disk.conf"), encoding="utf-8-sig").read() if rc == 0 else ""
    ctx.check(rc == 0 and re.search(r"^disk = (serial:|gpt:\{|mbr:)", disk_text, re.M) is not None or
              "has no stable name" in out, "--save-config saves disk 0 by a stable name")

    # Errors name the file and line.
    for name, content, expected in (
            ("e1.conf", "[server]\nfoo = 1\n", "e1.conf, line 2: Unknown option: foo"),
            ("e2.conf", "# x\nimage = a.img\n", "e2.conf, line 2: image must be in a [target <name>] section"),
            ("e3.conf", f"[target {a_target}]\nport = 5\n", "e3.conf, line 2: port must be in the [server] section"),
            ("e4.conf", "[bogus]\n", "e4.conf, line 1: Unknown section: [bogus]"),
            ("e5.conf", "[server]\nport\n", "e5.conf, line 2: Not a setting or a section: port"),
            ("e6.conf", "[server]\nwait = soon\n", "e6.conf, line 2: Invalid waiting time: soon"),
            ("e7.conf", "[server]\nskip-unavailable = maybe\n", "Invalid value for skip-unavailable: maybe"),
            ("e8.conf", f"[target {a_target}]\n[target {a_target}]\n", "line 2: The target name is used more than once"),
            ("e9.conf", f"[target {a_target}]\n", f"e9.conf: Target {a_target} has no disks"),
            ("e10.conf", "[target not-an-iqn]\nram = 1M\n", "line 1: Invalid iSCSI target name: not-an-iqn"),
            ("e11.conf", "[server]\nport = 1\n", "No disks specified")):
        path = os.path.join(folder, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
        rc, out = run_cli(ctx, "--config", path)
        ctx.check(rc == 1 and expected in out, f"configuration error: {expected}")
    rc, out = run_cli(ctx, "--config", conf, "--ram", "1M")
    ctx.check(rc == 1 and "--config cannot be combined" in out, "--config cannot be combined with disks")
    rc, out = run_cli(ctx, "--config", os.path.join(folder, "missing.conf"))
    ctx.check(rc == 1 and "Cannot read the configuration file" in out, "a missing configuration file is reported")
    rc, out = run_cli(ctx, "--ram", "1M", "--wait", "100000")
    ctx.check(rc == 1 and "Invalid waiting time" in out, "--wait has a maximum")

    # --wait: an image that shows up later.
    late = os.path.join(folder, "late.img")
    process = subprocess.Popen([ctx.exe, "--lang", "en-US", "--listen", HOST, "--port", str(ctx.port),
                                "--target", a_target, "--image", late, "--wait", "20"],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    try:
        time.sleep(2)
        ctx.check(process.poll() is None, "--wait keeps waiting for a missing image")
        with open(late, "wb") as f:
            f.write(first)
            f.truncate(MiB)
        listening = False
        deadline = time.time() + 10
        while time.time() < deadline and not listening:
            try:
                socket.create_connection((HOST, ctx.port), timeout=1).close()
                listening = True
            except OSError:
                time.sleep(0.2)
        ctx.check(listening, "the server starts once the image is there")
        if listening:
            s = ctx.session()
            ctx.check(s.full_login(a_target)[0] == 0 and s.read(0, 1)[1] == first, "the late image is served")
            s.logout()
    finally:
        if process.poll() is None:
            os.kill(process.pid, signal.CTRL_BREAK_EVENT)
        out, _ = process.communicate(timeout=20)
    ctx.check(f"Waiting up to 20 s for the disks of {a_target}" in out.decode("utf-8", "replace"),
              "--wait says what it is waiting for")

    # --wait: an image another process has open (still copying it, say).
    import ctypes
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.restype = ctypes.c_void_p
    kernel32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                     ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    held = kernel32.CreateFileW(late, 0xC0000000, 0, None, 3, 0, None)  # read-write, not shared
    process = subprocess.Popen([ctx.exe, "--lang", "en-US", "--listen", HOST, "--port", str(ctx.port),
                                "--target", a_target, "--image", late, "--wait", "20"],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    try:
        time.sleep(2)
        ctx.check(process.poll() is None, "--wait waits for an image another process has open")
        kernel32.CloseHandle(held)
        listening = False
        deadline = time.time() + 10
        while time.time() < deadline and not listening:
            try:
                socket.create_connection((HOST, ctx.port), timeout=1).close()
                listening = True
            except OSError:
                time.sleep(0.2)
        ctx.check(listening, "the server starts once the other process closes the image")
    finally:
        if process.poll() is None:
            os.kill(process.pid, signal.CTRL_BREAK_EVENT)
        process.communicate(timeout=20)
    os.remove(late)
    start = time.time()
    rc, out = run_cli(ctx, "--port", str(ctx.port), "--image", late, "--wait", "2")
    ctx.check(rc == 2 and "Cannot open the disk image" in out and 1.5 < time.time() - start < 15,
              "--wait gives up after the time and reports the error")
    rc, out = run_cli(ctx, "--port", str(ctx.port), "--image", ctx.path("config"), "--wait", "30")
    ctx.check(rc == 2 and "Waiting" not in out, "--wait does not wait for errors that stay (a folder as an image)")

    # --skip-unavailable: the other targets are served.
    with Server(ctx, ["--target", a_target, "--image", late, "--target", b_target, "--ram", "1M",
                      "--skip-unavailable"]) as server:
        s = ctx.session()
        ctx.check(s.full_login(b_target)[0] == 0, "--skip-unavailable serves the targets that open")
        s.logout()
        s = ctx.session()
        ctx.check(s.full_login(a_target)[0] != 0, "the target that did not open is not served")
        s.close()
        server.stop()
    ctx.check(f"Target {a_target} is not served." in server.output, "--skip-unavailable names the skipped target")
    rc, out = run_cli(ctx, "--port", str(ctx.port), "--image", late, "--skip-unavailable")
    ctx.check(rc == 2 and "None of the targets can be served" in out, "--skip-unavailable with nothing left fails")


# ---- Access control ------------------------------------------------------------

def test_access(ctx):
    allowed, chap, oneway = "iqn.2026-10.test:allowed", "iqn.2026-10.test:chap", "iqn.2026-10.test:oneway"
    other = "iqn.2026-10.test:other-initiator"
    secret, mutual = "alice-secret-1", "target-secret-2"
    args = ["--target", allowed, "--allow-initiator", "IQN.2026-10.TEST:INITIATOR", "--ram", "1M",
            "--target", chap, "--chap-user", "alice", "--chap-secret", secret, "--mutual-chap-secret", mutual,
            "--ram", "1M",
            "--target", oneway, "--chap-secret", "oneway-secret", "--ram", "1M"]
    with Server(ctx, args) as server:
        s = ctx.session()
        ctx.check(s.full_login(allowed)[0] == 0, "an allowed initiator logs in (names compare without case)")
        s.logout()
        s = Session(HOST, ctx.port, initiator=other)
        ctx.check(s.full_login(allowed)[0] == 0x0202, "another initiator is refused (authorization failure)")
        s.close()
        s = Session(HOST, ctx.port, initiator=other)
        s.full_login()
        _, keys = s.text([("SendTargets", "All")])
        ctx.check(sorted(keys.get("TargetName", [])) == sorted([chap, oneway]),
                  "discovery hides targets the initiator may not log in to")
        s.logout()

        s = ctx.session()
        _, status, _, _ = s.login([("InitiatorName", s.initiator), ("SessionType", "Normal"), ("TargetName", chap),
                                   ("AuthMethod", "None")], 0, 1)
        ctx.check(status == 0x0201, "a CHAP target refuses AuthMethod=None (authentication failure)")
        s.close()
        s = ctx.session()
        ctx.check(s.full_login(chap)[0] == 0x0201, "a CHAP target refuses initiators that skip security negotiation")
        s.close()
        s = ctx.session()
        status, details = s.chap_login(chap, "alice", secret)
        st, size, _ = s.capacity() if status == 0 else (None, 0, 0)
        ctx.check(status == 0 and size == MiB, "the right CHAP name and secret log in")
        s.logout()
        for name, key, what in (("alice", "wrong-secret-0", "a wrong secret"), ("bob", secret, "a wrong CHAP name")):
            s = ctx.session()
            ctx.check(s.chap_login(chap, name, key)[0] == 0x0201, f"{what} is refused")
            s.close()
        s = ctx.session()
        status, details = s.chap_login(chap, "alice", secret, mutual_secret=mutual)
        ctx.check(status == 0 and details["mutual_ok"] and details["target_name"] == chap,
                  "mutual CHAP: the target proves it knows the mutual secret, with its name as CHAP_N")
        s.logout()
        s = ctx.session()
        ctx.check(s.chap_login(chap, "alice", secret, mutual_secret=mutual, reflect=True)[0] == 0x0201,
                  "mutual CHAP refuses the target's own challenge (reflection)")
        s.close()
        s = ctx.session()
        status, details = s.chap_login(chap, "alice", secret, algorithms="7")
        ctx.check(status == 0x0201 and details["keys"].get("CHAP_A") == ["Reject"],
                  "an unsupported CHAP algorithm is rejected")
        s.close()
        s = ctx.session()
        ctx.check(s.chap_login(oneway, "anyone", "oneway-secret", base64=True)[0] == 0,
                  "without chap-user any CHAP name works; base64 (0b) responses are accepted")
        s.logout()
        s = ctx.session()
        ctx.check(s.chap_login(oneway, "anyone", "oneway-secret", mutual_secret="x" * 12)[0] == 0x0201,
                  "mutual CHAP without a mutual secret on the target is refused")
        s.close()
        server.stop()
    ctx.check("Allowed initiators: IQN.2026-10.TEST:INITIATOR" in server.output and
              "CHAP authentication required; mutual CHAP available" in server.output and
              "CHAP authentication failed" in server.output and secret not in server.output,
              "the server shows each target's access and logs failures, without secrets")

    # Configuration files keep the settings.
    saved = ctx.path("access.conf")
    rc, _ = run_cli(ctx, *args, "--save-config", saved)
    text = open(saved, encoding="utf-8-sig").read() if rc == 0 else ""
    ctx.check(f"[target {chap}]\nchap-user = alice\nchap-secret = {secret}\nmutual-chap-secret = {mutual}\nram = 1M\n"
              in text.replace("\r\n", "\n") and "allow-initiator = IQN.2026-10.TEST:INITIATOR" in text,
              "--save-config writes the access settings")
    with Server(ctx, ["--config", saved]):
        s = ctx.session()
        ctx.check(s.full_login(chap)[0] == 0x0201, "a saved CHAP target still needs CHAP")
        s.close()
        s = ctx.session()
        ctx.check(s.chap_login(chap, "alice", secret)[0] == 0, "and logs in with it")
        s.logout()
    for extra, expected in ((["--chap-secret", "short"], "--chap-secret must be 12 to 255 characters long"),
                            (["--chap-user", "alice"], "chap-user and the mutual CHAP settings need chap-secret"),
                            (["--chap-secret", secret, "--mutual-chap-secret", secret], "must differ from chap-secret"),
                            (["--chap-secret", secret, "--mutual-chap-user", "t"], "mutual-chap-user needs"),
                            (["--allow-initiator", "not-an-iqn"], "Invalid iSCSI initiator name: not-an-iqn")):
        rc, out = run_cli(ctx, "--target", chap, "--ram", "1M", *extra)
        ctx.check(rc == 1 and expected in out and (secret not in out or "--mutual" in " ".join(extra)),
                  f"access error: {expected}")


# ---- Windows service ---------------------------------------------------------

def test_service(ctx):
    conf = os.path.join(ctx.path("service"), "server.conf")
    os.makedirs(os.path.dirname(conf), exist_ok=True)
    with open(conf, "w", encoding="utf-8") as f:
        f.write(f"[target {TARGET}]\nram = 1M\n")
    rc, out = run_cli(ctx, "--service", "--config", conf)
    ctx.check(rc == 1 and "--service is for the Windows service controller" in out,
              "--service from a console says what it is for")
    rc, out = run_cli(ctx, "--install-service")
    ctx.check(rc == 1 and "need --config" in out, "--install-service needs --config")
    rc, out = run_cli(ctx, "--install-service", "--config", conf, "--port", "5")
    ctx.check(rc == 1 and "takes only --config and --service-name" in out,
              "--install-service takes the server options from the file only")
    rc, out = run_cli(ctx, "--install-service", "--config", conf, "--service-name", "a/b")
    ctx.check(rc == 1 and "Invalid service name: a/b" in out, "service names cannot contain slashes")
    if not is_elevated():
        rc, out = run_cli(ctx, "--install-service", "--config", conf, "--service-name", "TISCSI-Test")
        ctx.check(rc == 2 and "Access is denied" in out and "administrator" in out,
                  "without administrator rights, --install-service explains why it failed")


def service_config(name):
    result = subprocess.run(["sc", "qc", name], capture_output=True, timeout=30)
    return result.returncode, result.stdout.decode("oem", "replace")


def test_service_elevated(ctx):
    """Needs administrator rights: installs a test service, serves from it,
    updates it, lets it fail on a broken configuration and removes it."""
    if not is_elevated():
        print("  SKIP not running as administrator")
        return
    name = f"TISCSI-Test-{os.getpid()}"
    folder = ctx.path("service elevated")
    os.makedirs(folder, exist_ok=True)
    data = os.urandom(512)
    ctx.make_image(os.path.join("service elevated", "disk.img"), MiB, data)
    conf = os.path.join(folder, "server.conf")
    with open(conf, "w", encoding="utf-8") as f:
        f.write(f"[server]\nlisten = {HOST}\nport = {ctx.port}\nwait = 5\n[target {TARGET}]\nimage-ro = disk.img\n")
    log = os.path.join(folder, "TISCSI-service.log")
    try:
        rc, out = run_cli(ctx, "--install-service", "--config", conf, "--service-name", name)
        ctx.check(rc == 0 and f"Installed the service {name}" in out and "The service is running" in out,
                  "--install-service installs and starts the service")
        rc, qc = service_config(name)
        ctx.check(rc == 0 and "AUTO_START" in qc and f'--service --config "{conf}"' in qc and "LocalSystem" in qc,
                  "the service starts automatically, as LocalSystem, with the configuration file")
        listening = False
        deadline = time.time() + 15
        while time.time() < deadline and not listening:
            try:
                socket.create_connection((HOST, ctx.port), timeout=1).close()
                listening = True
            except OSError:
                time.sleep(0.3)
        ctx.check(listening, "the service listens")
        if listening:
            s = ctx.session()
            ctx.check(s.full_login(TARGET)[0] == 0 and s.read(0, 1)[1] == data, "the service serves the configuration")
            s.logout()
        text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
        ctx.check(f"Listening on {HOST}:{ctx.port}" in text and "Press Ctrl+C" not in text,
                  "the service logs (in English) to TISCSI-service.log next to the configuration")

        rc, out = run_cli(ctx, "--install-service", "--config", conf, "--service-name", name)
        ctx.check(rc == 0 and f"Updated the service {name}" in out and "The service is running" in out,
                  "--install-service again updates and restarts the service")

        with open(conf, "w", encoding="utf-8") as f:
            f.write(f"[server]\nport = {ctx.port}\n[target {TARGET}]\nimage = missing.img\n")
        rc, out = run_cli(ctx, "--install-service", "--config", conf, "--service-name", name)
        ctx.check(rc == 2 and "stopped (exit code 2)" in out and "no wait setting" in out,
                  "a configuration that cannot be served stops the service, and the install says so")
        text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
        ctx.check("Cannot open the disk image" in text and "missing.img" in text, "the service logs why it stopped")
    finally:
        rc, out = run_cli(ctx, "--uninstall-service", "--service-name", name)
        ctx.check(rc == 0 and f"Removed the service {name}" in out, "--uninstall-service removes the service")
        rc, _ = service_config(name)
        ctx.check(rc != 0, "the service is gone")


def test_optical_elevated(ctx):
    """Needs administrator rights: connects the Microsoft iSCSI Initiator to a
    CD and a DVD image and reads files through Windows' CD-ROM driver and CDFS."""
    if not is_elevated():
        print("  SKIP not running as administrator")
        return
    target = "iqn.2026-10.test:optical"
    hello = b"Hello from TISCSI\r\n"
    payload = os.urandom(3 * MiB)
    files = {"HELLO.TXT": hello, "RANDOM.BIN": payload}
    cd = iso_tool.make_iso(ctx.path("win-cd.iso"), files, "TISCSI_CD")
    dvd = iso_tool.make_iso(ctx.path("win-dvd.iso"), files, "TISCSI_DVD")
    make_sparse(dvd, DVD_SECTORS * 2048)
    chap_target = "iqn.2026-10.test:optical-chap"
    chap_cd = iso_tool.make_iso(ctx.path("win-chap.iso"), files, "TISCSI_CHAP")
    chap_secret = "ms-secret-1234"  # the Microsoft initiator wants 12 to 16 characters

    _, status = powershell("(Get-Service MSiSCSI).Status")
    started_service = status != "Running"
    if started_service:
        rc, out = powershell("Start-Service MSiSCSI")
        ctx.check(rc == 0, "the Microsoft iSCSI Initiator service starts")
        if rc:
            print(out)
            return
    portal = f"-TargetPortalAddress {HOST} -TargetPortalPortNumber {ctx.port}"
    try:
        with Server(ctx, ["--target", target, "--image-ro", cd, "--image-ro", dvd,
                          "--target", chap_target, "--chap-user", "msuser", "--chap-secret", chap_secret,
                          "--cdrom", chap_cd]):
            rc, out = powershell(f"New-IscsiTargetPortal {portal} | Out-Null; "
                                 f"Connect-IscsiTarget -NodeAddress {target} {portal} | Out-Null")
            ctx.check(rc == 0, "Microsoft iSCSI Initiator logs in")
            if rc:
                print(out)
            try:
                for label in ("TISCSI_CD", "TISCSI_DVD"):
                    drive = find_cd_volume(label) if rc == 0 else None
                    ctx.check(drive is not None, f"Windows mounts {label} on a CD/DVD drive")
                    if drive:
                        with open(drive + "\\HELLO.TXT", "rb") as f:
                            text = f.read()
                        with open(drive + "\\RANDOM.BIN", "rb") as f:
                            data = f.read()
                        ctx.check(text == hello and data == payload, f"{label}: files read back through CDFS")
                _, out = powershell("Get-CimInstance Win32_CDROMDrive | Where-Object Caption -like 'TISCSI*' | "
                                    "ForEach-Object { \"$($_.Caption)|$($_.MediaLoaded)\" }")
                ctx.check(out.count("|True") == 2, f"Win32_CDROMDrive lists both drives with media ({out!r})")

                # CHAP with the Microsoft initiator.
                rc, _ = powershell(f"Connect-IscsiTarget -NodeAddress {chap_target} {portal} | Out-Null")
                ctx.check(rc != 0, "Microsoft iSCSI Initiator without CHAP is refused by a CHAP target")
                rc, out = powershell(f"Connect-IscsiTarget -NodeAddress {chap_target} {portal} "
                                     f"-AuthenticationType ONEWAYCHAP -ChapUsername msuser -ChapSecret {chap_secret} "
                                     "| Out-Null")
                ctx.check(rc == 0, "Microsoft iSCSI Initiator logs in with CHAP")
                if rc:
                    print(out)
                drive = find_cd_volume("TISCSI_CHAP") if rc == 0 else None
                text = open(drive + "\\HELLO.TXT", "rb").read() if drive else b""
                ctx.check(text == hello, "TISCSI_CHAP: files read back through CDFS")
            finally:
                powershell(f"Disconnect-IscsiTarget -NodeAddress {target} -Confirm:$false")
                powershell(f"Disconnect-IscsiTarget -NodeAddress {chap_target} -Confirm:$false")
                powershell(f"Remove-IscsiTargetPortal {portal} -Confirm:$false")
    finally:
        if started_service:
            powershell("Stop-Service MSiSCSI")


# ---- Physical disks, volumes, SCSI pass-through ------------------------------

def is_elevated():
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def list_devices(ctx):
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--list")
    return rc, out


def find_listed_disk(listing, description):
    """Disk number of the --list entry "  N: <description>", or None."""
    import re
    match = re.search(r"^  (\d+): " + re.escape(description) + r"(?: \(dynamic disk\))?$", listing, re.M)
    return match.group(1) if match else None


def find_listed_volume(listing, disk_index):
    """Volume name of the first --list volume on disk `disk_index`, or None."""
    import re
    pattern = r"^  (\\\\\?\\Volume\{[^}]+\}\\)\r?\n[^\n]*on disk: " + re.escape(disk_index) + r"(?:,|$)"
    match = re.search(pattern, listing, re.M)
    return match.group(1) if match else None


def listed_stable_names(listing, entry):
    """Stable names on the "    stable names:" line right after the --list
    line `entry` (a regex matching the whole line), or []."""
    match = re.search(r"^" + entry + r"\r?\n    stable names: ([^\r\n]+)", listing, re.M)
    if not match:
        return []
    return [name.strip('"') for name in match.group(1).split("  ")]


def listed_disks(listing):
    """(disk number, stable names) of every --list disk."""
    return [(index, listed_stable_names(listing, r"  " + index + r": [^\r\n]*"))
            for index in re.findall(r"^  (\d+): ", listing, re.M)]


def listed_volumes(listing):
    """(volume name, stable name or None) of every --list volume."""
    result = []
    for name in re.findall(r"^  (\\\\\?\\Volume\{[^}]+\}\\)\r?$", listing, re.M):
        names = listed_stable_names(listing, r"  " + re.escape(name) + r"\r?\n[^\r\n]*")
        result.append((name, names[0] if names else None))
    return result


def probe_server(ctx, *args):
    """Output of TISCSICli started with `args`, stopped if it starts serving."""
    process = subprocess.Popen([ctx.exe, "--lang", "en-US", "--listen", HOST, "--port", str(ctx.port), *args],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    try:
        out, _ = process.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        os.kill(process.pid, signal.CTRL_BREAK_EVENT)
        out, _ = process.communicate(timeout=20)
    return out.decode("utf-8", "replace")


def test_devices_unprivileged(ctx):
    rc, out = list_devices(ctx)
    ctx.check(rc == 0 and "Physical disks:" in out and "Volumes:" in out and "SCSI devices:" in out,
              "--list shows disks, volumes and SCSI devices")
    disks = listed_disks(out)
    volumes = listed_volumes(out)
    # The system disk has a partition table, so at least one disk and volume have stable names.
    ctx.check(any(names for _, names in disks) and any(name for _, name in volumes) and
              all(name.startswith(("serial:", "gpt:{", "mbr:")) for _, names in disks for name in names) and
              all(name.startswith(("gpt:{", "mbr:")) for _, name in volumes if name),
              "--list shows stable names for disks and volumes")
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--disk", "abc")
    ctx.check(rc == 1 and "Invalid disk" in out, "--disk needs a number or a stable name")
    for name in ("mbr:XYZ", "mbr:0", "mbr:1234:5", "gpt:{1234}", "serial:"):
        rc, out = run_tool(ctx.exe, "--lang", "en-US", "--disk", name)
        ctx.check(rc == 1 and "Invalid disk" in out, f"--disk rejects {name}")
    for name in ("mbr:1234", "gpt:not-a-guid"):
        rc, out = run_tool(ctx.exe, "--lang", "en-US", "--volume", name)
        ctx.check(rc == 1 and "Invalid device name" in out, f"--volume rejects {name}")
    for option, name in (("--disk", "serial:NO-SUCH-DISK"), ("--disk", "mbr:0BADF00D"),
                         ("--disk", "gpt:{0badf00d-0000-4000-8000-000000000001}"),
                         ("--volume", "gpt:{0badf00d-0000-4000-8000-000000000001}"),
                         ("--volume", "mbr:0BADF00D:1048576"), ("--spti", "serial:NO-SUCH-DEVICE")):
        rc, out = run_tool(ctx.exe, "--lang", "en-US", "--port", str(ctx.port), option, name)
        ctx.check(rc == 2 and f'No device has the name "{name}"' in out, f"{option} {name} matches nothing")
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--ram", "1M", "--spti", r"\\.\CdRom0")
    ctx.check(rc == 1 and "--spti cannot be combined" in out, "--spti cannot be mixed with disks")
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--volume", "Q:\\no\\such\\mount\\")
    ctx.check(rc == 2 and "Cannot open" in out, "unknown volume is reported")
    if not is_elevated():
        rc, out = run_tool(ctx.exe, "--lang", "en-US", "--port", str(ctx.port), "--disk", "0")
        ctx.check(rc == 2 and "Access is denied" in out and "administrator" in out,
                  "without administrator rights, --disk explains why it failed")
        # Opening usually fails, but the error names the device the stable
        # name matched. Removable media may be readable without
        # administrator rights; then the LUN line names it.
        for index, names in disks:
            for name in names:
                out = probe_server(ctx, "--disk-ro", name.upper())
                ctx.check(rf"\\.\PhysicalDrive{index}" in out or f"Physical disk {index} (" in out,
                          f"--disk-ro {name.upper()} finds disk {index}")
        for volume, name in volumes:
            if name:
                out = probe_server(ctx, "--volume-ro", name)
                ctx.check(volume in out, f"--volume-ro {name} finds {volume}")


def diskpart(ctx, name, commands):
    script = ctx.path(name)
    with open(script, "w", encoding="utf-8") as f:
        f.write("\n".join(commands) + "\n")
    result = subprocess.run(["diskpart", "/s", script], capture_output=True, timeout=120)
    # diskpart writes in the console (OEM) code page.
    return result.returncode, (result.stdout + result.stderr).decode("oem", "replace")


VDS_E_NOT_SUPPORTED = 0x80042400


def test_devices_elevated(ctx):
    """Needs administrator rights: attaches a 64 MiB test VHD with diskpart and
    serves it as a physical disk, a volume and through SCSI pass-through."""
    if not is_elevated():
        print("  SKIP not running as administrator")
        return
    vhd = ctx.path("device-test.vhd")
    rc, out = diskpart(ctx, "attach.txt", [
        f'create vdisk file="{vhd}" maximum=64 type=fixed',
        f'select vdisk file="{vhd}"',
        "attach vdisk",
        "convert mbr",
        "create partition primary",
        "format fs=fat32 quick label=TISCSITEST",
    ])
    ctx.check(rc == 0, "diskpart creates and attaches a test VHD")
    try:
        time.sleep(2)
        _, out = list_devices(ctx)
        index = find_listed_disk(out, "Msft Virtual Disk, 64 MiB")
        ctx.check(index is not None, "the test VHD shows up in --list")
        if not index:
            return
        volume = find_listed_volume(out, index)
        ctx.check(volume is not None, "its volume shows up in --list")
        pattern = os.urandom(4096)

        with Server(ctx, ["--target", TARGET, "--disk", index]):
            s = ctx.session()
            s.full_login(TARGET)
            st, size, block = s.capacity()
            ctx.check(st == 0 and size == 64 * MiB and block == 512, "--disk: capacity")
            st, data, _, _ = s.read(0, 1)
            ctx.check(st == 0 and data[510:512] == b"\x55\xaa", "--disk: reads the MBR")
            st, _, _, _ = s.write(8, pattern)  # in the gap before the first partition
            st2, data, _, _ = s.read(8, 8)
            ctx.check(st == 0 and st2 == 0 and data == pattern, "--disk: write and read back (volume locked)")
            s.logout()

        if volume:
            with Server(ctx, ["--target", TARGET, "--volume", volume]):
                s = ctx.session()
                s.full_login(TARGET)
                st, data, _, _ = s.read(0, 1)
                ctx.check(st == 0 and data[82:90] == b"FAT32   ", "--volume: sector 0 is the FAT32 boot sector")
                st, size, _ = s.capacity()
                last = size // 512 - 8
                st, _, _, _ = s.write(last, pattern)
                st2, data, _, _ = s.read(last, 8)
                ctx.check(st == 0 and st2 == 0 and data == pattern, "--volume: write and read back (volume locked)")
                s.logout()

        with Server(ctx, ["--target", TARGET, "--disk-ro", index]):
            s = ctx.session()
            s.full_login(TARGET)
            st, data, _, _ = s.read(8, 8)
            ctx.check(st == 0 and data == pattern, "--disk-ro: earlier write persisted")
            st, data, _, _ = s.write(8, bytes(4096))
            ctx.check(st == 2 and sense_key(data) == 7, "--disk-ro: writes are refused")
            s.logout()

        disk_names = [name for name in dict(listed_disks(out)).get(index, []) if name.startswith("mbr:")]
        ctx.check(len(disk_names) == 1, "--list shows the test VHD's MBR disk signature")
        for name in disk_names:
            with Server(ctx, ["--target", TARGET, "--disk-ro", name]):
                s = ctx.session()
                s.full_login(TARGET)
                st, data, _, _ = s.read(8, 8)
                ctx.check(st == 0 and data == pattern, f"--disk-ro {name}: serves the test VHD")
                s.logout()
        volume_name = dict(listed_volumes(out)).get(volume) if volume else None
        ctx.check(bool(volume_name and disk_names and volume_name.startswith(disk_names[0] + ":")),
                  "--list shows the volume's MBR stable name")
        if volume_name:
            with Server(ctx, ["--target", TARGET, "--volume-ro", volume_name]):
                s = ctx.session()
                s.full_login(TARGET)
                st, data, _, _ = s.read(0, 1)
                ctx.check(st == 0 and data[82:90] == b"FAT32   ", f"--volume-ro {volume_name}: serves the volume")
                s.logout()

        try:
            with Server(ctx, ["--target", TARGET, "--spti", rf"\\.\PhysicalDrive{index}"]):
                s = ctx.session()
                s.full_login(TARGET)
                st, data, _, _ = s.scsi(bytes([0x12, 0, 0, 0, 96, 0]), read_len=96)
                ctx.check(st == 0 and data[8:12] == b"Msft", "--spti: INQUIRY comes from the device")
                st, data, _, _ = s.scsi(cdb10(0x28, 8, 8), read_len=4096)
                ctx.check(st == 0 and data == pattern, "--spti: READ (10) through pass-through")
                s.logout()
        except RuntimeError as e:
            ctx.check(False, f"--spti on the test disk: {e}")
    finally:
        diskpart(ctx, "detach.txt", [f'select vdisk file="{vhd}"', "detach vdisk"])


# ---- Dynamic volumes (LDM) ---------------------------------------------------

def read_volume(s, lun, size):
    """Whole LUN in 1 MiB reads, or None on error."""
    return read_all(s, lun, size)


def ldm_args(paths):
    args = []
    for path in paths:
        args += ["--ldm-disk", path]
    return args


def test_dynamic_volumes(ctx):
    """Dynamic volumes assembled from images of LDM disks built by ldm_tool."""
    paths = [ctx.path(f"ldm{i}.img") for i in range(3)]
    group, contents = ldm_tool.build_test_group(paths)
    ldm_tool.damage_primary_headers(group, paths)  # readers fall back to the secondary copies
    volumes = {v.name: v for v in group.volumes}

    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--list", *ldm_args(paths))
    expected = [("Volume1", "simple"), ("Volume2", "spanned"), ("Volume3", "striped"), ("Volume4", "mirrored"),
                ("Volume5", "RAID-5")]
    ctx.check(rc == 0 and all(f"  {name}  {{{volumes[name].guid}}}" in out for name, _ in expected),
              "--list --ldm-disk shows every volume (secondary PRIVHEAD / TOCBLOCK used)")
    ctx.check(all(re.search(rf"{name}  .*\r?\n.*layout: {layout}; status: healthy", out) for name, layout in expected),
              "--list shows layouts and health")
    ctx.check("Not mounted by Windows" in out, "--list says image volumes are not mounted by Windows")

    # Every layout read-only, by name and by GUID.
    args = ["--target", TARGET] + ldm_args(paths)
    for name, _ in expected:
        args += ["--dynamic-volume-ro", name if name != "Volume3" else f"{{{volumes[name].guid}}}"]
    with Server(ctx, args):
        s = ctx.session()
        s.full_login(TARGET, [("MaxRecvDataSegmentLength", "262144")])
        for lun, (name, layout) in enumerate(expected):
            size = len(contents[name])
            st, capacity, _ = s.capacity(lun)
            ctx.check(st == 0 and capacity == size, f"{name} ({layout}): capacity {capacity}")
            ctx.check(read_volume(s, lun, size) == contents[name], f"{name} ({layout}): content")
        st, data, _, _ = s.write(0, bytes(4096), lun=0)
        ctx.check(st == 2 and sense_key(data) == 7, "--dynamic-volume-ro refuses writes")
        s.logout()

    # Degraded: without disk 1, the mirror reads its second plex and RAID-5
    # rebuilds column 1 from the others; the spanned volume cannot be served.
    present = [paths[0], paths[2]]
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--list", *ldm_args(present))
    ctx.check(re.search(r"Volume4  .*\r?\n.*status: failed redundancy", out) is not None and
              re.search(r"Volume2  .*\r?\n.*status: failed", out) is not None,
              "--list shows failed redundancy and failed volumes")
    with Server(ctx, ["--target", TARGET] + ldm_args(present) +
                ["--dynamic-volume-ro", "Volume4", "--dynamic-volume-ro", "Volume5"]):
        s = ctx.session()
        s.full_login(TARGET, [("MaxRecvDataSegmentLength", "262144")])
        ctx.check(read_volume(s, 0, len(contents["Volume4"])) == contents["Volume4"],
                  "degraded mirror reads from the remaining plex")
        ctx.check(read_volume(s, 1, len(contents["Volume5"])) == contents["Volume5"],
                  "degraded RAID-5 rebuilds the missing column")
        s.logout()
    with Server(ctx, ["--target", TARGET] + ldm_args(paths[:2]) + ["--dynamic-volume-ro", "Volume5"]):
        s = ctx.session()
        s.full_login(TARGET, [("MaxRecvDataSegmentLength", "262144")])
        ctx.check(read_volume(s, 0, len(contents["Volume5"])) == contents["Volume5"],
                  "degraded RAID-5 rebuilds the last column")
        s.logout()
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--port", str(ctx.port), *ldm_args(present),
                       "--dynamic-volume-ro", "Volume2")
    ctx.check(rc == 2 and "disks holding its data are missing" in out, "a volume with missing data is refused")
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--port", str(ctx.port), *ldm_args(present),
                       "--dynamic-volume", "Volume5")
    ctx.check(rc == 2 and "missing a member disk" in out, "read-write access to a degraded volume is refused")
    rc, out = run_tool(ctx.exe, "--lang", "ja-JP", "--port", str(ctx.port), *ldm_args(present),
                       "--dynamic-volume", "Volume5")
    ctx.check(rc == 2 and "メンバー ディスクが欠けています" in out, "the refusal is localized")

    # Read-write: writes across extents, stripes and parity rows.
    writes = [(4000, os.urandom(300 * 512)),      # spanned: across the extent boundary at 4096
              (100, os.urandom(70 * 1024)),        # partial stripes
              (1000, os.urandom(1 * MiB + 1536)),  # many stripes
              (0, os.urandom(512))]
    rw_names = ["Volume2", "Volume3", "Volume4", "Volume5"]
    args = ["--target", TARGET] + ldm_args(paths)
    for name in rw_names:
        args += ["--dynamic-volume", name]
    expected_after = {name: apply_writes(contents[name], writes) for name in rw_names}
    with Server(ctx, args) as server:
        s = ctx.session()
        s.full_login(TARGET, [("MaxRecvDataSegmentLength", "262144")])
        for lun, name in enumerate(rw_names):
            ok = all(s.write(lba, data, lun=lun)[0] == 0 for lba, data in writes)
            ctx.check(ok and read_volume(s, lun, len(contents[name])) == expected_after[name],
                      f"{name}: writes read back over iSCSI")
        s.logout()
        rc, out = run_tool(ctx.exe, "--lang", "en-US", "--port", str(ctx.port + 1), *ldm_args(paths),
                           "--dynamic-volume", "Volume1")
        ctx.check(rc == 2 and "Cannot open" in out, "member images are opened exclusively for writing")
    files = ldm_tool.open_files(paths)
    try:
        for name in rw_names:
            volume = volumes[name]
            plexes = [group.read_plex(files, plex, volume.sectors()) for plex in volume.plexes]
            ctx.check(all(p == expected_after[name] for p in plexes), f"{name}: data on the member images")
        ctx.check(group.parity_ok(files, volumes["Volume5"].plexes[0]), "Volume5: RAID-5 parity is consistent")
    finally:
        for f in files:
            f.close()

    # Lookup errors.
    other = ctx.path("ldm-other.img")
    other_group = ldm_tool.Group("OTHERDg0", [16384], gpt=[False])
    other_group.volumes = [ldm_tool.Volume("Volume1", [ldm_tool.Plex("concat", [ldm_tool.Extent(0, 0, 4096)])])]
    other_group.write_disks([other])
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--port", str(ctx.port), *ldm_args(paths + [other]),
                       "--dynamic-volume-ro", "Volume1")
    ctx.check(rc == 2 and "More than one dynamic volume" in out, "an ambiguous name is refused")
    with Server(ctx, ["--target", TARGET] + ldm_args(paths + [other]) +
                ["--dynamic-volume-ro", str(other_group.volumes[0].guid)]):
        s = ctx.session()
        s.full_login(TARGET)
        st, capacity, _ = s.capacity(0)
        ctx.check(st == 0 and capacity == 4096 * 512, "a GUID picks the volume of the second group")
        s.logout()
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--port", str(ctx.port), *ldm_args(paths),
                       "--dynamic-volume-ro", "Volume9")
    ctx.check(rc == 2 and "No dynamic volume matches" in out, "an unknown volume is reported")
    rc, out = run_tool(ctx.exe, "--lang", "en-US", "--ldm-disk", paths[0], "--ram", "1M")
    ctx.check(rc == 1 and "--ldm-disk can only be used" in out, "--ldm-disk without a dynamic volume is rejected")


WINDOWS_VOLUME_PREFIX = "\\\\?\\Volume{"


def test_dynamic_volumes_elevated(ctx):
    """Needs administrator rights: builds dynamic volumes with diskpart on three
    test VHDs and checks that TISCSI reads Windows' LDM database the way
    Windows does (served through Windows while attached, assembled from the
    VHD files after they are detached)."""
    if not is_elevated():
        print("  SKIP not running as administrator")
        return
    vhds = [ctx.path(f"ldm-test{i}.vhd") for i in range(3)]
    commands = []
    for vhd in vhds:
        commands += [f'create vdisk file="{vhd}" maximum=64 type=fixed', f'select vdisk file="{vhd}"',
                     "attach vdisk", "convert mbr"]
    rc, out = diskpart(ctx, "ldm-attach.txt", commands)
    converting = rc == 0
    if converting:
        commands = sum(([f'select vdisk file="{v}"', "convert dynamic"] for v in vhds), [])
        rc, out = diskpart(ctx, "ldm-convert.txt", commands)
    try:
        if converting and rc == VDS_E_NOT_SUPPORTED:
            # Recent Windows 11 builds refuse to make virtual disks dynamic.
            print("  SKIP this Windows does not convert virtual disks to dynamic disks (VDS_E_NOT_SUPPORTED);"
                  " run this test on Windows 7 / 10 or Windows Server")
            return
        ctx.check(rc == 0, "diskpart creates three dynamic test disks")
        if rc != 0:
            print(f"  diskpart exit code {rc:#x}, output:\n" + "\n".join("    " + line for line in out.splitlines()))
            return  # the finally block detaches whatever was attached
        time.sleep(2)
        _, listing = list_devices(ctx)
        indices = []
        for match in re.finditer(r"^  (\d+): Msft Virtual Disk, 64 MiB \(dynamic disk\)$", listing, re.M):
            indices.append(match.group(1))
        ctx.check(len(indices) >= 3, "the dynamic test disks show up in --list")
        if len(indices) < 3:
            return
        # The newest three virtual disks are ours.
        a, b, c = indices[-3:]
        layouts = [("simple", f"create volume simple size=8 disk={a}"),
                   ("spanned", f"create volume simple size=4 disk={a}\nextend size=4 disk={b}"),
                   ("striped", f"create volume stripe size=6 disk={a},{b},{c}"),
                   ("mirrored", f"create volume mirror size=6 disk={b},{c}"),
                   ("RAID-5", f"create volume raid size=6 disk={a},{b},{c}")]
        created = []
        for layout, command in layouts:
            rc, out = diskpart(ctx, "ldm-volume.txt", command.split("\n"))
            if rc == 0:
                created.append(layout)
            elif layout == "RAID-5":
                print("  NOTE RAID-5 volumes need Windows Server; skipped")
            else:
                ctx.check(False, f"diskpart creates a {layout} volume: {out.strip()[-200:]}")
        time.sleep(3)
        _, listing = list_devices(ctx)
        found = {}
        ours = {a, b, c}
        for match in re.finditer(r"^  (Volume\d+)  \{([0-9a-f-]+)\}[ \t]*\r?\n.*layout: ([^;]+); status: ([^;]+); "
                                 r"size: [^;]+; on disk: ([^\r\n]*)\r?\n[ \t]*(.*)$", listing, re.M):
            name, guid, layout, health, disks, mounted = match.groups()
            # Only volumes on the test disks (the computer may have others).
            if layout in created and layout not in found and set(disks.strip().split(", ")) <= ours:
                found[layout] = (guid, health, mounted.strip())
        ctx.check(sorted(found) == sorted(created), f"--list shows the new dynamic volumes ({', '.join(created)})")
        ctx.check(all(m.startswith("Windows volume: \\\\?\\Volume{" + g + "}") for g, _, m in found.values()),
                  "the LDM volume GUID is the Windows volume GUID")

        # Write each volume in full through Windows (read-write uses the
        # operating system's volume, which TISCSI locks).
        contents = {}
        for layout, (guid, _, _) in found.items():
            with Server(ctx, ["--target", TARGET, "--dynamic-volume", guid]) as server:
                s = ctx.session()
                s.full_login(TARGET, [("MaxRecvDataSegmentLength", "262144")])
                st, size, _ = s.capacity()
                data = os.urandom(size)
                ok = st == 0 and all(s.write(o // 512, data[o:o + MiB])[0] == 0 for o in range(0, size, MiB))
                ctx.check(ok and read_volume(s, 0, size) == data, f"{layout}: written over iSCSI")
                contents[layout] = data
                s.logout()
            ctx.check(WINDOWS_VOLUME_PREFIX + guid in server.output, f"{layout}: served through the Windows volume")

        rc, out = diskpart(ctx, "ldm-detach.txt", sum(([f'select vdisk file="{v}"', "detach vdisk"] for v in vhds), []))
        ctx.check(rc == 0, "diskpart detaches the test disks" + ("" if rc == 0 else ": " + out.strip()[-300:]))
        time.sleep(2)

        # Assembled by TISCSI from the VHD files: same data, then new writes.
        writes = [(16, os.urandom(200 * 1024)), (3000, os.urandom(MiB))]
        for layout, (guid, _, _) in found.items():
            with Server(ctx, ["--target", TARGET] + ldm_args(vhds) + ["--dynamic-volume", guid]) as server:
                s = ctx.session()
                s.full_login(TARGET, [("MaxRecvDataSegmentLength", "262144")])
                data = contents[layout]
                ctx.check(read_volume(s, 0, len(data)) == data, f"{layout}: TISCSI assembles the data Windows wrote")
                ok = all(s.write(lba, chunk)[0] == 0 for lba, chunk in writes)
                ctx.check(ok, f"{layout}: written by TISCSI from the VHD files")
                contents[layout] = apply_writes(data, writes)
                s.logout()
            ctx.check("{" + guid + "}" in server.output and WINDOWS_VOLUME_PREFIX not in server.output,
                      f"{layout}: assembled by TISCSI")

        commands = sum(([f'select vdisk file="{v}"', "attach vdisk"] for v in vhds), [])
        rc, out = diskpart(ctx, "ldm-reattach.txt", commands)
        ctx.check(rc == 0, "diskpart attaches the test disks again" + ("" if rc == 0 else ": " + out.strip()[-300:]))
        time.sleep(5)
        for layout, (guid, _, _) in found.items():
            try:
                with Server(ctx, ["--target", TARGET, "--dynamic-volume-ro", guid]) as server:
                    s = ctx.session()
                    s.full_login(TARGET, [("MaxRecvDataSegmentLength", "262144")])
                    content_ok = read_volume(s, 0, len(contents[layout])) == contents[layout]
                    s.logout()
                # Windows normally mounts the volumes again; if it left the
                # group offline, TISCSI assembled them from the physical disks.
                route = "Windows" if WINDOWS_VOLUME_PREFIX + guid in server.output else "TISCSI (physical disks)"
                ctx.check(content_ok, f"{layout}: after re-attaching, {route} reads what TISCSI wrote")
            except RuntimeError as e:
                ctx.check(False, f"{layout}: serving after re-attaching: {e}")
    finally:
        commands = []
        for vhd in vhds:
            commands += [f'select vdisk file="{vhd}"', "detach vdisk noerr"]
        diskpart(ctx, "ldm-cleanup.txt", commands)


# ---- Graphical front end ---------------------------------------------------

class Gui:
    """Drives TISCSI.exe through window messages (ctypes, no extra packages)."""

    WM_SETTEXT, WM_GETTEXT, WM_GETTEXTLENGTH, WM_COMMAND, WM_CLOSE = 0x000C, 0x000D, 0x000E, 0x0111, 0x0010
    # Control and command IDs from src/TISCSI/resource.h
    IDOK, IDC_PORT, IDC_START, IDC_ADD_TARGET = 1, 1011, 1012, 1014
    IDC_TARGET_NAME, IDC_CREATE_RAMDISK, IDC_SIZE_MB, IDM_LANG_FIRST = 1020, 1023, 1033, 40101
    IDC_ADD_IMAGE, IDC_PATH, IDC_READ_ONLY, IDC_OPTICAL = 1024, 1030, 1032, 1038
    IDCANCEL, IDM_FILE_OPEN_CONFIG, IDM_FILE_SAVE_CONFIG = 2, 40003, 40004
    IDC_ACCESS, IDC_ACCESS_SUMMARY, IDC_ALLOWED_INITIATORS = 1028, 1029, 1040
    IDC_CHAP_USER, IDC_CHAP_SECRET = 1041, 1042
    IDC_STATIC_TEXT = 0xFFFF  # message box text
    BM_GETCHECK, BM_CLICK = 0x00F0, 0x00F5

    def __init__(self, exe, args):
        import ctypes
        from ctypes import wintypes
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        self.user32.SendMessageW.restype = wintypes.LPARAM
        self.user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        self.user32.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
        self.user32.GetDlgItem.restype = wintypes.HWND
        self.user32.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
        self.user32.FindWindowExW.restype = wintypes.HWND
        self.ctypes = ctypes
        self.wintypes = wintypes
        self.process = subprocess.Popen([exe] + args)
        time.sleep(1.5)

    def top_window(self):
        """The process's visible, enabled top-level window (the active dialog)."""
        ctypes, wintypes = self.ctypes, self.wintypes
        found = []
        proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

        def callback(hwnd, _):
            pid = wintypes.DWORD()
            self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == self.process.pid and self.user32.IsWindowVisible(hwnd) and \
                    self.user32.IsWindowEnabled(hwnd):
                found.append(hwnd)
            return True

        time.sleep(0.7)
        self.user32.EnumWindows(proc(callback), 0)
        return found[0] if found else None

    def text(self, hwnd):
        length = self.user32.SendMessageW(hwnd, self.WM_GETTEXTLENGTH, 0, 0)
        buffer = self.ctypes.create_unicode_buffer(length + 1)
        self.user32.SendMessageW(hwnd, self.WM_GETTEXT, length + 1, self.ctypes.addressof(buffer))
        return buffer.value

    def set_text(self, dialog, control, value):
        buffer = self.ctypes.create_unicode_buffer(value)
        self.user32.SendMessageW(self.user32.GetDlgItem(dialog, control), self.WM_SETTEXT, 0,
                                 self.ctypes.addressof(buffer))

    def click(self, dialog, control):
        self.user32.SendMessageW(self.user32.GetDlgItem(dialog, control), self.BM_CLICK, 0, 0)

    def checkbox(self, dialog, control):
        """(checked, enabled) of a check box."""
        hwnd = self.user32.GetDlgItem(dialog, control)
        return self.user32.SendMessageW(hwnd, self.BM_GETCHECK, 0, 0) == 1, bool(self.user32.IsWindowEnabled(hwnd))

    def command(self, window, command_id):
        self.user32.PostMessageW(window, self.WM_COMMAND, command_id, 0)

    def class_name(self, hwnd):
        buffer = self.ctypes.create_unicode_buffer(256)
        self.user32.GetClassNameW(hwnd, buffer, 256)
        return buffer.value

    def file_dialog_name_box(self, dialog):
        """The file name edit box of an Open / Save As dialog: an Edit in a
        ComboBox (the address bar's ComboBox sits in a ComboBoxEx32)."""
        ctypes, wintypes = self.ctypes, self.wintypes
        found = []
        proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

        def callback(hwnd, _):
            parent = self.user32.GetParent(hwnd)
            if self.class_name(hwnd) == "Edit" and self.class_name(parent) == "ComboBox" and \
                    self.class_name(self.user32.GetParent(parent)) != "ComboBoxEx32":
                found.append(hwnd)
            return True

        self.user32.EnumChildWindows(dialog, proc(callback), 0)
        return found[0] if found else None

    def status_text(self, window):
        status = self.user32.FindWindowExW(window, None, "msctls_statusbar32", None)
        return self.text(status) if status else ""

    def close(self):
        if self.process.poll() is None:
            self.process.kill()


def test_gui(ctx):
    gui_exe = os.path.join(os.path.dirname(ctx.exe), "TISCSI.exe")
    if not os.path.exists(gui_exe):
        print("  SKIP TISCSI.exe not found next to TISCSICli.exe")
        return
    ini = os.path.join(os.path.dirname(gui_exe), "TISCSI.ini")
    saved_ini = open(ini, "rb").read() if os.path.exists(ini) else None
    gui = Gui(gui_exe, ["--lang=en-US"])
    try:
        main = gui.top_window()
        ctx.check(main is not None and gui.text(main) == "TISCSI", "main window opens")
        gui.set_text(main, Gui.IDC_PORT, str(ctx.port))
        gui.command(main, Gui.IDC_ADD_TARGET)
        add = gui.top_window()
        ctx.check(add is not None and add != main, "Add Target dialog opens")
        gui.set_text(add, Gui.IDC_TARGET_NAME, TARGET)
        gui.command(add, Gui.IDC_CREATE_RAMDISK)
        ram = gui.top_window()
        gui.set_text(ram, Gui.IDC_SIZE_MB, "64")
        gui.command(ram, Gui.IDOK)
        time.sleep(0.5)
        iso, iso_content = make_iso(ctx, "gui.iso", 64)
        gui.command(add, Gui.IDC_ADD_IMAGE)
        image = gui.top_window()
        gui.set_text(image, Gui.IDC_PATH, iso)
        gui.command(image, Gui.IDOK)
        time.sleep(0.5)
        disc, disc_content = make_iso(ctx, "gui-disc.bin", 64)
        gui.command(add, Gui.IDC_ADD_IMAGE)
        image = gui.top_window()
        gui.set_text(image, Gui.IDC_PATH, disc)
        gui.click(image, Gui.IDC_OPTICAL)
        ctx.check(gui.checkbox(image, Gui.IDC_OPTICAL) == (True, True) and
                  gui.checkbox(image, Gui.IDC_READ_ONLY) == (True, False),
                  "the CD/DVD-ROM check box sets and locks Read-only")
        gui.command(image, Gui.IDOK)
        time.sleep(0.5)
        gui.command(add, Gui.IDOK)
        time.sleep(0.5)
        gui.command(main, Gui.IDC_START)
        time.sleep(1.0)
        ctx.check(f":{ctx.port} - 0 active session(s)" in gui.status_text(main), "status bar: listening")

        s = ctx.session()
        status, _, _ = s.full_login(TARGET)
        st, size, _ = s.capacity()
        ctx.check(status == 0 and size == 64 * MiB, "initiator logs in to the target added in the GUI")
        time.sleep(0.8)
        ctx.check("1 active session(s)" in gui.status_text(main), "status bar counts the session")
        data = os.urandom(4096)
        ctx.check(s.write(0, data)[0] == 0 and s.read(0, 8)[1] == data, "RAM disk from the GUI reads and writes")
        st, inquiry, _, _ = s.scsi(bytes([0x12, 0, 0, 0, 96, 0]), lun=1, read_len=96)
        st2, sector, _, _ = s.scsi(cdb10(0x28, 16, 1), lun=1, read_len=2048)
        ctx.check(st == 0 and inquiry[0] & 0x1F == 5 and st2 == 0 and sector == iso_content[16 * 2048:17 * 2048],
                  "an .iso added in the GUI is a CD/DVD-ROM drive")
        st, inquiry, _, _ = s.scsi(bytes([0x12, 0, 0, 0, 96, 0]), lun=2, read_len=96)
        st2, sector, _, _ = s.scsi(cdb10(0x28, 63, 1), lun=2, read_len=2048)
        ctx.check(st == 0 and inquiry[0] & 0x1F == 5 and st2 == 0 and sector == disc_content[63 * 2048:],
                  "a .bin added with the CD/DVD-ROM check box is a CD/DVD-ROM drive")
        s.logout()
        time.sleep(0.8)
        ctx.check("0 active session(s)" in gui.status_text(main), "status bar: session ended")

        # A second target, restricted through Access Control.
        chap_target = "iqn.2026-10.test:gui-chap"
        gui.command(main, Gui.IDC_ADD_TARGET)
        add = gui.top_window()
        gui.set_text(add, Gui.IDC_TARGET_NAME, chap_target)
        gui.command(add, Gui.IDC_CREATE_RAMDISK)
        ram = gui.top_window()
        gui.set_text(ram, Gui.IDC_SIZE_MB, "1")
        gui.command(ram, Gui.IDOK)
        time.sleep(0.5)
        gui.command(add, Gui.IDC_ACCESS)
        access = gui.top_window()
        ctx.check(access not in (None, add), "Access Control dialog opens")
        gui.set_text(access, Gui.IDC_ALLOWED_INITIATORS, f"  {DEFAULT_INITIATOR}\r\n\r\n")
        gui.set_text(access, Gui.IDC_CHAP_USER, "gui-user")
        gui.set_text(access, Gui.IDC_CHAP_SECRET, "too-short")
        gui.command(access, Gui.IDOK)
        box = gui.top_window()
        message = gui.text(gui.user32.GetDlgItem(box, Gui.IDC_STATIC_TEXT)) if box not in (None, access) else ""
        ctx.check("12 to 255 characters" in message, "Access Control refuses a short CHAP secret")
        if box not in (None, access):
            gui.command(box, Gui.IDCANCEL)
        gui.set_text(access, Gui.IDC_ALLOWED_INITIATORS, "not-an-iqn")
        gui.set_text(access, Gui.IDC_CHAP_SECRET, "gui-chap-secret")
        gui.command(access, Gui.IDOK)
        box = gui.top_window()
        message = gui.text(gui.user32.GetDlgItem(box, Gui.IDC_STATIC_TEXT)) if box not in (None, access) else ""
        ctx.check("Invalid iSCSI initiator name: not-an-iqn" in message, "Access Control refuses an invalid initiator name")
        if box not in (None, access):
            gui.command(box, Gui.IDCANCEL)
        gui.set_text(access, Gui.IDC_ALLOWED_INITIATORS, f"  {DEFAULT_INITIATOR}\r\n\r\n")
        gui.command(access, Gui.IDOK)
        ctx.check(gui.top_window() == add and
                  gui.text(gui.user32.GetDlgItem(add, Gui.IDC_ACCESS_SUMMARY)) == "Initiator list + CHAP",
                  "Add Target shows the access settings")
        gui.command(add, Gui.IDOK)
        time.sleep(0.5)
        s = ctx.session()
        ctx.check(s.full_login(chap_target)[0] == 0x0201, "a target added with CHAP in the GUI needs CHAP")
        s.close()
        s = Session(HOST, ctx.port, initiator="iqn.2026-10.test:other-initiator")
        ctx.check(s.chap_login(chap_target, "gui-user", "gui-chap-secret")[0] == 0x0202,
                  "and refuses initiators that are not listed")
        s.close()
        s = ctx.session()
        status, _ = s.chap_login(chap_target, "gui-user", "gui-chap-secret")
        st, size, _ = s.capacity() if status == 0 else (None, 0, 0)
        ctx.check(status == 0 and size == MiB, "the listed initiator logs in with the CHAP name and secret")
        s.logout()

        # File > Save Configuration As, through the common file dialog.
        saved = ctx.path("gui saved.conf")
        gui.command(main, Gui.IDM_FILE_SAVE_CONFIG)
        dialog = gui.top_window()
        name_box = gui.file_dialog_name_box(dialog) if dialog else None
        if name_box:
            buffer = gui.ctypes.create_unicode_buffer(saved)
            gui.user32.SendMessageW(name_box, Gui.WM_SETTEXT, 0, gui.ctypes.addressof(buffer))
            gui.command(dialog, Gui.IDOK)
            time.sleep(1.5)
        if gui.top_window() != main and dialog:
            gui.command(dialog, Gui.IDCANCEL)  # do not leave the dialog open for the checks below
        text = open(saved, encoding="utf-8-sig").read() if os.path.exists(saved) else ""
        ctx.check(f"port = {ctx.port}\n" in text and
                  f"[target {TARGET}]\nram = 64M\nimage = {os.path.basename(iso)}\n"
                  f"cdrom = {os.path.basename(disc)}\n" in text,
                  "File > Save Configuration As writes the targets added in the GUI (paths relative to the file)")
        ctx.check(f"[target {chap_target}]\nallow-initiator = {DEFAULT_INITIATOR}\nchap-user = gui-user\n"
                  "chap-secret = gui-chap-secret\nram = 1M\n" in text,
                  "File > Save Configuration As writes the access settings set in the GUI")

        gui.command(main, Gui.IDM_LANG_FIRST + 1)  # View > Language > 日本語
        japanese = gui.top_window()
        ctx.check(japanese is not None and "待ち受け中" in gui.status_text(japanese),
                  "switching to Japanese rebuilds the window, server keeps running")
        s = ctx.session()
        status, _, _ = s.full_login(TARGET)
        ctx.check(status == 0, "target survives the language switch")
        s.logout()
        gui.command(japanese, Gui.IDM_LANG_FIRST + 5)  # View > Language > Türkçe
        turkish = gui.top_window()
        ctx.check(turkish is not None and "dinleniyor" in gui.status_text(turkish),
                  "switching to Turkish rebuilds the window, server keeps running")
        ctx.check(gui.text(gui.user32.GetDlgItem(turkish, Gui.IDC_START)) == "&Durdur",
                  "Turkish dialog resources are loaded")
        s = ctx.session()
        ctx.check(s.full_login(TARGET)[0] == 0, "target survives the Turkish language switch")
        s.logout()
        gui.user32.PostMessageW(turkish, Gui.WM_CLOSE, 0, 0)
        try:
            gui.process.wait(timeout=10)
            ctx.check(gui.process.returncode == 0, "the window closes and the process exits")
        except subprocess.TimeoutExpired:
            ctx.check(False, "the window closes and the process exits")

        # --config opens the saved configuration and starts the server.
        gui.close()
        gui = Gui(gui_exe, ["--lang=en-US", "--config", saved])
        main = gui.top_window()
        time.sleep(1.0)
        ctx.check(f":{ctx.port} - 0 active session(s)" in gui.status_text(main), "--config starts the server")
        s = ctx.session()
        status, _, _ = s.full_login(TARGET)
        st, size, _ = s.capacity()
        st2, sector, _, _ = s.scsi(cdb10(0x28, 63, 1), lun=2, read_len=2048)
        ctx.check(status == 0 and size == 64 * MiB and st2 == 0 and sector == disc_content[63 * 2048:],
                  "the opened configuration serves the same disks")
        s.logout()
        gui.user32.PostMessageW(main, Gui.WM_CLOSE, 0, 0)
        gui.process.wait(timeout=10)

        # Targets that cannot be opened are listed; the others are served.
        broken = ctx.path("gui broken.conf")
        with open(broken, "w", encoding="utf-8") as f:
            f.write(f"[server]\nport = {ctx.port}\n[target iqn.2026-10.test:missing]\nimage = {ctx.path('missing.img')}\n"
                    f"[target {TARGET}]\nchap-secret = gui-secret-12\nram = 1M\n")
        gui.close()
        gui = Gui(gui_exe, ["--lang=en-US", "--config", broken])
        box = gui.top_window()
        message = gui.text(gui.user32.GetDlgItem(box, Gui.IDC_STATIC_TEXT)) if box else ""
        ctx.check("iqn.2026-10.test:missing" in message and "missing.img" in message,
                  "a target that cannot be opened is reported")
        if box:
            # The OK button of an MB_OK box is IDCANCEL. BM_CLICK can miss a
            # window that is not active, so send the command.
            gui.command(box, Gui.IDCANCEL)
        main = gui.top_window()
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                socket.create_connection((HOST, ctx.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.2)
        s = ctx.session()
        ctx.check(s.full_login(TARGET)[0] == 0x0201, "the GUI applies the configuration's CHAP setting")
        s.close()
        s = ctx.session()
        ctx.check(s.chap_login(TARGET, "gui", "gui-secret-12")[0] == 0,
                  "the other targets of the configuration are served")
        s.logout()
        gui.user32.PostMessageW(main, Gui.WM_CLOSE, 0, 0)
        gui.process.wait(timeout=10)
    finally:
        gui.close()
        if saved_ini is None:
            if os.path.exists(ini):
                os.remove(ini)
        else:
            with open(ini, "wb") as f:
                f.write(saved_ini)


TESTS = [test_cli_errors, test_protocol, test_verify, test_synchronize_cache,
         test_exclusive_image, test_stop_with_sessions, test_image_formats_qemu,
         test_vhdx, test_create_vhd, test_differencing_vhd, test_image_formats_vmware, test_optical_images,
         test_config_files, test_access, test_service, test_service_elevated, test_optical_elevated, test_devices_unprivileged,
         test_devices_elevated, test_dynamic_volumes, test_dynamic_volumes_elevated, test_gui]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=os.path.join(REPO, "bin", "x64", "Debug", "TISCSICli.exe"))
    parser.add_argument("--port", type=int, default=3270)
    parser.add_argument("-k", dest="filter", default="", help="only run tests whose name contains this")
    args = parser.parse_args()
    args.exe = os.path.abspath(args.exe)  # CreateProcess rejects relative paths with '/'
    if not os.path.exists(args.exe):
        sys.exit(f"{args.exe} not found; build the solution first or pass --exe")

    with tempfile.TemporaryDirectory(prefix="tiscsi-tests-") as workdir:
        ctx = Context(args.exe, args.port, workdir)
        for test in TESTS:
            if args.filter not in test.__name__:
                continue
            print(test.__name__)
            try:
                test(ctx)
            except Exception:
                ctx.failures += 1
                traceback.print_exc()
    print(f"\n{ctx.checks} checks, {ctx.failures} failure(s)")
    sys.exit(1 if ctx.failures else 0)


if __name__ == "__main__":
    main()
