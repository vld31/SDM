# Universal Black Box — SWD flight recorder for RP2040 / RP2350

A Raspberry Pi **Pico 2 W** that records what an *unmodified* RP2040 (PicoBricks) or
RP2350 target does, using nothing but the two SWD wires and the target's ELF file.
It is a **black box, not a debugger**: it never stops the target to look at it,
it writes a time-stamped stream of *events* into its own flash in a ring, and
when the target crashes it takes a post-mortem picture (registers, stacked frame,
RAM regions) and lets the target carry on.  Afterwards you plug the probe into a
laptop, pull the ring over USB and the decoder rebuilds the story.

The design follows [Pico SWD black box approach.md](Pico%20SWD%20black%20box%20approach.md)
(four capture classes, allowlisted peripherals, intrusiveness levels, CRC'd flash
ring, change-only encoding, offline decoder).

```
  target (RP2040)  ──SWCLK/SWDIO/GND──▶  probe (Pico 2 W)  ──USB──▶  laptop
  runs unmodified                        core0: reads + records        ubb.py pull / decode
                                         core1: flash ring + USB       events.jsonl, trail.txt,
                                                                       timeline.vcd, trace.json
```

## Hardware

| Probe pin (Pico 2 W) | Function |
| --- | --- |
| GP2 | SWCLK → target SWCLK |
| GP3 | SWDIO → target SWDIO |
| GND | → target GND |
| GP12 | **RUN** LED (active LOW): on = recording a live target, blinking = no target found |
| GP18 | **EVENT** LED (active LOW): on after a fault / snapshot / trigger was captured (`ubb.py clear` turns it off) |
| GP‑x (optional) | push button to GND for a manual MARK + snapshot; set `UBB_PIN_TRIGGER` in `firmware/src/config.h` |

Keep SWD leads short (≤ 15 cm) with a ground next to them.  The default SWD clock
is 4 MHz; the manifest can raise it (`--swd-hz 10000000`).

## Repository layout

```
firmware/       probe firmware (pico-sdk, C)       -> firmware/build/ubb_probe.uf2
  src/swd.pio     PIO SWD bit engine (debugprobe style, own code)
  src/swd.c       SWD transactions, dormant/JTAG-to-SWD sequences, TARGETSEL
  src/dap.c       ADIv5 (RP2040 multidrop) and ADIv6 (RP2350) DP/AP, memory + core access
  src/sampler.c   THE RECORDER: frames, change detection, fault catch, snapshots, triggers
  src/stream.c    record stream staged in 4 KB RAM blocks (core0 -> core1)
  src/flashlog.c  CRC'd sector ring in the probe flash + manifest slots
  src/host.c      USB protocol + flash writer (core1)
  src/ubb_format.h  the on-flash/USB contract shared with the PC tools
tools/          PC side (Python 3, pyelftools + pyserial)
  ubb.py          CLI: manifest / upload / status / pull / decode / erase / mark / start / stop / clear
  ubb/elfinfo.py  DWARF globals, struct/bitfield/enum expansion, addr -> file:line
  ubb/svdinfo.py  SVD registers + read-side-effect denylist
  ubb/decode.py   capture -> events.jsonl, trail.txt, timeline.vcd, trace.json, snapshots
  tests/          synthetic round-trip test
target_demo/    an unmodified RP2040 "machine" that runs a state machine and crashes
```

## Build and flash the probe

Requirements: the toolchain the *Raspberry Pi Pico* VS Code extension installs
(`%USERPROFILE%\.pico-sdk`, SDK 2.3.1, arm-none-eabi 14.2).  Adjust the versions
at the top of `build.ps1` if yours differ.

```powershell
.\firmware\build.ps1
```

Hold BOOTSEL on the Pico 2 W, plug USB, copy `firmware\build\ubb_probe.uf2` to the
`RP2350` drive.  Both LEDs flash once at boot (lamp test).  Then the RUN LED blinks
until a target answers on SWD and stays on while recording.

The firmware is `copy_to_ram`: core 0 samples from SRAM while core 1 erases and
programs the flash ring, so writing the log never stalls the recorder.

