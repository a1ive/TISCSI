# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 A1ive
"""Minimal iSCSI initiator for exercising TISCSI from tests.

Implements just enough of RFC 3720 (initiator side) to log in, run SCSI
commands with R2T / immediate / unsolicited data, send text, NOP and logout
requests. No digests, one connection per session.
"""
import socket
import struct

DEFAULT_INITIATOR = "iqn.2026-10.test:initiator"


def kv(params):
    return b"".join(f"{k}={v}".encode() + b"\0" for k, v in params)


def parse_kv(data):
    out = {}
    for entry in data.split(b"\0"):
        if b"=" in entry:
            k, v = entry.split(b"=", 1)
            out.setdefault(k.decode(), []).append(v.decode())
    return out


def lun_bytes(lun):
    return struct.pack(">H", lun) + b"\0" * 6


def sense_key(payload):
    """Sense key from a SCSI Response data segment (2-byte length prefix)."""
    return payload[4] & 0x0F if len(payload) > 4 else None


def asc(payload):
    return payload[14] if len(payload) > 14 else None


def cdb10(opcode, lba, blocks):
    return struct.pack(">BBIBHB", opcode, 0, lba, 0, blocks, 0)


def cdb16(opcode, lba, blocks):
    return struct.pack(">BBQIBB", opcode, 0, lba, blocks, 0, 0)


