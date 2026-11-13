"""Binary formats shared with firmware/src/ubb_format.h."""
import struct
import zlib

BLOCK_SIZE = 4096
BLOCK_MAGIC = 0x58424255  # "UBBX"
BLOCK_HDR = struct.Struct("<IIQBBHIII")  # magic, seq, ts_us, type, flags, len, manifest_hash, session_id, crc
BLOCK_HDR_LEN = 32
assert BLOCK_HDR.size == BLOCK_HDR_LEN

BLKF_HAS_KEYFRAME = 0x01
BLKF_AFTER_GAP = 0x02

# record tags
REC_KEYFRAME = 0x01
REC_CHANGE = 0x02
REC_PCSAMPLE = 0x03
REC_TARGET_TIME = 0x04
REC_STATUS = 0x05
REC_FAULT = 0x06
REC_SNAPSHOT = 0x07
REC_RESET_CAUSE = 0x08
REC_MARK = 0x09
REC_LINKSTAT = 0x0A
REC_SESSION = 0x0B
REC_CORE1 = 0x0C
REC_TRIGGER = 0x0D

STATUS_NAMES = {
    1: "attached", 2: "lost", 3: "reset_seen", 4: "lockup", 5: "halted", 6: "resumed",
    7: "swd_error", 8: "overrun", 9: "armed", 10: "snapshot_done", 11: "rec_start",
    12: "rec_stop", 13: "held", 14: "sleeping", 15: "stuck", 16: "disarmed",
}
FAULT_REASONS = {1: "hardfault", 2: "lockup", 3: "bkpt", 4: "mark", 5: "trigger", 6: "reset"}

# manifest
MANIFEST_MAGIC = 0x4D424255  # "UBBM"
MANIFEST_VERSION = 2
MANIFEST_HDR = struct.Struct("<IHHIIIBBBBIIIIIIHHHH")
# magic, version, header_len, total_len, crc, elf_hash, target, level, on_fault, pc_sampling,
# swd_hz, fast_period_us, slow_period_us, keyframe_us, snapshot_on_fault, snapshot_on_reset,
# n_entries, n_regions, n_triggers, reserved
ENTRY = struct.Struct("<IBBH")      # addr, size, cls, id
REGION = struct.Struct("<II")       # addr, len
TRIGGER = struct.Struct("<HBBI")    # id, op, flags, value

CLASS_FAST, CLASS_SLOW = 0, 1
LEVEL_PASSIVE, LEVEL_ARMED = 0, 1
ONFAULT_RESUME, ONFAULT_HOLD = 0, 1
TGT_RP2040, TGT_RP2350 = 1, 2
TRIG_OPS = {"==": 0, "!=": 1, ">": 2, "<": 3, "change": 4, "bitset": 5}
TRIGF_SNAPSHOT, TRIGF_ONCE = 0x01, 0x02

# host protocol
CMD_PING, CMD_STATUS, CMD_READ_SECTOR, CMD_ERASE_LOG = 1, 2, 3, 4
CMD_MANIFEST_BEGIN, CMD_MANIFEST_DATA, CMD_MANIFEST_END = 5, 6, 7
CMD_MARK, CMD_REC_START, CMD_REC_STOP, CMD_CLEAR_EVENT, CMD_READ_MANIFEST, CMD_FLUSH = 8, 9, 10, 11, 12, 13
RSP_OK, RSP_ERR, RSP_INFO, RSP_DATA = 0x80, 0x81, 0x82, 0x83
INFO_RSP = struct.Struct("<BBIIBBIIII")
STATUS_RSP = struct.Struct("<BBBBIIIIIIIIIIIIQII")


def crc32(data: bytes, crc: int = 0) -> int:
    return zlib.crc32(data, crc) & 0xFFFFFFFF


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def varint_encode(v: int) -> bytes:
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def varint_decode(buf: bytes, pos: int):
    shift = 0
    v = 0
    while True:
        b = buf[pos]
        pos += 1
        v |= (b & 0x7F) << shift
        if not b & 0x80:
            return v, pos
        shift += 7


def parse_block(raw: bytes):
    """Return (hdr dict, payload bytes) or None if the block is not valid."""
    if len(raw) < BLOCK_HDR_LEN:
        return None
    f = BLOCK_HDR.unpack_from(raw, 0)
    magic, seq, ts, btype, flags, length, mhash, sid, crc = f
    if magic != BLOCK_MAGIC or length > BLOCK_SIZE - BLOCK_HDR_LEN or len(raw) < BLOCK_HDR_LEN + length:
        return None
    hdr0 = BLOCK_HDR.pack(magic, seq, ts, btype, flags, length, mhash, sid, 0)
    if crc32(raw[BLOCK_HDR_LEN:BLOCK_HDR_LEN + length], crc32(hdr0)) != crc:
        return None
    return (dict(seq=seq, ts_us=ts, type=btype, flags=flags, len=length, manifest_hash=mhash, session_id=sid),
            raw[BLOCK_HDR_LEN:BLOCK_HDR_LEN + length])


def build_manifest(entries, regions, triggers, *, target, level, on_fault, pc_sampling, swd_hz,
                   fast_period_us, slow_period_us, keyframe_us, snapshot_on_fault, snapshot_on_reset, elf_hash=0):
    """entries: list of (addr, size, cls); regions: list of (addr, len); triggers: list of (id, op, flags, value)."""
    body = b"".join(ENTRY.pack(a, s, c, i) for i, (a, s, c) in enumerate(entries))
    body += b"".join(REGION.pack(a, l) for a, l in regions)
    body += b"".join(TRIGGER.pack(i, o, f, v & 0xFFFFFFFF) for i, o, f, v in triggers)
    total = MANIFEST_HDR.size + len(body)

    def hdr(crc):
        return MANIFEST_HDR.pack(MANIFEST_MAGIC, MANIFEST_VERSION, MANIFEST_HDR.size, total, crc, elf_hash,
                                 target, level, on_fault, pc_sampling, swd_hz, fast_period_us, slow_period_us,
                                 keyframe_us, snapshot_on_fault, snapshot_on_reset,
                                 len(entries), len(regions), len(triggers), 0)
    crc = crc32(body, crc32(hdr(0)))
    data = hdr(crc) + body
    return data, crc32(data)
