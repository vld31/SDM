"""USB serial link to the probe."""
import struct
import time

import serial
import serial.tools.list_ports

from . import format as F

PROBE_VID = 0x2E8A
PROBE_PIDS = {0x000A, 0x0009}   # pico-sdk stdio_usb CDC


class ProbeError(Exception):
    pass


def find_port(explicit=None):
    if explicit:
        return explicit
    cands = []
    for p in serial.tools.list_ports.comports():
        if p.vid == PROBE_VID and (p.pid in PROBE_PIDS or "Black Box" in (p.product or "")):
            cands.append(p.device)
    if not cands:
        raise ProbeError("no probe found (Raspberry Pi USB CDC); pass --port")
    return sorted(cands)[0]


class Probe:
    def __init__(self, port=None, timeout=3.0):
        self.port = find_port(port)
        self.ser = serial.Serial(self.port, 115200, timeout=timeout, write_timeout=timeout)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def _send(self, cmd, payload=b""):
        hdr = bytes([cmd, len(payload) & 0xFF, len(payload) >> 8])
        crc = F.crc16_ccitt(hdr + payload)
        self.ser.write(b"UB" + hdr + payload + struct.pack("<H", crc))

    def _recv(self, timeout=None):
        if timeout is not None:
            self.ser.timeout = timeout
        deadline = time.time() + (self.ser.timeout or 3.0)
        # resync on 'U','B'
        state = 0
        while time.time() < deadline:
            b = self.ser.read(1)
            if not b:
                continue
            if state == 0 and b == b"U":
                state = 1
            elif state == 1:
                if b == b"B":
                    break
                state = 1 if b == b"U" else 0
        else:
            raise ProbeError("timeout waiting for response")
        hdr = self.ser.read(3)
        if len(hdr) < 3:
            raise ProbeError("short response header")
        rtype, length = hdr[0], hdr[1] | (hdr[2] << 8)
        payload = b""
        while len(payload) < length:
            chunk = self.ser.read(length - len(payload))
            if not chunk:
                raise ProbeError("short response payload")
            payload += chunk
        crc = self.ser.read(2)
        if len(crc) < 2 or struct.unpack("<H", crc)[0] != F.crc16_ccitt(hdr + payload):
            raise ProbeError("response CRC mismatch")
        if rtype == F.RSP_ERR:
            raise ProbeError(f"probe error {payload[0]}: {payload[1:].decode(errors='replace')}")
        return rtype, payload

    def cmd(self, cmd, payload=b"", timeout=None):
        self._send(cmd, payload)
        return self._recv(timeout)

    def ping(self):
        _, p = self.cmd(F.CMD_PING)
        f = F.INFO_RSP.unpack_from(p)
        keys = ["fw_major", "fw_minor", "session_id", "manifest_hash", "manifest_valid", "target_type",
                "log_offset", "log_sectors", "block_size", "sector_size"]
        return dict(zip(keys, f))

    def status(self):
        _, p = self.cmd(F.CMD_STATUS)
        f = F.STATUS_RSP.unpack_from(p)
        keys = ["recording", "link_state", "level", "event_latched", "dpidr", "last_dhcsr", "frames", "words_per_s",
                "wait_acks", "fault_acks", "overruns", "blocks_written", "next_seq", "ram_blocks_used",
                "faults_captured", "resets_seen", "uptime_us", "first_valid_sector", "valid_sectors"]
        return dict(zip(keys, f))

    def read_sector(self, index):
        _, p = self.cmd(F.CMD_READ_SECTOR, struct.pack("<I", index), timeout=5.0)
        return p

    def flush(self):
        self.cmd(F.CMD_FLUSH, timeout=10.0)

    def erase_log(self):
        self.cmd(F.CMD_ERASE_LOG, timeout=180.0)

    def upload_manifest(self, data: bytes, progress=None):
        self.cmd(F.CMD_MANIFEST_BEGIN, struct.pack("<I", len(data)))
        for off in range(0, len(data), 1024):
            chunk = data[off:off + 1024]
            self.cmd(F.CMD_MANIFEST_DATA, struct.pack("<I", off) + chunk)
            if progress:
                progress(off + len(chunk), len(data))
        self.cmd(F.CMD_MANIFEST_END, timeout=20.0)

    def read_manifest(self):
        out = b""
        while True:
            _, p = self.cmd(F.CMD_READ_MANIFEST, struct.pack("<IH", len(out), 4096))
            if not p:
                return out
            out += p

    def mark(self, mark_id=1, snapshot=False):
        self.cmd(F.CMD_MARK, struct.pack("<IB", mark_id, 1 if snapshot else 0))

    def rec_start(self):
        self.cmd(F.CMD_REC_START)

    def rec_stop(self):
        self.cmd(F.CMD_REC_STOP)

    def clear_event(self):
        self.cmd(F.CMD_CLEAR_EVENT)

    def pull(self, progress=None, min_seq=0):
        """Read every sector, keep valid blocks, return list of (hdr, raw) sorted by seq."""
        info = self.ping()
        self.flush()
        blocks = []
        n = info["log_sectors"]
        for i in range(n):
            raw = self.read_sector(i)
            parsed = F.parse_block(raw)
            if parsed and parsed[0]["seq"] >= min_seq:
                blocks.append((parsed[0], raw[:F.BLOCK_HDR_LEN + parsed[0]["len"]]))
            if progress and (i % 16 == 0 or i == n - 1):
                progress(i + 1, n, len(blocks))
        blocks.sort(key=lambda b: b[0]["seq"])
        return info, blocks