class Session:
    def __init__(self, host, port, isid=0x23D000000001, initiator=DEFAULT_INITIATOR, timeout=10):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.isid = isid
        self.initiator = initiator
        self.itt = 1
        self.cmdsn = 1
        self.expstatsn = 0
        self.params = {}

    # ---- transport ------------------------------------------------------

    def send(self, bhs, data=b""):
        bhs = bytearray(bhs)
        bhs[5:8] = len(data).to_bytes(3, "big")
        self.sock.sendall(bytes(bhs) + data + b"\0" * ((-len(data)) % 4))

    def recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("connection closed by target")
            buf += chunk
        return buf

    def recv(self):
        bhs = self.recv_exact(48)
        ahs = bhs[4] * 4
        dsl = int.from_bytes(bhs[5:8], "big")
        rest = self.recv_exact(ahs + dsl + ((-(ahs + dsl)) % 4))
        opcode = bhs[0] & 0x3F
        carries_statsn = opcode in (0x21, 0x23, 0x24, 0x26, 0x3F) or (opcode == 0x25 and bhs[1] & 1) or \
            (opcode == 0x20 and struct.unpack(">I", bhs[16:20])[0] != 0xFFFFFFFF)
        if carries_statsn:
            self.expstatsn = struct.unpack(">I", bhs[24:28])[0] + 1
        return bhs, rest[ahs:ahs + dsl]

    def is_closed_by_target(self, timeout=5):
        try:
            self.sock.settimeout(timeout)
            return self.sock.recv(1) == b""
        except OSError:
            return True

    def close(self):
        self.sock.close()

    def next_itt(self):
        self.itt += 1
        return self.itt

    # ---- login ----------------------------------------------------------

    def login(self, params, csg, nsg, transit=True):
        """One Login Request; returns (bhs, status, tsih, response keys)."""
        bhs = bytearray(48)
        bhs[0] = 0x43  # immediate Login Request
        bhs[1] = (0x80 if transit else 0) | (csg << 2) | nsg
        bhs[8:14] = self.isid.to_bytes(6, "big")
        struct.pack_into(">I", bhs, 16, self.next_itt())
        struct.pack_into(">HxxII", bhs, 20, 1, self.cmdsn, self.expstatsn)
        self.send(bhs, kv(params))
        rbhs, data = self.recv()
        status = struct.unpack(">H", rbhs[36:38])[0]
        tsih = struct.unpack(">H", rbhs[14:16])[0]
        return rbhs, status, tsih, parse_kv(data)

    def full_login(self, target=None, extra=(), security_stage=False):
        """Logs in to `target` (or a discovery session if None).

        Returns (status, tsih, security_stage_ok)."""
        base = [("InitiatorName", self.initiator)]
        if target:
            base += [("SessionType", "Normal"), ("TargetName", target)]
        else:
            base += [("SessionType", "Discovery")]
        security_ok = None
        if security_stage:
            _, status, _, resp = self.login(base + [("AuthMethod", "None")], 0, 1)
            security_ok = status == 0 and resp.get("AuthMethod") == ["None"]
            base = []
        ops = [("HeaderDigest", "None"), ("DataDigest", "None")] + list(extra)
        _, status, tsih, resp = self.login(base + ops, 1, 3)
        self.params = {k: v[0] for k, v in resp.items()}
        return status, tsih, security_ok

    # ---- full feature phase ---------------------------------------------

    def scsi(self, cdb, lun=0, read_len=0, write_data=None):
        """Runs a SCSI command; returns (status | "reject", data, r2t_count, final_bhs)."""
        itt = self.next_itt()
        edtl = read_len if write_data is None else len(write_data)
        bhs = bytearray(48)
        bhs[0] = 0x01
        bhs[1] = 0x80 | (0x40 if read_len else 0) | (0x20 if write_data is not None else 0) | 0x01
        bhs[8:16] = lun_bytes(lun)
        struct.pack_into(">IIII", bhs, 16, itt, edtl, self.cmdsn, self.expstatsn)
        bhs[32:32 + len(cdb)] = cdb
        self.cmdsn += 1
        mrdsl = int(self.params.get("MaxRecvDataSegmentLength", 8192))
        first_burst = int(self.params.get("FirstBurstLength", 65536))
        immediate = b""
        if write_data is not None and self.params.get("ImmediateData") == "Yes":
            immediate = write_data[:min(mrdsl, first_burst)]
        self.send(bhs, immediate)
        sent = len(immediate)
        if write_data is not None and self.params.get("InitialR2T") == "No":
            # Unsolicited Data-Out PDUs up to FirstBurstLength
            end = min(first_burst, len(write_data))
            datasn = 0
            while sent < end:
                chunk = write_data[sent:min(end, sent + mrdsl)]
                self.data_out(itt, 0xFFFFFFFF, sent, chunk, datasn, final=(sent + len(chunk) == end), lun=lun)
                datasn += 1
                sent += len(chunk)
        return self.collect(itt, write_data, lun)

    def send_command_only(self, cdb, read_len, lun=0):
        """Sends a read command without waiting; returns its ITT (for pipelining)."""
        itt = self.next_itt()
        bhs = bytearray(48)
        bhs[0] = 0x01
        bhs[1] = 0x80 | 0x40 | 0x01
        bhs[8:16] = lun_bytes(lun)
        struct.pack_into(">IIII", bhs, 16, itt, read_len, self.cmdsn, self.expstatsn)
        bhs[32:32 + len(cdb)] = cdb
        self.cmdsn += 1
        self.send(bhs)
        return itt

    def data_out(self, itt, ttt, offset, chunk, datasn, final, lun=0):
        bhs = bytearray(48)
        bhs[0] = 0x05
        bhs[1] = 0x80 if final else 0
        bhs[8:16] = lun_bytes(lun)
        struct.pack_into(">II", bhs, 16, itt, ttt)
        struct.pack_into(">I", bhs, 28, self.expstatsn)
        struct.pack_into(">II", bhs, 36, datasn, offset)
        self.send(bhs, chunk)

    def collect(self, itt, write_data=None, lun=0):
        data = bytearray()
        r2ts = 0
        mrdsl = int(self.params.get("MaxRecvDataSegmentLength", 8192))
        while True:
            bhs, payload = self.recv()
            op = bhs[0] & 0x3F
            if op == 0x31:  # R2T: answer with Data-Out PDUs
                r2ts += 1
                ttt, = struct.unpack(">I", bhs[20:24])
                offset, length = struct.unpack(">II", bhs[40:48])
                datasn = 0
                pos = offset
                while pos < offset + length:
                    chunk = write_data[pos:min(offset + length, pos + mrdsl)]
                    self.data_out(itt, ttt, pos, chunk, datasn, final=(pos + len(chunk) == offset + length), lun=lun)
                    datasn += 1
                    pos += len(chunk)
            elif op == 0x25:  # Data-In
                offset, = struct.unpack(">I", bhs[40:44])
                if offset != len(data):
                    raise AssertionError("Data-In out of order")
                data += payload
                if bhs[1] & 0x01:
                    return bhs[3], bytes(data), r2ts, bhs
            elif op == 0x21:  # SCSI Response
                return bhs[3], payload, r2ts, bhs
            elif op == 0x3F:  # Reject
                return "reject", payload, r2ts, bhs
            else:
                raise AssertionError(f"unexpected opcode 0x{op:02x}")

    def read(self, lba, blocks, lun=0, block_size=512):
        return self.scsi(cdb16(0x88, lba, blocks), lun=lun, read_len=blocks * block_size)

    def write(self, lba, data, lun=0, block_size=512):
        return self.scsi(cdb16(0x8A, lba, len(data) // block_size), lun=lun, write_data=data)

    def capacity(self, lun=0):
        status, data, _, _ = self.scsi(bytes([0x9E, 0x10] + [0] * 8 + [0, 0, 0, 32, 0, 0]), lun=lun, read_len=32)
        last, block_size = struct.unpack(">QI", data[:12])
        return status, (last + 1) * block_size, block_size

    def text(self, params):
        bhs = bytearray(48)
        bhs[0] = 0x04
        bhs[1] = 0x80
        struct.pack_into(">IIII", bhs, 16, self.next_itt(), 0xFFFFFFFF, self.cmdsn, self.expstatsn)
        self.cmdsn += 1
        self.send(bhs, kv(params))
        rbhs, data = self.recv()
        return rbhs, parse_kv(data)

    def nop(self, payload=b"ping"):
        itt = self.next_itt()
        bhs = bytearray(48)
        bhs[0] = 0x40  # immediate NOP-Out
        bhs[1] = 0x80
        struct.pack_into(">IIII", bhs, 16, itt, 0xFFFFFFFF, self.cmdsn, self.expstatsn)
        self.send(bhs, payload)
        rbhs, data = self.recv()
        return rbhs, data, itt

    def raw_request(self, opcode, flags=0x80):
        """Sends a bare request PDU (e.g. an unsupported opcode) and returns the reply."""
        bhs = bytearray(48)
        bhs[0] = opcode
        bhs[1] = flags
        struct.pack_into(">IIII", bhs, 16, self.next_itt(), 0xFFFFFFFF, self.cmdsn, self.expstatsn)
        self.send(bhs)
        return self.recv()

    def logout(self):
        """Closes the session; returns (response code, connection closed by target)."""
        bhs = bytearray(48)
        bhs[0] = 0x46
        bhs[1] = 0x80  # reason: close the session
        struct.pack_into(">IHxxII", bhs, 16, self.next_itt(), 1, self.cmdsn, self.expstatsn)
        self.send(bhs)
        rbhs, _ = self.recv()
        closed = self.is_closed_by_target()
        self.sock.close()
        return rbhs[2], closed
