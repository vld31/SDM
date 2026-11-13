"""CMSIS-SVD reading with the side-effect denylist that keeps the recorder passive."""
import fnmatch
import os
import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Dict, List, Optional

# Registers that change state when read.  A match here refuses the register
# even if it is on the allowlist.  Names are PERIPHERAL.REGISTER globs.
DENYLIST = [
    "SIO.FIFO_RD", "SIO.SPINLOCK*", "SIO.FIFO_ST",
    "TIMER*.TIMELR", "TIMER*.TIMEHR",
    "UART*.UARTDR", "UART*.UARTRSR",
    "ADC.FIFO",
    "PIO*.RXF*", "PIO*.TXF*",
    "SPI*.SSPDR",
    "I2C*.IC_CLR_*", "I2C*.IC_DATA_CMD",
    "USBCTRL*", "XIP*", "QMI*", "SSI*", "TRNG*", "OTP*", "HSTX*",
    "ROSC.RANDOMBIT", "RTC.*",
]

DEFAULT_ALLOW = {
    "rp2040": [
        "SIO.GPIO_IN", "SIO.GPIO_OE", "SIO.GPIO_OUT",
        "PWM.CH*_CSR", "PWM.CH*_CC", "PWM.CH*_TOP",
        "ADC.CS", "ADC.RESULT",
        "UART0.UARTFR", "UART0.UARTCR", "UART0.UARTLCR_H", "UART0.UARTIBRD", "UART0.UARTFBRD",
        "UART1.UARTFR", "UART1.UARTCR",
        "WATCHDOG.CTRL", "WATCHDOG.REASON",
        "VREG_AND_CHIP_RESET.CHIP_RESET",
        "RESETS.RESET_DONE",
    ],
    "rp2350": [
        "SIO.GPIO_IN", "SIO.GPIO_OE", "SIO.GPIO_OUT",
        "PWM.CH*_CSR", "PWM.CH*_CC", "PWM.CH*_TOP",
        "ADC.CS", "ADC.RESULT",
        "UART0.UARTFR", "UART0.UARTCR", "UART1.UARTFR", "UART1.UARTCR",
        "WATCHDOG.CTRL", "WATCHDOG.REASON",
        "POWMAN.CHIP_RESET",
        "RESETS.RESET_DONE",
    ],
}


def default_svd_path(target: str) -> Optional[str]:
    home = os.path.expanduser("~")
    sub = "rp2040" if target == "rp2040" else "rp2350"
    name = "RP2040.svd" if target == "rp2040" else "RP2350.svd"
    cands = []
    base = os.path.join(home, ".pico-sdk", "sdk")
    if os.path.isdir(base):
        for v in sorted(os.listdir(base), reverse=True):
            cands.append(os.path.join(base, v, "src", sub, "hardware_regs", name))
    sdk = os.environ.get("PICO_SDK_PATH")
    if sdk:
        cands.insert(0, os.path.join(sdk, "src", sub, "hardware_regs", name))
    cands.append(os.path.join(os.path.dirname(__file__), "..", "svd", name))
    for c in cands:
        if os.path.isfile(c):
            return c
    return None


@dataclass
class Field:
    name: str
    bit_offset: int
    bit_width: int
    enum: Dict[int, str] = field(default_factory=dict)


@dataclass
class Register:
    periph: str
    name: str
    addr: int
    size: int = 4
    access: str = "read-write"
    read_action: Optional[str] = None
    fields: List[Field] = field(default_factory=list)

    @property
    def full(self):
        return f"{self.periph}.{self.name}"


def _text(el, tag, default=None):
    x = el.find(tag)
    return x.text.strip() if x is not None and x.text else default


def _int(s, default=0):
    if s is None:
        return default
    s = s.strip()
    try:
        if s.lower().startswith("0x"):
            return int(s, 16)
        if s.startswith("#"):
            return int(s[1:].replace("x", "0"), 2)
        return int(s, 0)
    except ValueError:
        return default


def _bit_range(f):
    br = _text(f, "bitRange")
    if br:
        m = re.match(r"\[(\d+):(\d+)\]", br)
        if m:
            hi, lo = int(m.group(1)), int(m.group(2))
            return lo, hi - lo + 1
    lo = _text(f, "bitOffset")
    w = _text(f, "bitWidth")
    if lo is not None:
        return _int(lo), _int(w, 1)
    lsb, msb = _text(f, "lsb"), _text(f, "msb")
    if lsb is not None:
        return _int(lsb), _int(msb) - _int(lsb) + 1
    return 0, 32


class Svd:
    def __init__(self, path: str):
        self.path = path
        self.registers: Dict[str, Register] = {}
        root = ET.parse(path).getroot()
        self.device = _text(root, "name", "device")
        periphs = {}
        for p in root.iter("peripheral"):
            name = _text(p, "name")
            base = _int(_text(p, "baseAddress"))
            derived = p.get("derivedFrom")
            regs_el = p.find("registers")
            if regs_el is None and derived and derived in periphs:
                regs_el = periphs[derived]
            periphs[name] = regs_el
            if regs_el is None:
                continue
            for r in regs_el.iter("register"):
                rname = _text(r, "name")
                dim = _text(r, "dim")
                names = [rname]
                offs = [_int(_text(r, "addressOffset"))]
                if dim:
                    n = _int(dim)
                    inc = _int(_text(r, "dimIncrement"))
                    idx = _text(r, "dimIndex")
                    labels = idx.split(",") if idx else [str(i) for i in range(n)]
                    names = [rname.replace("%s", labels[i].strip()) for i in range(n)]
                    offs = [offs[0] + i * inc for i in range(n)]
                fields = []
                fe = r.find("fields")
                if fe is not None:
                    for f in fe.iter("field"):
                        lo, w = _bit_range(f)
                        enum = {}
                        ev = f.find("enumeratedValues")
                        if ev is not None:
                            usage = _text(ev, "usage", "read-write")
                            if usage in ("read", "read-write"):
                                for e in ev.iter("enumeratedValue"):
                                    v = _text(e, "value")
                                    if v is not None:
                                        enum[_int(v)] = _text(e, "name")
                        fields.append(Field(_text(f, "name"), lo, w, enum))
                for nm, off in zip(names, offs):
                    reg = Register(name, nm, base + off, _int(_text(r, "size"), 32) // 8,
                                   _text(r, "access", "read-write"), _text(r, "readAction"), fields)
                    self.registers[reg.full] = reg

    def match(self, patterns) -> List[Register]:
        out = []
        for full, reg in self.registers.items():
            if any(fnmatch.fnmatchcase(full, p) for p in patterns):
                out.append(reg)
        return out

    @staticmethod
    def is_denied(reg: Register) -> Optional[str]:
        if reg.read_action:
            return f"SVD readAction={reg.read_action}"
        if reg.access == "write-only":
            return "write-only"
        for d in DENYLIST:
            if fnmatch.fnmatchcase(reg.full, d):
                return f"denylist {d}"
        return None

    def by_addr(self, addr: int) -> Optional[Register]:
        for r in self.registers.values():
            if r.addr == addr:
                return r
        return None
