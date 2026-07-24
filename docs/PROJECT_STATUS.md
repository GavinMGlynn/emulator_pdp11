# PROJECT STATUS — PDP-11 emulator

Single source of truth for *what works*. Updated in the **same commit** as the
code it describes (emulator-setup-guide.md §10).

## Accuracy goal & the two oracles

Target: **cycle-accurate**, PDP-11/70 (KB11-C) first, then broadened to the full
SimH model range. Two oracles verify two different things:

- **Architectural state → SimH** (vendored at `ext/simh`, MIT/X11 licensed).
  Registers, PSW, memory, and device behaviour are diffed against SimH via
  `tools/simh-oracle/`. SimH is authoritative for *what* the machine computes.
- **Cycle timing → DEC documentation** (KB11-C timing tables / flow charts,
  PDP-11/70 handbook). SimH models **no** timing, and the 11/70 exposes no
  software-readable cycle counter, so timing cannot be self-measured or SimH-
  oracled. Every timing number is derived from DEC formulas and cites its
  source in `tools/simh-oracle/FINDINGS.md`.

No accuracy claim is made yet — only the subsystems below are verified.

## Subsystem status

| Subsystem | Status | Verified by |
|-----------|--------|-------------|
| Build / CI / oracle harness | **working** (P0) | CTest green on debug+release; golden vs SimH |
| CPU integer core | **seed only** | MOV/ADD/HALT + all addressing modes; unit + 1 golden |
| CPU full instruction set | not started | — |
| EIS (MUL/DIV/ASH/ASHC) | not started | — |
| Traps / interrupts / PIRQ | not started | — |
| MMU (KT11, 22-bit, I/D) | not started | — |
| Cache + Unibus/Massbus timing | not started | — (the cycle-accuracy core) |
| FP11-C floating point | not started | — |
| Devices (KW11-L, DL11, RK, RP, TM/TU) | not started | — |
| Content boot (Unix V6/V7) | not started | — |
| Interactive SDL frontend | not started | — |
| Verified fast mode | not started | — |
| Other models (11/20…11/94) | not started | — |

### CPU seed — what actually works today
- General registers R0–R7, PSW condition codes (N Z V C) and T bit.
- All eight addressing modes incl. deferred, and the PC-relative / immediate /
  absolute forms of modes 2/3/6/7.
- Instructions: `MOV`, `ADD`, `HALT`. Unimplemented opcodes are currently
  no-ops (a real reserved-instruction trap arrives with P2).
- Runs unmapped in the low 16-bit address space (MMU is P3).

## Deliberate approximations (with reason & cost to close)
| Approximation | Reason | Cost to close |
|---------------|--------|---------------|
| Unimplemented opcodes are no-ops | first slice only proves the harness | folded into P1/P2 (trap to 4/10) |
| No timing yet (`instr_count` is a placeholder, not cycles) | need KB11-C tables (P4) | P4: cache + bus timing model |
| Flat RAM, 16-bit addresses only | MMU/Unibus map are P3/P4 | P3/P4 |

## Target model range (user-confirmed)
11/20, 11/03, 11/04, 11/05, 11/23, 11/23+, 11/24, 11/34, 11/40, 11/44, 11/53,
11/60, 11/70, 11/73, 11/73B, 11/83, 11/84, 11/93, 11/94 — i.e. SimH's full
PDP-11 set. The 11/70 is built first as the superset the others subset down from.