## PC tools

```powershell
py -m pip install -r tools\requirements.txt
```

### 1. Describe what to record (manifest)

```powershell
py tools\ubb.py manifest path\to\firmware.elf -o manifest --pins 7 10 --trigger "g_state==6:snap"
```

* Globals are taken from DWARF (`DW_OP_addr` variables in RAM, structs, arrays,
  bitfields, enums expanded) from the application's own compilation units only
  (`--include-sdk` to scan the framework too); narrow with `--vars "g_*" "state*"`.
* Peripheral registers come from the RP2040/RP2350 SVD through an **allowlist**
  (`GPIO_IN`, PWM CSR/CC/TOP, ADC CS/RESULT, UART FR/CR…, `--periph "PWM.*"` for
  more, `--pins N` for IO_BANK0 CTRL of pins).  Every register is checked against
  the SVD `readAction` and a denylist of pop/latch-on-read registers (`FIFO_RD`,
  `TIMELR/TIMEHR`, `UARTDR`, PIO `RXF`, `SSPDR`, I2C `IC_CLR_*`, spinlocks…).
  A refused register is printed and dropped: the worst case is a register we did
  not record, never a register we corrupted.
* Snapshot regions default to `.data/.bss/vector table + stack` (`--snapshot full`
  for all SRAM, `stack`, or `none`).
* `--trigger` writes a TRIGGER event (and `:snap` a memory snapshot) when a
  variable crosses a value: `var==5`, `var>100:snap`, `change:var`, `bitset:flags:0x4`.
* `--passive` keeps the recorder at level L0 (reads only, no fault catch).
  Default is L0+L1: `DEMCR.VC_HARDERR`/`DWTENA` and `C_DEBUGEN` are written once,
  and the core is halted **only** at a HardFault/lockup, for the few hundred
  microseconds it takes to read 17 registers and the stacked frame, then released
  (`--on-fault hold` keeps it halted instead).
* The tool prints the link budget (transfers per frame vs. the frame period) and
  warns if the SWD link cannot keep the requested rate.

Output: `manifest.bin` (what the probe gets — only addresses, sizes, classes,
regions, triggers) and `manifest.json` (names, types, enums, fields, ELF path —
what the decoder uses).  The probe never sees a symbol name.

PlatformIO note: release builds have no DWARF.  Add `-g` to `build_flags`
(code generation is unchanged; compare the `.text` hash to prove it).

### 2. Load it and record

```powershell
py tools\ubb.py upload manifest.bin     # stored in probe flash, recorder restarts with it
py tools\ubb.py status                  # link, rates, WAIT/FAULT counts, sectors used
```

The manifest survives power cycles; the probe records autonomously from power-up.
`ubb.py mark --snapshot` writes a manual MARK (+ RAM snapshot) from the laptop.

### 3. Pull and decode

```powershell
py tools\ubb.py pull -m manifest.json --show
```

reads every sector of the ring, keeps the CRC-valid blocks sorted by sequence
number, writes `capture_<date>.ubb` and decodes it into
`capture_<date>/`:

* `trail.txt` – the human story: attach, reset cause, every variable change with
  enum names and register fields, pin edges, triggers, marks, the fault with
  registers, DFSR bits, faulting instruction and return address as `file:line`,
  snapshot files, halt duration, gaps/overruns, PC-sample profile, last values.
* `events.jsonl` – one JSON event per line (`t_us` on the probe clock).
* `timeline.vcd` – open in GTKWave/Surfer: every variable as a wire, faults as events, PC.
* `trace.json` – open in Perfetto (`ui.perfetto.dev`) or `chrome://tracing`:
  counters per variable, function occupancy from PC samples, instant events.
* `snapshot_NN_<addr>.bin` – raw memory regions, and `snapshot_NN.dump`
  (CrashCatcher-style hex dump for CrashDebug + GDB, experimental).

`ubb.py erase` clears the ring; `stop`/`start` pause/resume the recorder.

## What is recorded (record stream)

