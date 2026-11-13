#!/usr/bin/env python3
"""Universal Black Box - command line tool.

  ubb.py manifest  firmware.elf [-o manifest] [--vars PAT ...] [--periph PAT ...] [--trigger SPEC ...]
  ubb.py upload    manifest.bin            upload to the probe (restarts recording with it)
  ubb.py status                            probe/link/recorder status
  ubb.py pull      [-o capture.ubb]        copy the flash ring to the PC (and decode if -m given)
  ubb.py decode    capture.ubb -m manifest.json [-o out_dir]
  ubb.py erase | mark [--snapshot] | start | stop | clear
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ubb import format as F  # noqa: E402


def cmd_manifest(a):
    from ubb.manifest import generate
    generate(a.elf, a.out, target=a.target, svd_path=a.svd, var_patterns=a.vars or ["*"], exclude=a.exclude or [],
             periph_patterns=a.periph, no_default_periph=a.no_default_periph, pins=a.pins or [],
             snapshot=a.snapshot, triggers=a.trigger or [], level=0 if a.passive else 1, on_fault=a.on_fault,
             pc_sampling=a.pc_sampling, swd_hz=a.swd_hz, fast_us=a.fast_us, slow_us=a.slow_us,
             keyframe_us=a.keyframe_us, snapshot_on_fault=not a.no_snapshot_on_fault,
             snapshot_on_reset=a.snapshot_on_reset, max_vars=a.max_vars, include_sdk=a.include_sdk)


def _probe(a):
    from ubb.link import Probe
    return Probe(a.port)


def cmd_status(a):
    p = _probe(a)
    info = p.ping()
    st = p.status()
    print(f"probe on {p.port}: fw {info['fw_major']}.{info['fw_minor']}, session 0x{info['session_id']:08X}, "
          f"target={'rp2350' if info['target_type']==2 else 'rp2040'}, manifest "
          f"{'0x%08X' % info['manifest_hash'] if info['manifest_valid'] else 'NONE (built-in default)'}")
    link = {0: "no target", 1: "attached", 2: "held (halted)"}.get(st["link_state"], "?")
    print(f"recorder: {'RECORDING' if st['recording'] else 'stopped'}, link {link}, level L{st['level']}, "
          f"event LED {'ON' if st['event_latched'] else 'off'}")
    print(f"target: DPIDR=0x{st['dpidr']:08X} DHCSR=0x{st['last_dhcsr']:08X} faults={st['faults_captured']} resets={st['resets_seen']}")
    print(f"link:   {st['words_per_s']} transfers/s, {st['frames']} frames, WAIT={st['wait_acks']} FAULT={st['fault_acks']}, "
          f"overruns={st['overruns']}")
    print(f"flash:  {st['valid_sectors']}/{info['log_sectors']} sectors hold data, {st['blocks_written']} written since boot, "
          f"next seq {st['next_seq']}, {st['ram_blocks_used']} blocks staged in RAM, uptime {st['uptime_us']/1e6:.0f} s")


def cmd_upload(a):
    p = _probe(a)
    with open(a.manifest, "rb") as f:
        data = f.read()
    p.upload_manifest(data, progress=lambda d, t: print(f"\r  {d}/{t} bytes", end=""))
    print("\nmanifest stored; recorder restarted with it")
    time.sleep(0.5)
    cmd_status(a)


def cmd_pull(a):
    p = _probe(a)
    t0 = time.time()
    info, blocks = p.pull(progress=lambda i, n, k: print(f"\r  sector {i}/{n}, {k} valid blocks", end=""), min_seq=a.since)
    print()
    out = a.out or time.strftime("capture_%Y%m%d_%H%M%S.ubb")
    with open(out, "wb") as f:
        for _, raw in blocks:
            f.write(raw)
    size = sum(len(r) for _, r in blocks)
    print(f"wrote {out}: {len(blocks)} blocks, {size} bytes in {time.time()-t0:.1f} s")
    if a.manifest:
        a.capture = out
        a.outdir = a.outdir or os.path.splitext(out)[0]
        cmd_decode(a)


def cmd_decode(a):
    from ubb.decode import Decoder
    outdir = a.outdir or os.path.splitext(a.capture)[0] + "_decoded"
    d = Decoder(a.capture, a.manifest, outdir, elf_path=a.elf).run()
    files = d.write_all()
    print(f"{d.stats['blocks']} blocks, {d.stats['records']} records -> {len(d.events)} events, "
          f"{d.stats['faults']} fault captures, {len(d.snapshots)} snapshots, {d.stats['gaps']} gaps")
    for w in d.warnings[:10]:
        print("warning:", w)
    for f in files:
        print("  ", f)
    if a.show:
        with open(files[1], encoding="utf-8") as f:
            sys.stdout.write(f.read())


def cmd_simple(a):
    p = _probe(a)
    if a.cmd == "erase":
        print("erasing log ring (can take a while)...")
        p.erase_log()
    elif a.cmd == "mark":
        p.mark(a.id, a.snapshot)
    elif a.cmd == "start":
        p.rec_start()
    elif a.cmd == "stop":
        p.rec_stop()
    elif a.cmd == "clear":
        p.clear_event()
    print("ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port of the probe (auto-detected)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("manifest", help="build manifest from ELF (+SVD)")
    m.add_argument("elf")
    m.add_argument("-o", "--out", default="manifest", help="output base name (.bin and .json)")
    m.add_argument("--target", choices=["rp2040", "rp2350"], help="target chip (auto from ELF)")
    m.add_argument("--svd", help="CMSIS-SVD file (auto from pico-sdk)")
    m.add_argument("--vars", nargs="*", help="glob patterns of globals to record (default all)")
    m.add_argument("--exclude", nargs="*", help="glob patterns to skip")
    m.add_argument("--max-vars", type=int, default=200)
    m.add_argument("--include-sdk", action="store_true", help="also scan pico-sdk/framework compilation units")
    m.add_argument("--periph", nargs="*", help="extra PERIPH.REG globs (allowlist)")
    m.add_argument("--no-default-periph", action="store_true")
    m.add_argument("--pins", nargs="*", type=int, help="GPIO numbers whose IO_BANK0 CTRL to record")
    m.add_argument("--snapshot", choices=["sections", "stack", "full", "none"], default="sections")
    m.add_argument("--trigger", action="append", help="e.g. 'state==5:snap', 'change:errors', 'bitset:flags:0x4'")
    m.add_argument("--passive", action="store_true", help="L0 only: no debug config writes, no halts")
    m.add_argument("--on-fault", choices=["resume", "hold"], default="resume")
    m.add_argument("--pc-sampling", type=int, default=10, help="PC sample every N fast frames (0=off)")
    m.add_argument("--swd-hz", type=int, default=4_000_000)
    m.add_argument("--fast-us", type=int, default=1000)
    m.add_argument("--slow-us", type=int, default=10_000)
    m.add_argument("--keyframe-us", type=int, default=1_000_000)
    m.add_argument("--no-snapshot-on-fault", action="store_true")
    m.add_argument("--snapshot-on-reset", action="store_true")
    m.set_defaults(fn=cmd_manifest)

    u = sub.add_parser("upload", help="upload manifest.bin to the probe")
    u.add_argument("manifest")
    u.set_defaults(fn=cmd_upload)

    s = sub.add_parser("status")
    s.set_defaults(fn=cmd_status)

    pl = sub.add_parser("pull", help="copy the flash ring to the PC")
    pl.add_argument("-o", "--out")
    pl.add_argument("-m", "--manifest", help="manifest.json sidecar: decode right away")
    pl.add_argument("--elf")
    pl.add_argument("--outdir")
    pl.add_argument("--since", type=int, default=0, help="only blocks with seq >= N")
    pl.add_argument("--show", action="store_true", help="print trail.txt after decoding")
    pl.set_defaults(fn=cmd_pull)

    d = sub.add_parser("decode")
    d.add_argument("capture")
    d.add_argument("-m", "--manifest", required=True, help="manifest.json sidecar")
    d.add_argument("--elf", help="override ELF path from the sidecar")
    d.add_argument("-o", "--outdir")
    d.add_argument("--show", action="store_true")
    d.set_defaults(fn=cmd_decode)

    for name, hlp in [("erase", "erase the log ring"), ("start", "start recording"), ("stop", "stop recording"),
                      ("clear", "turn the event LED off")]:
        x = sub.add_parser(name, help=hlp)
        x.set_defaults(fn=cmd_simple)
    mk = sub.add_parser("mark", help="write a MARK event (optionally with a memory snapshot)")
    mk.add_argument("--id", type=int, default=1)
    mk.add_argument("--snapshot", action="store_true")
    mk.set_defaults(fn=cmd_simple)

    a = ap.parse_args()
    try:
        a.fn(a)
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
