"""Build the capture manifest (binary for the probe + JSON sidecar for the decoder)."""
import json
import os
import re
from typing import List

from . import format as F
from .elfinfo import ElfInfo, Leaf
from .svdinfo import Svd, DEFAULT_ALLOW, default_svd_path

TRIGGER_RE = re.compile(r"^(?P<name>[A-Za-z_][\w:.\[\]]*)\s*(?P<op>==|!=|>|<)\s*(?P<val>-?(0x[0-9a-fA-F]+|\d+))(?P<snap>:snap)?$")


def parse_trigger(spec: str):
    """'var==5', 'var>100:snap', 'change:var', 'bitset:var:0x4:snap' -> (name, op, flags, value)."""
    flags = 0
    if spec.endswith(":snap"):
        flags |= F.TRIGF_SNAPSHOT
    s = spec[:-5] if flags & F.TRIGF_SNAPSHOT else spec
    if s.startswith("change:"):
        return s[7:], F.TRIG_OPS["change"], flags, 0
    if s.startswith("bitset:"):
        _, name, val = s.split(":", 2)
        return name, F.TRIG_OPS["bitset"], flags, int(val, 0)
    m = TRIGGER_RE.match(spec)
    if not m:
        raise ValueError(f"bad trigger: {spec}")
    return m.group("name"), F.TRIG_OPS[m.group("op")], flags, int(m.group("val"), 0)


