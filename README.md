# pdp11 — a cycle-accurate PDP-11 emulator

[![CI](https://github.com/GavinMGlynn/emulator_pdp11/actions/workflows/ci.yml/badge.svg)](https://github.com/GavinMGlynn/emulator_pdp11/actions/workflows/ci.yml)

A from-scratch emulator of Digital's PDP-11 minicomputer, built **cycle-accurate
first** and centred on the **PDP-11/70** (the KB11-C processor with the 22-bit
KT11 MMU, FP11-C floating point, cache, and the Unibus + RH70 Massbus). The
11/70 is implemented as the full superset, and every other model in the range —
**11/03, 11/04, 11/05, 11/20, 11/23(+), 11/24, 11/34, 11/40, 11/44, 11/45,
11/53, 11/60, 11/70, 11/73(B), 11/83, 11/84, 11/93, 11/94** — is a verified
subset of it.

It boots **Unix V6 to an interactive root shell**, and the entire console
session is **byte-identical to SimH**.

```
@unix

login: root
# ls /
bin
dev
etc
lib
mnt
mnt2
rkunix
rkunix.40
tmp
unix
usr
usr2
#
```

## The two-oracle methodology

Correctness is never asserted on reasoning alone — every behaviour is checked
against a reference:

- **Architectural state → SimH.** Registers, PSW, memory, MMU, and device
  behaviour are diffed **byte-for-byte** against the vendored
  [open-simh](https://github.com/open-simh/simh) `pdp11` simulator. A Python
  harness (`tools/simh-oracle/`) assembles a bare-metal probe, runs it under both
  SimH and this emulator, and compares the canonical state dumps. The checked-in
  goldens in `tests/goldens/` make this a no-reference regression that CI runs on
  every push, on every platform, with no copyrighted media required.
- **Cycle timing → DEC documentation.** SimH models *no* timing, and the 11/70
  exposes no software-readable cycle counter, so instruction times cannot be
  self-measured or SimH-oracled. Every timing number is derived from the DEC
  KB11-C / PDP-11/70 Handbook tables and cites its source in
  `tools/simh-oracle/FINDINGS.md`.

## What works

Everything below is verified — SimH golden **[A]**, DEC-cited timing **[T]**, or
a booted-machine diff.

| Area | Status |
|------|--------|
| **CPU integer core** | Full single/double-operand set (word + byte), all 8 addressing modes, branches, JMP/JSR/RTS/SOB, condition-code ops, EIS (MUL/DIV/ASH/ASHC/XOR) |
| **Traps & interrupts** | BPT/IOT/EMT/TRAP, RTI/RTT + T-bit trace, reserved-instruction & odd-address & NXM aborts, 7-level priority-gated interrupts, PIRQ, WAIT/RESET, stack-limit yellow/red zones |
| **KT11 MMU** | 22-bit relocation, PAR/PDR (Kernel/Super/User × I/D), access-control & page-length aborts, MMR0-3 status, MMR1 register-delta log + MMR2 saved-PC, dual register sets, MFPI/MTPI/MFPD/MTPD |
| **FP11-C floating point** | Single & double precision, a bit-exact port of the arithmetic; FEC/FEA exception model and the FPE trap |
| **Timing** | KB11-C instruction times + the 11/70 two-way set-associative cache (hit/miss), EIS per-shift/per-operand times, FP11 preinteraction + execution times |
| **Devices** | KW11-L line clock, DL11 console, RK11/RK05, RP04 via RH70 Massbus, TM11/TU10 tape, **RL11/RL01-RL02**; the Unibus Map; the 11/70 CPU/system registers; I/O-page NXM |
| **Model range** | All 20 models subset per-model from the 11/70 (options, base instruction set, PSW mask, MMU registers, memory ceiling, quirks) — from SimH's `cpu_tab` |
| **Verified fast mode** | A deterministic full-machine-state hash (the identity oracle) + an exact idle-skip scheduler that jumps WAIT to the next scheduled event |
| **Frontends** | A deterministic **headless** frontend (boots disks, scriptable console) and an interactive **SDL3** frontend (VT terminal on the DL11 + a KY11 console panel) |

See [`docs/PROJECT_STATUS.md`](docs/PROJECT_STATUS.md) for the per-subsystem
detail and [`docs/COMPLETION_PLAN.md`](docs/COMPLETION_PLAN.md) for the phased
roadmap.

## Building

Requires CMake ≥ 3.21, Ninja, and Clang (C23). Only the Unity submodule is
needed to build and test — SimH is required *only* to regenerate goldens.

```sh
git submodule update --init ext/unity
cmake --preset linux-debug        # or macos-* / windows-*
cmake --build --preset linux-debug
ctest --preset linux-debug        # goldens vs SimH + Unity unit suites
```

The probe/golden regression is emulated architectural state, so it must be
**bit-identical across platforms and build types** — CI runs it on Linux,
Rocky, macOS, and Windows, on both the `-ci` (`-O0`) and `-release` presets.

```sh
ctest --preset linux-release      # must match the debug run exactly
```

## Running

**Headless** — boot a disk image and drive the console with a scripted dialog
(`expect|send|…`, paced in emulated time):

```sh
./build/linux-debug/src/frontend/headless/pdp11_headless \
    --boot-rk unix0_v6_rk.dsk \
    --dialog '@|unix\r|login: |root\r|# |ls /\r'
```

**Interactive (SDL3)** — a VT-style terminal plus a lights-and-switches console
panel; built only if SDL3 is found via `pkg-config`:

```sh
./build/linux-debug/src/frontend/sdl/pdp11_sdl --boot-rk unix0_v6_rk.dsk
```

> Disk/tape images and ROMs are not distributed with this repository
> (`roms/` and `images/` are gitignored). Supply your own media.

## Repository layout

```
src/core/        the emulator, a static library with zero frontend deps
  cpu/  mmu/  fp/  cache/  timing/  clk/  console/  devices/  memory/
src/frontend/
  headless/      deterministic frontend (boots, scriptable console, state dump)
  sdl/           interactive SDL3 frontend (terminal + console panel)
tests/           Unity unit suites + the checked-in SimH goldens
tools/
  probes/        Python → PDP-11 memory-image encoder (no toolchain needed)
  simh-oracle/   drives SimH as the architectural oracle; FINDINGS.md
docs/
  PROJECT_STATUS.md   what works + how it's verified
  COMPLETION_PLAN.md  the phased roadmap
  references/         DEC manuals (KB11-C, FP11-C, handbooks)
ext/             vendored submodules: Unity (tests), SimH (oracle)
```

`src/core` never depends on a frontend; frontends depend on the core, never the
reverse.

## License

The first-party code in `src/`, `tests/`, and `tools/` is released under the
**MIT License** (see [`LICENSE`](LICENSE)) — use it freely, no warranty. Vendored
dependencies under `ext/` keep their own licenses — **SimH** is MIT/X11, **Unity**
is MIT. The DEC manuals under `docs/references/` are © Digital Equipment
Corporation, included for reference.

## Acknowledgements

Verified throughout against [open-simh](https://github.com/open-simh/simh), the
architectural oracle, and the original DEC KB11-C and FP11-C maintenance manuals
and PDP-11/70 handbooks for cycle timing.
