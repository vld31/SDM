"""Decode a pulled capture into events.jsonl, trail.txt, timeline.vcd and trace.json.

The recorder stores *what happened*; this module turns it into a story:
every change is dated on the probe clock, named from the manifest sidecar,
and faults are mapped back to source lines with the ELF.
"""
import json
import os
import struct
from collections import defaultdict
from typing import Dict, List, Optional

from . import format as F

try:
    from .elfinfo import ElfInfo
except Exception:  # pyelftools missing
    ElfInfo = None


def iter_blocks(data: bytes):
    pos = 0
    while pos + F.BLOCK_HDR_LEN <= len(data):
        hdr = F.BLOCK_HDR.unpack_from(data, pos)
        length = hdr[5]
        chunk = data[pos:pos + F.BLOCK_HDR_LEN + length]
        parsed = F.parse_block(chunk)
        if parsed is None:
            pos += 1  # resync
            continue
        yield parsed
        pos += F.BLOCK_HDR_LEN + length


def _region(addr: int) -> str:
    if addr < 0x4000:
        return "boot ROM"
    if 0x10000000 <= addr < 0x11000000:
        return "flash (XIP)"
    if 0x20000000 <= addr < 0x20042000 or 0x20000000 <= addr < 0x20082000:
        return "SRAM"
    if 0x40000000 <= addr < 0x60000000:
        return "peripheral space"
    if 0xD0000000 <= addr < 0xD0001000:
        return "SIO"
    return "unmapped"


def _fmt_time(us: int) -> str:
    s, rem = divmod(us, 1_000_000)
    return f"{s:6d}.{rem:06d}"


class Entry:
    def __init__(self, i, m):
        self.id = i
        self.name = m["name"]
        self.addr = m["addr"]
        self.size = m["size"]
        self.kind = m.get("kind", "raw")
        self.ctype = m.get("ctype", "")
        self.enum = {int(k): v for k, v in (m.get("enum") or {}).items()}
        self.bit_offset = m.get("bit_offset")
        self.bit_size = m.get("bit_size")
        self.fields = m.get("fields") or []
        self.cls = m.get("cls", "fast")

    def raw_to_value(self, raw: bytes):
        if self.kind == "float":
            return struct.unpack("<f" if self.size == 4 else "<d", raw)[0]
        if self.kind == "char":
            if self.size == 1:
                return raw[0]
            return raw.split(b"\0", 1)[0].decode("latin-1")
        v = int.from_bytes(raw, "little", signed=(self.kind == "int"))
        if self.bit_size is not None:
            v = (int.from_bytes(raw, "little") >> self.bit_offset) & ((1 << self.bit_size) - 1)
        return v

    def describe(self, v) -> str:
        if self.kind == "enum":
            return f"{self.enum.get(v, '?')} ({v})"
        if self.kind == "bool":
            return "true" if v else "false"
        if self.kind == "pointer":
            return f"0x{v:08X}"
        if self.kind == "float":
            return f"{v:.6g}"
        if self.kind == "reg":
            return f"0x{v:08X}"
        if self.kind == "char" and isinstance(v, str):
            return repr(v)
        return str(v)

    def reg_fields(self, v) -> Dict[str, str]:
        out = {}
        for f in self.fields:
            fv = (v >> f["bit_offset"]) & ((1 << f["bit_width"]) - 1)
            enum = f.get("enum") or {}
            nm = enum.get(str(fv))
            out[f["name"]] = f"{nm}({fv})" if nm else (f"0x{fv:X}" if f["bit_width"] > 4 else str(fv))
        return out


RESET_BITS_RP2040 = {8: "POR/BOR", 16: "RUN pin", 24: "debugger (PSM restart)"}
RESET_BITS_RP2350 = {16: "RUN pin", 24: "debugger (RESCUE)", 28: "double-tap"}


def decode_reset_cause(target, chip_reset, wdog_reason):
    bits = RESET_BITS_RP2350 if target == "rp2350" else RESET_BITS_RP2040
    causes = [name for bit, name in bits.items() if chip_reset & (1 << bit)]
    if wdog_reason & 1:
        causes.append("watchdog timer")
    if wdog_reason & 2:
        causes.append("watchdog force")
    return causes or ["unknown"]


