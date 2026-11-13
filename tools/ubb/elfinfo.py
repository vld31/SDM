"""ELF/DWARF reading: fixed-address globals, types, sections, address -> file:line."""
import bisect
import fnmatch
import os
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection
from elftools.dwarf.descriptions import describe_form_class

RAM_RANGES = {
    "rp2040": (0x20000000, 0x20000000 + 264 * 1024),
    "rp2350": (0x20000000, 0x20000000 + 520 * 1024),
}


@dataclass
class Leaf:
    name: str
    addr: int
    size: int
    kind: str                       # int, uint, float, bool, char, enum, pointer, raw
    enum: Dict[int, str] = field(default_factory=dict)
    bit_offset: Optional[int] = None  # for bitfields, within the stored unit (LSB first)
    bit_size: Optional[int] = None
    ctype: str = ""


class ElfInfo:
    def __init__(self, path: str):
        self.path = path
        self.f = open(path, "rb")
        self.elf = ELFFile(self.f)
        self.dwarf = self.elf.get_dwarf_info() if self.elf.has_dwarf_info() else None
        self._symbols = None
        self._funcs = None
        self._lines = None
        self.cpu = self._detect_cpu()

    # ---- target detection -------------------------------------------------
    def _detect_cpu(self) -> str:
        sec = self.elf.get_section_by_name(".ARM.attributes")
        if sec is None:
            return "unknown"
        data = sec.data()
        if b"6S-M" in data or b"Cortex-M0" in data:
            return "cortex-m0plus"
        if b"8-M.MAIN" in data or b"Cortex-M33" in data:
            return "cortex-m33"
        return "unknown"

    def guess_target(self) -> str:
        return "rp2350" if self.cpu == "cortex-m33" else "rp2040"

    # ---- symbols ------------------------------------------------------------
    def symbols(self):
        if self._symbols is None:
            self._symbols = {}
            self._funcs = []
            for sec in self.elf.iter_sections():
                if not isinstance(sec, SymbolTableSection):
                    continue
                for sym in sec.iter_symbols():
                    t = sym["st_info"]["type"]
                    if t == "STT_FUNC" and sym["st_size"] > 0:
                        self._funcs.append((sym["st_value"] & ~1, sym["st_size"], sym.name))
                    if t in ("STT_OBJECT", "STT_NOTYPE", "STT_FUNC") and sym.name:
                        self._symbols[sym.name] = sym["st_value"]
            self._funcs.sort()
        return self._symbols

    def symbol(self, name: str) -> Optional[int]:
        return self.symbols().get(name)

    def function_at(self, addr: int) -> Optional[str]:
        self.symbols()
        addr &= ~1
        i = bisect.bisect_right(self._funcs, (addr, 1 << 32, "")) - 1
        if i >= 0:
            start, size, name = self._funcs[i]
            if start <= addr < start + size:
                return name
        return None

    # ---- sections / regions ----------------------------------------------------
    def data_sections(self, target="rp2040"):
        """RAM-resident loaded sections worth snapshotting (.data, .bss, vector table...)."""
        lo, hi = RAM_RANGES.get(target, RAM_RANGES["rp2040"])
        out = []
        for sec in self.elf.iter_sections():
            flags = sec["sh_flags"]
            if not (flags & 0x2) or flags & 0x400:          # SHF_ALLOC, not SHF_TLS
                continue
            if sec["sh_size"] == 0 or not (lo <= sec["sh_addr"] < hi):
                continue
            if sec.name.startswith((".heap", ".stack_dummy", ".stack")):
                continue
            out.append((sec.name, sec["sh_addr"], sec["sh_size"]))
        return out

    def stack_region(self, target: str):
        lo, hi = RAM_RANGES.get(target, RAM_RANGES["rp2040"])
        top = self.symbol("__StackTop") or self.symbol("__stack") or self.symbol("_estack")
        bottom = self.symbol("__StackLimit") or self.symbol("__StackBottom") or self.symbol("_sstack")
        if top and bottom and bottom < top <= hi:
            return bottom, top - bottom
        if top and lo < top <= hi:
            return top - 0x1000, 0x1000
        return hi - 0x1000, 0x1000

    # ---- addr -> file:line -----------------------------------------------------
    def _build_lines(self):
        self._lines = []
        if not self.dwarf:
            return
        for cu in self.dwarf.iter_CUs():
            try:
                lp = self.dwarf.line_program_for_CU(cu)
            except Exception:
                continue
            if lp is None:
                continue
            files = lp["file_entry"]
            dirs = lp["include_directory"]
            prev = None
            for entry in lp.get_entries():
                st = entry.state
                if st is None:
                    continue
                if prev is not None and not prev.end_sequence:
                    fe = files[prev.file - 1] if 0 < prev.file <= len(files) else None
                    if fe is not None:
                        d = fe.dir_index
                        dname = dirs[d - 1].decode(errors="replace") if 0 < d <= len(dirs) else ""
                        fname = os.path.join(dname, fe.name.decode(errors="replace"))
                        self._lines.append((prev.address, st.address, fname, prev.line))
                prev = st
        self._lines.sort()

    def addr2line(self, addr: int) -> Optional[str]:
        if self._lines is None:
            self._build_lines()
        addr &= ~1
        i = bisect.bisect_right(self._lines, (addr, 1 << 40, "", 0)) - 1
        if i >= 0:
            a, b, f, l = self._lines[i]
            if a <= addr < b:
                return f"{f}:{l}"
        return None

    def describe_addr(self, addr: int) -> str:
        fn = self.function_at(addr)
        fl = self.addr2line(addr)
        parts = [f"0x{addr:08X}"]
        if fn:
            parts.append(fn + "()")
        if fl:
            parts.append(fl)
        return " ".join(parts)

    # ---- globals from DWARF ------------------------------------------------------
    def _attr(self, die, name):
        a = die.attributes.get(name)
        return a.value if a is not None else None

    def _die_name(self, die):
        n = self._attr(die, "DW_AT_name")
        if n is None and "DW_AT_specification" in die.attributes:
            try:
                return self._die_name(die.get_DIE_from_attribute("DW_AT_specification"))
            except Exception:
                return None
        if n is None and "DW_AT_abstract_origin" in die.attributes:
            try:
                return self._die_name(die.get_DIE_from_attribute("DW_AT_abstract_origin"))
            except Exception:
                return None
        return n.decode(errors="replace") if isinstance(n, bytes) else n

    def _type_die(self, die):
        if "DW_AT_type" in die.attributes:
            return die.get_DIE_from_attribute("DW_AT_type")
        if "DW_AT_specification" in die.attributes:
            return self._type_die(die.get_DIE_from_attribute("DW_AT_specification"))
        return None

    def _strip(self, tdie):
        """Follow typedef/const/volatile; return (die, typedef-name)."""
        name = None
        seen = 0
        while tdie is not None and tdie.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type") and seen < 16:
            if tdie.tag == "DW_TAG_typedef" and name is None:
                name = self._die_name(tdie)
            tdie = self._type_die(tdie)
            seen += 1
        return tdie, name

    def _location_addr(self, die):
        loc = die.attributes.get("DW_AT_location")
        if loc is None:
            return None
        if describe_form_class(loc.form) != "exprloc" and loc.form != "DW_FORM_block1":
            return None
        expr = loc.value
        if len(expr) == 5 and expr[0] == 0x03:  # DW_OP_addr
            return int.from_bytes(bytes(expr[1:5]), "little")
        return None

    def _expand(self, name, addr, tdie, out: List[Leaf], depth, limits):
        if depth > limits["depth"] or len(out) >= limits["leaves"]:
            return
        tdie, tname = self._strip(tdie)
        if tdie is None:
            return
        tag = tdie.tag
        size = self._attr(tdie, "DW_AT_byte_size")
        if tag == "DW_TAG_base_type":
            enc = self._attr(tdie, "DW_AT_encoding")
            kind = {0x02: "bool", 0x04: "float", 0x05: "int", 0x06: "char", 0x07: "uint", 0x08: "char"}.get(enc, "raw")
            if size in (1, 2, 4, 8):
                out.append(Leaf(name, addr, size, kind, ctype=tname or self._die_name(tdie) or ""))
        elif tag == "DW_TAG_enumeration_type":
            enum = {}
            for ch in tdie.iter_children():
                if ch.tag == "DW_TAG_enumerator":
                    v = self._attr(ch, "DW_AT_const_value")
                    enum[v] = self._die_name(ch)
            size = size or 4
            if size in (1, 2, 4, 8):
                out.append(Leaf(name, addr, size, "enum", enum=enum, ctype=tname or self._die_name(tdie) or "enum"))
        elif tag == "DW_TAG_pointer_type":
            out.append(Leaf(name, addr, size or 4, "pointer", ctype=tname or "ptr"))
        elif tag in ("DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_class_type"):
            for ch in tdie.iter_children():
                if ch.tag != "DW_TAG_member":
                    continue
                mname = self._die_name(ch) or "?"
                off = self._attr(ch, "DW_AT_data_member_location") or 0
                if isinstance(off, (bytes, list)):
                    off = 0
                bit_size = self._attr(ch, "DW_AT_bit_size")
                if bit_size is not None:
                    msize = self._attr(ch, "DW_AT_byte_size")
                    mt, _ = self._strip(self._type_die(ch))
                    if msize is None and mt is not None:
                        msize = self._attr(mt, "DW_AT_byte_size")
                    msize = msize or 4
                    dbo = self._attr(ch, "DW_AT_data_bit_offset")
                    if dbo is not None:
                        unit_off = (dbo // 8) & ~(msize - 1)
                        bit_off = dbo - unit_off * 8
                        out.append(Leaf(f"{name}.{mname}", addr + unit_off, msize, "uint",
                                        bit_offset=bit_off, bit_size=bit_size, ctype="bitfield"))
                    else:
                        bo = self._attr(ch, "DW_AT_bit_offset")
                        if bo is not None:  # DWARF2/3 big-endian-style numbering
                            bit_off = msize * 8 - bo - bit_size
                            out.append(Leaf(f"{name}.{mname}", addr + off, msize, "uint",
                                            bit_offset=bit_off, bit_size=bit_size, ctype="bitfield"))
                    continue
                self._expand(f"{name}.{mname}", addr + off, self._type_die(ch), out, depth + 1, limits)
        elif tag == "DW_TAG_array_type":
            et = self._type_die(tdie)
            est, _ = self._strip(et)
            esize = self._attr(est, "DW_AT_byte_size") if est is not None else None
            if est is not None and est.tag == "DW_TAG_pointer_type":
                esize = esize or 4
            count = None
            for ch in tdie.iter_children():
                if ch.tag == "DW_TAG_subrange_type":
                    ub = self._attr(ch, "DW_AT_upper_bound")
                    cnt = self._attr(ch, "DW_AT_count")
                    if cnt is not None:
                        count = cnt
                    elif ub is not None:
                        count = ub + 1
                    break
            if esize is None or count is None:
                return
            if est.tag == "DW_TAG_base_type" and self._attr(est, "DW_AT_encoding") in (0x06, 0x08) and esize == 1:
                # char array: record as raw string chunk(s) of up to 8 bytes each
                for i in range(0, min(count, limits["array"] * 8), 8):
                    n = min(8, count - i)
                    n = 8 if n > 4 else (4 if n > 2 else (2 if n > 1 else 1))
                    out.append(Leaf(f"{name}[{i}]", addr + i, n, "char", ctype="char[]"))
                return
            for i in range(min(count, limits["array"])):
                self._expand(f"{name}[{i}]", addr + i * esize, et, out, depth + 1, limits)
        # other tags (functions, subroutine types) are ignored

    # compilation units that belong to a vendor framework rather than the application
    SDK_CU_PATTERNS = ("pico-sdk", "pico_sdk", "/lib/", "\\lib\\", "framework-", "packages/", "newlib", "libgcc",
                       "tinyusb", "cyw43", "lwip", "mbedtls", "btstack", "FreeRTOS", "freertos", "cmsis", "CMSIS")

    def _is_sdk_cu(self, cu) -> bool:
        """True when the compilation unit's source file lives inside a vendor framework."""
        top = cu.get_top_DIE()
        name = self._attr(top, "DW_AT_name") or b""
        name = name if isinstance(name, str) else name.decode(errors="replace")
        return any(p in name for p in self.SDK_CU_PATTERNS)

    def globals(self, patterns=("*",), exclude=(), target="rp2040", max_leaves=256, array_limit=16, depth=4,
                include_sdk=False) -> List[Leaf]:
        """Fixed-address variables (globals and function statics) that live in RAM.

        By default only the application's own compilation units are scanned; the
        pico-sdk / framework internals are skipped unless include_sdk is set.
        """
        if not self.dwarf:
            return self.globals_from_symtab(patterns, exclude, target, max_leaves)
        lo, hi = RAM_RANGES.get(target, RAM_RANGES["rp2040"])
        limits = {"depth": depth, "leaves": max_leaves, "array": array_limit}
        leaves: List[Leaf] = []
        seen_addr = set()
        for cu in self.dwarf.iter_CUs():
            if not include_sdk and self._is_sdk_cu(cu):
                continue
            top = cu.get_top_DIE()
            stack = [(top, None)]
            while stack:
                die, func = stack.pop()
                for ch in die.iter_children():
                    if ch.tag == "DW_TAG_subprogram":
                        stack.append((ch, self._die_name(ch)))
                        continue
                    if ch.tag in ("DW_TAG_lexical_block", "DW_TAG_namespace"):
                        stack.append((ch, func))
                        continue
                    if ch.tag != "DW_TAG_variable":
                        continue
                    addr = self._location_addr(ch)
                    if addr is None or not (lo <= addr < hi):
                        continue
                    name = self._die_name(ch)
                    if not name or name.startswith("__"):
                        continue
                    if func:
                        name = f"{func}::{name}"
                    if not any(fnmatch.fnmatchcase(name, p) for p in patterns):
                        continue
                    if any(fnmatch.fnmatchcase(name, p) for p in exclude):
                        continue
                    if addr in seen_addr:
                        continue
                    seen_addr.add(addr)
                    self._expand(name, addr, self._type_die(ch), leaves, 0, limits)
                    if len(leaves) >= max_leaves:
                        return leaves
        return leaves

    def globals_from_symtab(self, patterns, exclude, target, max_leaves) -> List[Leaf]:
        lo, hi = RAM_RANGES.get(target, RAM_RANGES["rp2040"])
        out = []
        for sec in self.elf.iter_sections():
            if not isinstance(sec, SymbolTableSection):
                continue
            for sym in sec.iter_symbols():
                if sym["st_info"]["type"] != "STT_OBJECT":
                    continue
                a, s = sym["st_value"], sym["st_size"]
                if not (lo <= a < hi) or s not in (1, 2, 4, 8):
                    continue
                if not any(fnmatch.fnmatchcase(sym.name, p) for p in patterns):
                    continue
                if any(fnmatch.fnmatchcase(sym.name, p) for p in exclude):
                    continue
                out.append(Leaf(sym.name, a, s, "raw", ctype="symtab"))
                if len(out) >= max_leaves:
                    return out
        return out

    def build_id(self) -> Optional[str]:
        sec = self.elf.get_section_by_name(".note.gnu.build-id")
        if sec is None:
            return None
        for n in sec.iter_notes():
            if n["n_type"] == "NT_GNU_BUILD_ID":
                return n["n_desc"]
        return None