Every 4 KB flash block carries a header (magic, sequence, 64-bit probe timestamp,
manifest hash, session id, CRC32) and self-describing records with LEB128 Δt:

| Record | Written when |
| --- | --- |
| `KEYFRAME` | every second and at the start of every block (all values; blocks decode on their own) |
| `CHANGE` | a watched variable/register changed (fast frame 1 kHz, peripheral frame 100 Hz by default) |
| `PCSAMPLE` | DWT PC sample every N fast frames (statistical execution view, no halt) |
| `TARGET_TIME` | target `TIMERAWL` every 100 ms (clock correlation; freezes reveal halts/resets) |
| `STATUS` | attached / lost / reset seen / lockup / halted / resumed / held / armed / overrun / snapshot done / rec start-stop |
| `RESET_CAUSE` | after attach and after every observed reset (`CHIP_RESET`, `WATCHDOG.REASON`) |
| `FAULT` | halt caught by `VC_HARDERR`, lockup, BKPT, mark or trigger: DHCSR/DFSR/ICSR, r0–r12/sp/lr/pc/xPSR, 8-word stacked frame |
| `SNAPSHOT` | RAM regions in 1 KB chunks after a fault/mark/trigger/reset |
| `TRIGGER` / `MARK` | value trigger fired / host or button mark |
| `LINKSTAT` | once a second: transfers/s, WAIT/FAULT ACKs, frames, drops |
| `SESSION` / `CORE1` | probe boot info / core‑1 DHCSR state changes |

Intrusiveness levels, as declared in the `SESSION` record:
**L0** passive AHB-AP reads only; **L1** debug configuration writes (`DEMCR`, `DHCSR.C_DEBUGEN`);
**L2** halt-read-resume on a fault only.  Forbidden by construction: Rescue DP,
target memory writes, SIO writes, reset requests, side-effect reads.

Flash budget: 3 MB ring (768 sectors) on the 4 MB Pico 2 W; a partial block is
committed at the latest every 5 s, so a power pull loses at most the last 5 s.

## Demo target

`target_demo/` is a PicoBricks-style RP2040 program (LED PWM on GP7, button GP10,
buzzer GP20, potentiometer ADC0) with a state machine (`g_state`), counters, a
bitfield struct, a float filter, a char ring and — after 90 s or a long button
press — a NULL function call that hard-faults and ends in a watchdog reboot.
It is compiled `-O2 -g`, like a PlatformIO release build with `-g` added.

```powershell
.\target_demo\build.ps1                                   # -> ubb_target_demo.uf2 / .elf
py tools\ubb.py manifest target_demo\build\ubb_target_demo.elf -o demo --pins 7 10 --trigger "g_state==6:snap"
py tools\ubb.py upload demo.bin
# ... let it run, press the button, wait for the crash ...
py tools\ubb.py pull -m demo.json --show
```

Expected trail: attach → reset cause → `g_state: ST_IDLE -> ST_RAMP_UP` …
→ `PWM.CH3_CC` duty steps → `g_state -> ST_FAILING` (TRIGGER fires, snapshot) →
`HARDFAULT … faulting instruction: main() src/main.c:NNN` → snapshot files →
`target clock went backwards (reset?)` → `last reset cause: watchdog timer`.

## Testing without hardware

```powershell
py tools\tests\test_roundtrip.py
```

encodes a synthetic capture exactly as the firmware does (keyframes, changes,
trigger, fault, snapshot, gap) and checks the decoder's events, snapshot files
and manifest CRC; with the demo ELF built it also checks address → `file:line`.

## Status / open items

* Firmware builds (`ubb_probe.uf2`, 53 KB, runs from RAM) and tools pass the
  round-trip test; **the SWD path has not yet been exercised on a live target**.
  Phase 0 of the approach document (measure transfers/s at 4/10/24 MHz, verify the
  RP2040 multidrop attach, core‑1 DHCSR poll, watchdog behaviour during polling)
  is the next bench step — `ubb.py status` shows the measured transfers/s.
* RP2350 targets use the ADIv6 profile (AP 0x2000/0x4000); untested.
* The CrashDebug dump format is best-effort.