def generate(elf_path: str, out_base: str, *, target=None, svd_path=None, var_patterns=("*",),
             exclude=(), periph_patterns=None, no_default_periph=False, pins=(), snapshot="sections",
             triggers=(), level=F.LEVEL_ARMED, on_fault="resume", pc_sampling=10, swd_hz=4_000_000,
             fast_us=1000, slow_us=10_000, keyframe_us=1_000_000, snapshot_on_fault=True,
             snapshot_on_reset=False, max_vars=200, include_sdk=False, verbose=print):
    elf = ElfInfo(elf_path)
    target = target or elf.guess_target()
    tgt_code = F.TGT_RP2350 if target == "rp2350" else F.TGT_RP2040
    verbose(f"ELF: {elf_path}  cpu={elf.cpu}  target={target}  dwarf={'yes' if elf.dwarf else 'NO (symtab fallback)'}")

    leaves: List[Leaf] = elf.globals(var_patterns, exclude, target, max_leaves=max_vars, include_sdk=include_sdk)
    leaves.sort(key=lambda l: l.addr)
    verbose(f"variables: {len(leaves)} sampled leaves")

    # peripherals
    regs = []
    svd = None
    svd_path = svd_path or default_svd_path(target)
    if svd_path and os.path.isfile(svd_path):
        svd = Svd(svd_path)
        pats = [] if no_default_periph else list(DEFAULT_ALLOW.get(target, []))
        pats += [f"IO_BANK0.GPIO{p}_CTRL" for p in pins]
        pats += list(periph_patterns or [])
        for r in svd.match(pats):
            why = Svd.is_denied(r)
            if why:
                verbose(f"  refused {r.full}: {why}")
                continue
            regs.append(r)
        regs.sort(key=lambda r: r.addr)
        verbose(f"peripheral registers: {len(regs)} from {os.path.basename(svd_path)}")
    else:
        verbose("no SVD found: peripheral registers not recorded (pass --svd)")

    # entries: fast class first (variables), then slow (peripherals); each sorted by address
    entries = []
    meta = []
    for l in leaves:
        entries.append((l.addr, l.size, F.CLASS_FAST))
        meta.append(dict(name=l.name, addr=l.addr, size=l.size, kind=l.kind, ctype=l.ctype, enum={str(k): v for k, v in l.enum.items()},
                         bit_offset=l.bit_offset, bit_size=l.bit_size, cls="fast"))
    for r in regs:
        entries.append((r.addr, 4, F.CLASS_SLOW))
        meta.append(dict(name=r.full, addr=r.addr, size=4, kind="reg", ctype="register", cls="slow",
                         fields=[dict(name=f.name, bit_offset=f.bit_offset, bit_width=f.bit_width,
                                      enum={str(k): v for k, v in f.enum.items()}) for f in r.fields]))
    if len(entries) > 256:
        raise SystemExit(f"too many entries ({len(entries)}); narrow --vars/--periph")

    # snapshot regions
    regions = []
    if snapshot in ("sections", "full"):
        for name, addr, size in elf.data_sections(target):
            regions.append((addr, size, name))
        regions.append(elf.stack_region(target) + ("stack",))
    elif snapshot == "stack":
        regions.append(elf.stack_region(target) + ("stack",))
    if snapshot == "full":
        lo, hi = (0x20000000, 0x20000000 + (520 if target == "rp2350" else 264) * 1024)
        regions = [(lo, hi - lo, "sram")]
    # merge/limit
    regions.sort()
    merged = []
    for a, l, n in regions:
        if merged and a <= merged[-1][0] + merged[-1][1] + 64:
            pa, pl, pn = merged[-1]
            end = max(pa + pl, a + l)
            merged[-1] = (pa, end - pa, pn + "+" + n)
        else:
            merged.append((a, l, n))
    regions = merged[:16]
    verbose("snapshot regions: " + ", ".join(f"{n}@0x{a:08X}({l}B)" for a, l, n in regions))

    # triggers
    trig = []
    trig_meta = []
    names = {m["name"]: i for i, m in enumerate(meta)}
    for spec in triggers:
        name, op, flags, value = parse_trigger(spec)
        if name not in names:
            raise SystemExit(f"trigger refers to unknown entry '{name}'")
        trig.append((names[name], op, flags, value))
        trig_meta.append(dict(spec=spec, id=names[name], op=op, flags=flags, value=value))

    with open(elf_path, "rb") as f:
        elf_hash = F.crc32(f.read())
    data, mhash = F.build_manifest(entries, [(a, l) for a, l, _ in regions], trig, target=tgt_code, level=level,
                                   on_fault=F.ONFAULT_HOLD if on_fault == "hold" else F.ONFAULT_RESUME,
                                   pc_sampling=pc_sampling, swd_hz=swd_hz, fast_period_us=fast_us,
                                   slow_period_us=slow_us, keyframe_us=keyframe_us,
                                   snapshot_on_fault=int(snapshot_on_fault), snapshot_on_reset=int(snapshot_on_reset),
                                   elf_hash=elf_hash)
    sidecar = dict(
        version=F.MANIFEST_VERSION, manifest_hash=mhash, elf=os.path.abspath(elf_path), elf_hash=elf_hash,
        build_id=elf.build_id(), target=target, svd=svd_path, level=level, on_fault=on_fault,
        pc_sampling=pc_sampling, swd_hz=swd_hz, fast_us=fast_us, slow_us=slow_us, keyframe_us=keyframe_us,
        entries=meta, regions=[dict(addr=a, len=l, name=n) for a, l, n in regions], triggers=trig_meta,
    )
    with open(out_base + ".bin", "wb") as f:
        f.write(data)
    with open(out_base + ".json", "w") as f:
        json.dump(sidecar, f, indent=1)
    verbose(f"wrote {out_base}.bin ({len(data)} bytes, hash 0x{mhash:08X}) and {out_base}.json")
    est = sum(s for _, s, _ in entries)
    # ~48 SWD clocks per 32-bit transfer, x1.5 protocol overhead, +4 bookkeeping reads per frame (see the approach document)
    fast_words = sum(1 for e in entries if e[2] == F.CLASS_FAST) + 4
    slow_words = sum(1 for e in entries if e[2] == F.CLASS_SLOW)
    us_per_word = 48 * 1.5 / swd_hz * 1e6
    fast_t, slow_t = fast_words * us_per_word, slow_words * us_per_word
    load = fast_t / fast_us + slow_t / slow_us
    verbose(f"keyframe {est} B; fast frame ~{fast_words} transfers ≈ {fast_t:.0f} us every {fast_us} us, "
            f"slow frame ~{slow_words} ≈ {slow_t:.0f} us every {slow_us} us -> link load ≈ {100*load:.0f}% at {swd_hz/1e6:g} MHz")
    if load > 0.7:
        verbose("WARNING: the link cannot keep this rate; raise --fast-us/--slow-us, narrow --vars, or raise --swd-hz")
    return data, sidecar
