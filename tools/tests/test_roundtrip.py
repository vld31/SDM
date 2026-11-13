"""Encode a synthetic capture the way the firmware does and decode it back.

Run:  py -3 tools/tests/test_roundtrip.py
"""
import json
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from ubb import format as F  # noqa: E402
from ubb.decode import Decoder  # noqa: E402


class BlockWriter:
    def __init__(self, seq, ts, session=0x1234, mhash=0xABCD):
        self.seq, self.ts, self.session, self.mhash = seq, ts, session, mhash
        self.last = ts
        self.payload = bytearray()
        self.flags = 0

    def rec(self, tag, t, body: bytes):
        self.payload += bytes([tag]) + F.varint_encode(t - self.last) + body
        self.last = t

    def finish(self):
        hdr0 = F.BLOCK_HDR.pack(F.BLOCK_MAGIC, self.seq, self.ts, 1, self.flags, len(self.payload), self.mhash, self.session, 0)
        crc = F.crc32(bytes(self.payload), F.crc32(hdr0))
        hdr = F.BLOCK_HDR.pack(F.BLOCK_MAGIC, self.seq, self.ts, 1, self.flags, len(self.payload), self.mhash, self.session, crc)
        return hdr + bytes(self.payload)


def main():
    entries = [
        dict(name="g_state", addr=0x20000100, size=4, kind="enum", cls="fast", enum={"0": "BOOT", "1": "IDLE", "2": "RUN"}),
        dict(name="g_count", addr=0x20000104, size=4, kind="uint", cls="fast"),
        dict(name="g_temp", addr=0x20000108, size=4, kind="float", cls="fast"),
        dict(name="SIO.GPIO_IN", addr=0xD0000004, size=4, kind="reg", cls="slow",
             fields=[dict(name="GPIO_IN", bit_offset=0, bit_width=30)]),
    ]
    sidecar = dict(version=2, manifest_hash=0xABCD, elf=None, target="rp2040", entries=entries, regions=[], triggers=[
        dict(spec="g_count>3:snap", id=1, op=2, flags=1, value=3)])

    def kf(state, count, temp, gpio):
        return struct.pack("<IIfI", state, count, temp, gpio)

    t = 1_000_000
    w = BlockWriter(1, t)
    w.rec(F.REC_SESSION, t, struct.pack("<III", 0x0002, 0x1234, 0xABCD) + b"\x01")
    w.rec(F.REC_STATUS, t, bytes([1]) + struct.pack("<I", 0x0BC12477))
    w.rec(F.REC_RESET_CAUSE, t, struct.pack("<II", 1 << 8, 0))
    w.rec(F.REC_KEYFRAME, t + 10, kf(1, 0, 21.5, 0x400))
    w.flags |= F.BLKF_HAS_KEYFRAME
    for i in range(1, 6):
        t += 1000
        w.rec(F.REC_CHANGE, t, F.varint_encode(1) + struct.pack("<I", i))
    t += 500
    w.rec(F.REC_CHANGE, t, F.varint_encode(0) + struct.pack("<I", 2))
    w.rec(F.REC_CHANGE, t + 1, F.varint_encode(3) + struct.pack("<I", 0x401))
    w.rec(F.REC_TRIGGER, t + 2, bytes([0]) + F.varint_encode(1) + struct.pack("<I", 4))
    w.rec(F.REC_PCSAMPLE, t + 3, struct.pack("<I", 0x100001F0))
    t += 2000
    regs = [i for i in range(13)] + [0x20041F00, 0xFFFFFFF9, 0x100002C4, 0x21000003]
    frame = [0, 1, 2, 3, 12, 0x10000301, 0x10000212, 0x61000000]
    w.rec(F.REC_STATUS, t, bytes([5]) + struct.pack("<I", 0x00030003))
    w.rec(F.REC_FAULT, t, bytes([0, 1]) + struct.pack("<III", 0x30003, 8, 3) + struct.pack("<17I", *regs) + struct.pack("<8I", *frame))
    blob = bytes(range(256)) * 4
    w.rec(F.REC_SNAPSHOT, t + 100, struct.pack("<IH", 0x20041000, len(blob)) + blob)
    w.rec(F.REC_STATUS, t + 200, bytes([10]) + struct.pack("<I", len(blob)))
    w.rec(F.REC_STATUS, t + 300, bytes([6]) + struct.pack("<I", 300))
    w.rec(F.REC_LINKSTAT, t + 400, struct.pack("<5I", 50000, 1, 0, 5, 0))
    b1 = w.finish()

    w2 = BlockWriter(3, t + 1_000_000)   # seq 2 missing -> gap
    w2.rec(F.REC_KEYFRAME, t + 1_000_000, kf(1, 9, 22.0, 0x400))
    w2.rec(F.REC_STATUS, t + 1_000_100, bytes([3]) + struct.pack("<I", 1 << 25))
    b2 = w2.finish()

    tmp = tempfile.mkdtemp(prefix="ubb_test_")
    cap = os.path.join(tmp, "capture.ubb")
    with open(cap, "wb") as f:
        f.write(b1 + b"\xff" * 7 + b2)      # junk between blocks must be skipped
    side = os.path.join(tmp, "manifest.json")
    with open(side, "w") as f:
        json.dump(sidecar, f)

    d = Decoder(cap, side, os.path.join(tmp, "out")).run()
    files = d.write_all()
    kinds = [e["kind"] for e in d.events]
    assert d.stats["blocks"] == 2, d.stats
    # 5 count changes, 1 state change, and 4 disagreements with the second keyframe (state, count, temp, gpio)
    assert kinds.count("change") == 5 + 1 + 4, kinds
    assert kinds.count("reg") == 1
    assert kinds.count("fault") == 1 and d.stats["faults"] == 1
    assert kinds.count("gap") == 1
    assert len(d.snapshots) == 1 and d.snapshots[0]["regions"][0]["len"] == 1024
    assert any("TRIGGER" in e["text"] for e in d.events)
    assert any(e["kind"] == "reset_cause" and "POR/BOR" in e["causes"] for e in d.events)
    assert d.values[0] == 1 and d.values[1] == 9
    # manifest builder round trip
    data, h = F.build_manifest([(0x20000100, 4, 0), (0xD0000004, 4, 1)], [(0x20000000, 0x1000)], [(0, 0, 1, 5)],
                               target=1, level=1, on_fault=0, pc_sampling=10, swd_hz=4000000, fast_period_us=1000,
                               slow_period_us=10000, keyframe_us=1000000, snapshot_on_fault=1, snapshot_on_reset=0)
    assert len(data) == F.MANIFEST_HDR.size + 2 * 8 + 8 + 8
    hdr = F.MANIFEST_HDR.unpack_from(data)
    crc_field = hdr[4]
    zeroed = data[:12] + b"\0\0\0\0" + data[16:]
    assert F.crc32(zeroed) == crc_field
    print("roundtrip OK:", len(d.events), "events;", "outputs:", *files, sep="\n  ")
    with open(files[1], encoding="utf-8") as f:
        print(f.read())

    # With the demo ELF present, check that fault addresses map to function + file:line.
    elf = os.path.join(os.path.dirname(os.path.dirname(HERE)), "target_demo", "build", "ubb_target_demo.elf")
    if os.path.isfile(elf):
        from ubb.elfinfo import ElfInfo
        e = ElfInfo(elf)
        main_addr = e.symbol("main") & ~1
        d2 = Decoder(cap, side, os.path.join(tmp, "out2"), elf_path=elf)
        d2.run()
        desc = e.describe_addr(main_addr + 20)
        assert "main()" in desc and ":" in desc, desc
        print("ELF symbolisation OK:", desc)


if __name__ == "__main__":
    main()