class Decoder:
    def __init__(self, capture_path: str, sidecar_path: Optional[str], out_dir: str, elf_path: Optional[str] = None):
        with open(capture_path, "rb") as f:
            self.data = f.read()
        self.sidecar = None
        if sidecar_path and os.path.isfile(sidecar_path):
            with open(sidecar_path) as f:
                self.sidecar = json.load(f)
        self.entries: List[Entry] = [Entry(i, m) for i, m in enumerate(self.sidecar["entries"])] if self.sidecar else []
        self.target = self.sidecar.get("target", "rp2040") if self.sidecar else "rp2040"
        self.out_dir = out_dir
        os.makedirs(out_dir, exist_ok=True)
        self.elf = None
        elf_path = elf_path or (self.sidecar.get("elf") if self.sidecar else None)
        if elf_path and os.path.isfile(elf_path) and ElfInfo is not None:
            try:
                self.elf = ElfInfo(elf_path)
            except Exception as e:  # pragma: no cover
                print(f"warning: cannot open ELF {elf_path}: {e}")
        self.events: List[dict] = []
        self.values: Dict[int, object] = {}
        self.snapshots: List[dict] = []
        self.cur_snapshot = None
        self.warnings: List[str] = []
        self.sessions = defaultdict(lambda: dict(blocks=0, first=None, last=None, manifest_hash=None))
        self.cur_func = None
        self.pc_samples = []
        self.ttime_pairs = []
        self.stats = dict(blocks=0, records=0, gaps=0, changes=0, faults=0)
        # The probe clock restarts at every probe power-up, so a capture can hold several "boots".
        self.boot = 0
        self.boot_base = 0            # offset that places this boot after the previous one on a shared axis
        self.boots = []               # per boot: dict(first, last, blocks, session)
        self.last_core1_state = None

    # ---- event emission -------------------------------------------------------
    def emit(self, t, kind, **kw):
        ev = dict(t_us=t, t=_fmt_time(t).strip(), kind=kind, boot=self.boot, t_abs=self.boot_base + t)
        ev.update(kw)
        self.events.append(ev)
        return ev

    # ---- record handlers ---------------------------------------------------------
    def handle_keyframe(self, t, p):
        pos = 0
        for e in self.entries:
            raw = p[pos:pos + e.size]
            pos += e.size
            if len(raw) < e.size:
                self.warnings.append(f"short keyframe at {t}")
                return
            v = e.raw_to_value(raw)
            old = self.values.get(e.id)
            if old is None:
                self.emit(t, "initial", name=e.name, value=v, text=e.describe(v))
            elif old != v:
                # a keyframe that disagrees with our model means we missed a change (gap)
                self.emit(t, "change", name=e.name, old=old, value=v, text=f"{e.describe(old)} -> {e.describe(v)} (from keyframe)")
                self.stats["changes"] += 1
            self.values[e.id] = v

    def handle_change(self, t, p):
        eid, pos = F.varint_decode(p, 0)
        if eid >= len(self.entries):
            self.warnings.append(f"change for unknown entry {eid} at {t} (manifest mismatch?)")
            return
        e = self.entries[eid]
        raw = p[pos:pos + e.size]
        v = e.raw_to_value(raw)
        old = self.values.get(e.id)
        self.values[e.id] = v
        self.stats["changes"] += 1
        if e.kind == "reg":
            fo = e.reg_fields(old) if isinstance(old, int) else {}
            fn = e.reg_fields(v)
            diff = {k: f"{fo.get(k, '?')} -> {fn[k]}" for k in fn if fo.get(k) != fn[k]}
            if e.name.endswith("GPIO_IN") or e.name.endswith("GPIO_OUT") and isinstance(old, int):
                x = (old ^ v) if isinstance(old, int) else v
                pins = [f"GP{i}{'↑' if v >> i & 1 else '↓'}" for i in range(32) if x >> i & 1]
                diff = {"pins": " ".join(pins)}
            self.emit(t, "reg", name=e.name, old=old, value=v, fields=diff,
                      text=f"{e.describe(old) if old is not None else '?'} -> {e.describe(v)} " + " ".join(f"{k}={d}" for k, d in diff.items()))
        else:
            self.emit(t, "change", name=e.name, old=old, value=v,
                      text=f"{e.describe(old) if old is not None else '?'} -> {e.describe(v)}")

    def handle_pc(self, t, p):
        pc = struct.unpack_from("<I", p)[0]
        fn = self.elf.function_at(pc) if self.elf else None
        self.pc_samples.append((t, pc, fn))
        if fn != self.cur_func:
            self.cur_func = fn
            self.emit(t, "pc", pc=pc, function=fn, text=f"executing {fn or '?'}() at 0x{pc:08X}")

    def handle_status(self, t, p):
        code = p[0]
        arg = struct.unpack_from("<I", p, 1)[0]
        name = F.STATUS_NAMES.get(code, f"status{code}")
        text = name
        if name == "attached":
            text = f"target attached, DPIDR=0x{arg:08X}"
        elif name == "lost":
            text = f"target lost (SWD error {arg})"
            self.values.clear()
            self.cur_func = None
        elif name == "reset_seen":
            text = f"target RESET observed (DHCSR=0x{arg:08X})"
        elif name == "lockup":
            text = f"core LOCKUP (DHCSR=0x{arg:08X})"
        elif name == "halted":
            text = f"core halted by debug event (DHCSR=0x{arg:08X})"
        elif name == "resumed":
            text = f"core released after post-mortem capture ({arg} us frozen)"
        elif name == "held":
            text = "core left halted (hold policy)"
        elif name == "overrun":
            text = f"recorder overrun: {arg} records dropped"
            self.stats["gaps"] += 1
        elif name == "armed":
            text = f"fault catch armed (DEMCR=0x{arg:08X})"
        elif name == "snapshot_done":
            text = f"memory snapshot captured ({arg} bytes)"
            if self.cur_snapshot:
                self.finish_snapshot()
        elif name == "stuck":
            text = (f"core halted again right after release (fault handler loops on BKPT?): "
                    f"no further captures (DHCSR=0x{arg:08X})")
        elif name == "disarmed":
            text = "fault catch disarmed: target runs naturally again (re-armed on next reset)"
        elif name == "rec_start":
            text = "recording started"
        elif name == "rec_stop":
            text = "recording stopped"
        self.emit(t, "status", status=name, arg=arg, text=text)

    def handle_fault(self, t, p):
        core, reason = p[0], p[1]
        dhcsr, dfsr, icsr = struct.unpack_from("<III", p, 2)
        regs = list(struct.unpack_from("<17I", p, 14))
        frame = list(struct.unpack_from("<8I", p, 14 + 68))
        names = [f"r{i}" for i in range(13)] + ["sp", "lr", "pc", "xpsr"]
        rd = dict(zip(names, regs))
        why = F.FAULT_REASONS.get(reason, str(reason))
        exc = regs[16] & 0x1FF
        exc_name = {0: "thread", 2: "NMI", 3: "HardFault", 11: "SVCall", 14: "PendSV", 15: "SysTick"}.get(exc, f"IRQ{exc-16}" if exc >= 16 else str(exc))
        info = dict(core=core, reason=why, dhcsr=dhcsr, dfsr=dfsr, icsr=icsr, regs=rd,
                    stacked=dict(zip(["r0", "r1", "r2", "r3", "r12", "lr", "pc", "xpsr"], frame)),
                    exception=exc_name)
        dfsr_bits = [n for b, n in {0: "HALTED", 1: "BKPT", 2: "DWTTRAP", 3: "VCATCH", 4: "EXTERNAL"}.items() if dfsr >> b & 1]
        info["dfsr_bits"] = dfsr_bits
        if self.elf:
            info["pc_src"] = self.elf.describe_addr(regs[15])
            info["lr_src"] = self.elf.describe_addr(regs[14])
            if why in ("hardfault", "lockup") or exc == 3:
                info["fault_site"] = self.elf.describe_addr(frame[6])
                info["fault_return"] = self.elf.describe_addr(frame[5])
        self.stats["faults"] += 1
        site = info.get("fault_site") or info.get("pc_src") or f"pc=0x{regs[15]:08X}"
        text = f"{why.upper()} on core {core} in {exc_name}: {site}"
        if exc == 3 and sum(frame):
            # The hardware-stacked frame holds the state at the moment the fault was taken.
            spc, slr, sxpsr = frame[6], frame[5], frame[7]
            diag = [f"fault taken while executing/branching to 0x{spc:08X} ({_region(spc)}); "
                    f"return address 0x{slr & ~1:08X} ({_region(slr)})"]
            if not (sxpsr >> 24) & 1:
                diag.append("stacked xPSR T-bit is clear: a BX/BLX/POP targeted an EVEN address "
                            "(ARM state is illegal on Cortex-M0+) -> bad function pointer, NULL call or corrupted return address")
            if not (_region(spc).startswith(("flash", "SRAM"))):
                diag.append(f"0x{spc:08X} is not code the program could legitimately run")
            info["diagnosis"] = diag
            info["stacked_pc"], info["stacked_lr"] = spc, slr
            if not self.elf:
                text = (f"{why.upper()} on core {core}: CPU branched to 0x{spc:08X} ({_region(spc)}), "
                        f"return address 0x{slr & ~1:08X}")
        if why in ("mark", "trigger"):
            text = f"snapshot ({why}) taken while core {core} ran {info.get('pc_src', '')}"
        self.emit(t, "fault", text=text, **info)
        self.cur_snapshot = dict(t_us=t, reason=why, regs=rd, chunks=[], stacked=info["stacked"])

    def handle_snapshot(self, t, p):
        addr, length = struct.unpack_from("<IH", p)
        chunk = p[6:6 + length]
        if self.cur_snapshot is None:
            self.cur_snapshot = dict(t_us=t, reason="reset" if True else "", regs={}, chunks=[], stacked={})
        self.cur_snapshot["chunks"].append((addr, chunk))

    def finish_snapshot(self):
        s = self.cur_snapshot
        self.cur_snapshot = None
        if not s or not s["chunks"]:
            return
        n = len(self.snapshots) + 1
        base = os.path.join(self.out_dir, f"snapshot_{n:02d}")
        regions = []
        # coalesce chunks into contiguous region files
        s["chunks"].sort(key=lambda c: c[0])
        cur_addr, cur = None, bytearray()
        for addr, chunk in s["chunks"]:
            if cur_addr is not None and addr == cur_addr + len(cur):
                cur += chunk
            else:
                if cur_addr is not None:
                    regions.append((cur_addr, bytes(cur)))
                cur_addr, cur = addr, bytearray(chunk)
        if cur_addr is not None:
            regions.append((cur_addr, bytes(cur)))
        files = []
        for addr, blob in regions:
            fn = f"{base}_{addr:08X}.bin"
            with open(fn, "wb") as f:
                f.write(blob)
            files.append(dict(addr=addr, len=len(blob), file=os.path.basename(fn)))
        # CrashDebug/CrashCatcher style hex dump (so `gdb` + CrashDebug can open it)
        with open(base + ".dump", "w") as f:
            f.write("63430300\n00000000\n")
            r = s["regs"]
            for k in [f"r{i}" for i in range(13)] + ["sp", "lr", "pc", "xpsr"]:
                f.write(f"{r.get(k, 0):08X}\n")
            f.write(f"{r.get('sp', 0):08X}\n{r.get('sp', 0):08X}\n{r.get('xpsr', 0):08X}\n")
            for addr, blob in regions:
                f.write(f"{addr:08X}\n{addr + len(blob):08X}\n")
                for i in range(0, len(blob), 16):
                    f.write(blob[i:i + 16].hex().upper() + "\n")
        self.snapshots.append(dict(index=n, t_us=s["t_us"], reason=s["reason"], regs=s["regs"], regions=files))
        self.emit(s["t_us"], "snapshot", index=n, regions=files, text=f"snapshot #{n}: " + ", ".join(f"0x{r['addr']:08X}+{r['len']}" for r in files))

    def handle_reset_cause(self, t, p):
        chip, wd = struct.unpack_from("<II", p)
        causes = decode_reset_cause(self.target, chip, wd)
        self.emit(t, "reset_cause", chip_reset=chip, watchdog_reason=wd, causes=causes,
                  text="last reset cause: " + ", ".join(causes))

    def handle_target_time(self, t, p):
        tt = struct.unpack_from("<I", p)[0]
        self.ttime_pairs.append((t, tt))
        if len(self.ttime_pairs) >= 2:
            (t0, tt0), (t1, tt1) = self.ttime_pairs[-2], self.ttime_pairs[-1]
            dp = t1 - t0
            dt = (tt1 - tt0) & 0xFFFFFFFF
            if dp > 0 and (tt1 < tt0 and dt > 0x80000000):
                self.emit(t, "target_clock", text=f"target clock went backwards (reset?) {tt0} -> {tt1}")
            elif dp > 0 and abs(dt - dp) > max(2000, dp // 20):
                self.emit(t, "target_clock", probe_dt=dp, target_dt=dt,
                          text=f"target clock drift: probe {dp} us, target {dt} us ({'frozen' if dt < dp else 'ahead'} by {abs(dp-dt)} us)")

    def handle_linkstat(self, t, p):
        wps, waits, faults, frames, drops = struct.unpack_from("<5I", p)
        self.last_link = dict(words_per_s=wps, wait_acks=waits, fault_acks=faults, frames=frames, drops=drops)
        self.emit(t, "link", **self.last_link, text=f"SWD {wps} transfers/s, {frames} frames, WAIT={waits} FAULT={faults} dropped={drops}")

    def handle_session(self, t, p):
        fw, sid, mh = struct.unpack_from("<III", p)
        level = p[12]
        self.emit(t, "session", fw=f"{fw >> 8}.{fw & 0xFF}", session_id=sid, manifest_hash=mh, level=level,
                  text=f"probe fw {fw >> 8}.{fw & 0xFF}, session 0x{sid:08X}, manifest 0x{mh:08X}, intrusiveness level L{level}" +
                       (" (passive reads only)" if level == 0 else " (passive reads + fault catch)"))
        if self.sidecar and mh != self.sidecar.get("manifest_hash"):
            self.warnings.append(f"manifest hash in capture 0x{mh:08X} differs from sidecar 0x{self.sidecar.get('manifest_hash', 0):08X}; names may be wrong")

    def handle_core1(self, t, p):
        d = struct.unpack_from("<I", p)[0]
        # Core 1 idling in WFI toggles SLEEP many times a second: that is normal, not news.
        # Only halts, lockups and resets of core 1 are reported.
        state = d & ((1 << 17) | (1 << 19) | (1 << 25))
        if state == self.last_core1_state or (state == 0 and self.last_core1_state is None):
            self.last_core1_state = state
            return
        self.last_core1_state = state
        bits = [n for b, n in {17: "HALTED", 19: "LOCKUP", 25: "RESET"}.items() if d >> b & 1]
        self.emit(t, "core1", dhcsr=d, text=f"core 1 state: {' '.join(bits) or 'running normally'} (DHCSR=0x{d:08X})")

    def handle_mark(self, t, p):
        mid = struct.unpack_from("<I", p)[0]
        src = "button" if mid & 0xB0000000 == 0xB0000000 else "host"
        self.emit(t, "mark", id=mid, text=f"MARK #{mid & 0xFFFF} from {src}")

    def handle_trigger(self, t, p):
        ti = p[0]
        eid, pos = F.varint_decode(p, 1)
        e = self.entries[eid] if eid < len(self.entries) else None
        v = e.raw_to_value(p[pos:pos + e.size]) if e else None
        spec = self.sidecar["triggers"][ti]["spec"] if self.sidecar and ti < len(self.sidecar.get("triggers", [])) else f"#{ti}"
        self.emit(t, "trigger", trigger=spec, name=e.name if e else None, value=v,
                  text=f"TRIGGER '{spec}' fired: {e.name if e else '?'} = {e.describe(v) if e else v}")

    HANDLERS = {
        F.REC_KEYFRAME: "handle_keyframe", F.REC_CHANGE: "handle_change", F.REC_PCSAMPLE: "handle_pc",
        F.REC_TARGET_TIME: "handle_target_time", F.REC_STATUS: "handle_status", F.REC_FAULT: "handle_fault",
        F.REC_SNAPSHOT: "handle_snapshot", F.REC_RESET_CAUSE: "handle_reset_cause", F.REC_MARK: "handle_mark",
        F.REC_LINKSTAT: "handle_linkstat", F.REC_SESSION: "handle_session", F.REC_CORE1: "handle_core1",
        F.REC_TRIGGER: "handle_trigger",
    }

    # record payload sizes (None = variable, computed inline)
    def record_len(self, tag, p, pos):
        if tag == F.REC_KEYFRAME:
            return sum(e.size for e in self.entries)
        if tag == F.REC_CHANGE:
            eid, q = F.varint_decode(p, pos)
            return (q - pos) + (self.entries[eid].size if eid < len(self.entries) else 4)
        if tag in (F.REC_PCSAMPLE, F.REC_TARGET_TIME, F.REC_MARK, F.REC_CORE1):
            return 4
        if tag == F.REC_STATUS:
            return 5
        if tag == F.REC_FAULT:
            return 2 + 12 + 68 + 32
        if tag == F.REC_SNAPSHOT:
            return 6 + struct.unpack_from("<H", p, pos + 4)[0]
        if tag == F.REC_RESET_CAUSE:
            return 8
        if tag == F.REC_LINKSTAT:
            return 20
        if tag == F.REC_SESSION:
            return 13
        if tag == F.REC_TRIGGER:
            eid, q = F.varint_decode(p, pos + 1)
            return (q - pos) + (self.entries[eid].size if eid < len(self.entries) else 4)
        return None

    # ---- main ------------------------------------------------------------------------
    def _new_boot(self, hdr, prev_end):
        """The probe's clock restarted: start a fresh timeline with fresh per-boot state."""
        if self.cur_snapshot:
            self.finish_snapshot()
        self.boot += 1
        self.boot_base += prev_end + 1_000_000
        self.values.clear()
        self.cur_func = None
        self.ttime_pairs.clear()
        self.last_core1_state = None
        self.emit(hdr["ts_us"], "boot", text="--- probe restarted: new recording boot (probe clock back to 0) ---")

    def run(self):
        last_seq = None
        prev_ts = None
        for hdr, payload in iter_blocks(self.data):
            self.stats["blocks"] += 1
            if prev_ts is not None and hdr["ts_us"] + 1_000_000 < prev_ts:
                self._new_boot(hdr, prev_ts)
            prev_ts = hdr["ts_us"]
            while len(self.boots) <= self.boot:
                self.boots.append(dict(first=hdr["ts_us"], last=hdr["ts_us"], blocks=0, session=hdr["session_id"],
                                       manifest_hash=hdr["manifest_hash"]))
            b = self.boots[self.boot]
            b["blocks"] += 1
            b["last"] = max(b["last"], hdr["ts_us"])
            s = self.sessions[hdr["session_id"]]
            s["blocks"] += 1
            s["manifest_hash"] = hdr["manifest_hash"]
            s["first"] = hdr["ts_us"] if s["first"] is None else min(s["first"], hdr["ts_us"])
            s["last"] = max(s["last"] or 0, hdr["ts_us"])
            if last_seq is not None and hdr["seq"] != last_seq + 1:
                self.emit(hdr["ts_us"], "gap", text=f"missing blocks between seq {last_seq} and {hdr['seq']}")
                self.stats["gaps"] += 1
            last_seq = hdr["seq"]
            if hdr["flags"] & F.BLKF_AFTER_GAP:
                self.emit(hdr["ts_us"], "gap", text="records were dropped before this block (recorder overrun)")
                self.stats["gaps"] += 1
            t = hdr["ts_us"]
            pos = 0
            while pos < len(payload):
                tag = payload[pos]
                pos += 1
                dt, pos = F.varint_decode(payload, pos)
                t += dt
                n = self.record_len(tag, payload, pos)
                if n is None:
                    self.warnings.append(f"unknown record tag 0x{tag:02X} in block {hdr['seq']}; rest of block skipped")
                    break
                rec = payload[pos:pos + n]
                pos += n
                self.stats["records"] += 1
                getattr(self, self.HANDLERS[tag])(t, rec)
        if self.cur_snapshot:
            self.finish_snapshot()
        self.events.sort(key=lambda e: (e["boot"], e["t_us"]))
        return self

    # ---- outputs ------------------------------------------------------------------------
    def write_events(self):
        path = os.path.join(self.out_dir, "events.jsonl")
        with open(path, "w") as f:
            for e in self.events:
                f.write(json.dumps(e, default=str) + "\n")
        return path

    def write_trail(self):
        path = os.path.join(self.out_dir, "trail.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write("UNIVERSAL BLACK BOX - event trail\n")
            f.write("=" * 70 + "\n")
            f.write(f"blocks: {self.stats['blocks']}  records: {self.stats['records']}  changes: {self.stats['changes']}  "
                    f"faults: {self.stats['faults']}  gaps: {self.stats['gaps']}\n")
            for i, b in enumerate(self.boots):
                span = (b["last"] - b["first"]) / 1e6
                f.write(f"boot {i + 1}: {b['blocks']} blocks, {span:.1f} s recorded (probe clock), "
                        f"session 0x{b['session']:08X}, manifest 0x{b['manifest_hash']:08X}\n")
            if len(self.boots) > 1:
                f.write("(the probe was powered up more than once; each boot has its own clock starting near 0)\n")
            if self.warnings:
                f.write("warnings:\n" + "".join(f"  - {w}\n" for w in self.warnings))
            f.write("\n   time (s)      event\n" + "-" * 70 + "\n")
            quiet = {"pc", "link", "initial"}
            for e in self.events:
                tag = e["kind"]
                if tag in quiet and tag != "initial":
                    continue
                mark = {"fault": "!!", "trigger": "!", "mark": "*", "status": "-", "gap": "~", "snapshot": "#", "reset_cause": "-", "initial": "=", "boot": "="}.get(tag, " ")
                name = f" {e['name']}:" if "name" in e and tag in ("change", "reg", "initial") else ""
                f.write(f"{_fmt_time(e['t_us'])} {mark:>2}{name} {e.get('text', '')}\n")
                if tag == "fault":
                    r = e["regs"]
                    f.write(f"                   regs: pc={r['pc']:08X} lr={r['lr']:08X} sp={r['sp']:08X} xpsr={r['xpsr']:08X} "
                            f"r0={r['r0']:08X} r1={r['r1']:08X} r2={r['r2']:08X} r3={r['r3']:08X}\n")
                    if e.get("stacked"):
                        s = e["stacked"]
                        f.write(f"                   stacked frame: pc={s['pc']:08X} lr={s['lr']:08X} xpsr={s['xpsr']:08X} "
                                f"r0={s['r0']:08X} r1={s['r1']:08X} r2={s['r2']:08X} r3={s['r3']:08X} r12={s['r12']:08X}\n")
                    for line in e.get("diagnosis", []):
                        f.write(f"                   => {line}\n")
                    if e.get("fault_site"):
                        f.write(f"                   faulting instruction: {e['fault_site']}\n")
                        f.write(f"                   return address:       {e['fault_return']}\n")
                    if e.get("dfsr_bits"):
                        f.write(f"                   DFSR: {' '.join(e['dfsr_bits'])}\n")
            if self.pc_samples:
                f.write("\nexecution profile (DWT PC samples)\n" + "-" * 70 + "\n")
                counts = defaultdict(int)
                for _, pc, fn in self.pc_samples:
                    counts[fn or f"0x{pc:08X}"] += 1
                total = len(self.pc_samples)
                for fn, c in sorted(counts.items(), key=lambda kv: -kv[1])[:20]:
                    f.write(f"  {100*c/total:5.1f}%  {fn}\n")
            f.write("\nlast known values\n" + "-" * 70 + "\n")
            for e in self.entries:
                if e.id in self.values:
                    f.write(f"  {e.name:40s} = {e.describe(self.values[e.id])}\n")
        return path

    def write_vcd(self):
        path = os.path.join(self.out_dir, "timeline.vcd")
        ids = {}
        code = 33

        def next_id():
            nonlocal code
            s = ""
            c = code
            while True:
                s = chr(33 + c % 94) + s
                c //= 94
                if c == 0:
                    break
            code += 1
            return s
        with open(path, "w") as f:
            f.write("$timescale 1us $end\n$scope module target $end\n")
            for e in self.entries:
                vid = next_id()
                ids[e.id] = vid
                nm = e.name.replace(" ", "_").replace(":", "_")
                if e.kind == "float":
                    f.write(f"$var real 64 {vid} {nm} $end\n")
                else:
                    f.write(f"$var wire {e.size*8} {vid} {nm} $end\n")
            ev_id = next_id()
            f.write(f"$var event 1 {ev_id} fault $end\n")
            pc_id = next_id()
            f.write(f"$var wire 32 {pc_id} pc $end\n")
            f.write("$upscope $end\n$enddefinitions $end\n")
            t0 = self.events[0]["t_abs"] if self.events else 0
            cur_t = None
            by_name = {e.name: e for e in self.entries}
            for ev in self.events:
                k = ev["kind"]
                if k not in ("change", "reg", "initial", "fault", "pc"):
                    continue
                t = ev["t_abs"] - t0
                if t != cur_t:
                    f.write(f"#{t}\n")
                    cur_t = t
                if k == "fault":
                    f.write(f"1{ev_id}\n")
                elif k == "pc":
                    f.write(f"b{ev['pc']:032b} {pc_id}\n")
                else:
                    e = by_name.get(ev["name"])
                    if e is None:
                        continue
                    v = ev["value"]
                    if e.kind == "float":
                        f.write(f"r{v} {ids[e.id]}\n")
                    elif isinstance(v, int):
                        f.write(f"b{v & ((1 << (e.size*8)) - 1):b} {ids[e.id]}\n")
        return path

    def write_trace(self):
        """Chrome/Perfetto trace event JSON: counters for numeric values, instants for events, slices for functions."""
        path = os.path.join(self.out_dir, "trace.json")
        tr = []
        t0 = self.events[0]["t_abs"] if self.events else 0
        open_fn = None
        for ev in self.events:
            ts = ev["t_abs"] - t0
            k = ev["kind"]
            if k in ("change", "initial", "reg"):
                v = ev["value"]
                if isinstance(v, (int, float)):
                    tr.append(dict(name=ev["name"], ph="C", ts=ts, pid=1, tid=1, args={"value": v}))
            elif k == "pc":
                if open_fn:
                    tr.append(dict(name=open_fn[0], ph="X", ts=open_fn[1], dur=max(ts - open_fn[1], 1), pid=1, tid=2))
                open_fn = (ev.get("function") or f"0x{ev['pc']:08X}", ts)
            elif k in ("fault", "trigger", "mark", "status", "gap", "reset_cause", "snapshot", "core1", "target_clock"):
                tr.append(dict(name=ev.get("text", k)[:80], cat=k, ph="i", ts=ts, pid=1, tid=3, s="g",
                               args={kk: vv for kk, vv in ev.items() if kk not in ("t_us", "t", "kind") and isinstance(vv, (int, str, float))}))
        if open_fn:
            tr.append(dict(name=open_fn[0], ph="X", ts=open_fn[1], dur=1000, pid=1, tid=2))
        meta = [dict(name="process_name", ph="M", pid=1, args={"name": "target"}),
                dict(name="thread_name", ph="M", pid=1, tid=1, args={"name": "variables"}),
                dict(name="thread_name", ph="M", pid=1, tid=2, args={"name": "execution (PC samples)"}),
                dict(name="thread_name", ph="M", pid=1, tid=3, args={"name": "events"})]
        with open(path, "w") as f:
            json.dump(dict(traceEvents=meta + tr, displayTimeUnit="ms"), f)
        return path

    def write_all(self):
        return [self.write_events(), self.write_trail(), self.write_vcd(), self.write_trace()]
