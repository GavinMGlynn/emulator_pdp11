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
| CPU single/double-operand set | **working** (P1a) | 90-instr `alu` battery byte-identical to SimH; unit tests |
| CPU branches / JMP / JSR / RTS / SOB / cc-ops | **working** (P1b) | `flow` probe (loop+subroutine) byte-identical to SimH; unit tests |
| Traps: BPT/IOT/EMT/TRAP/RTI/RTT + T-bit trace | **working** (P2a) | `trap`+`trace` probes byte-identical to SimH; unit tests |
| EIS (MUL/DIV/ASH/ASHC/XOR) | **working** (P2c) | `eis` probe (14 cases, results+flags) byte-identical to SimH |
| Memory-mapped PSW (0177776) | **working** | read/written the hardware way; `eis` probe + unit test |
| Odd-address (bus) + reserved-instruction traps | **working** (P2b) | `faults` probe byte-identical to SimH; abort via setjmp/longjmp |
| Interrupts (priority-gated) / PIRQ / RESET / WAIT | **working** (P2d) | `intr` probe byte-identical to SimH; unit tests |
| MARK | **working** (P2d) | `mark` probe byte-identical to SimH |
| **CPU (integer) — P1+P2 COMPLETE** | **done** | 10 SimH goldens + 36 unit tests |
| MMU (KT11, 22-bit, I/D) | not started | — |
| Cache + Unibus/Massbus timing | not started | — (the cycle-accuracy core) |
| FP11-C floating point | not started | — |
| Devices (KW11-L, DL11, RK, RP, TM/TU) | not started | — |
| Content boot (Unix V6/V7) | not started | — |
| Interactive SDL frontend | not started | — |
| Verified fast mode | not started | — |
| Other models (11/20…11/94) | not started | — |

### CPU — what actually works today
- General registers R0–R7, PSW condition codes (N Z V C) and T bit.
- All eight addressing modes incl. deferred, and the PC-relative / immediate /
  absolute forms of modes 2/3/6/7.
- **Full single- and double-operand instruction set (word + byte):**
  MOV/CMP/BIT/BIC/BIS/ADD/SUB and MOVB/CMPB/BITB/BICB/BISB; CLR/COM/INC/DEC/
  NEG/TST/ADC/SBC/ROR/ROL/ASR/ASL/SWAB/SXT and their byte forms — all condition
  codes verified byte-identical to SimH over the `alu` battery.
- **All branches** (BR/BNE/BEQ/BGE/BLT/BGT/BLE/BPL/BMI/BHI/BLOS/BVC/BVS/BCC/BCS),
  **JMP, JSR, RTS, SOB**, and the **condition-code operators** (SEx/CLx/NOP),
  with stack (R6) push/pop for subroutine linkage.
- Still no-ops until later phases: traps (BPT/IOT/EMT/TRAP/RTI/RTT), WAIT/RESET,
  EIS, MARK/MTPS/MFP* (P2); FP11 (P5). MMU is P3 (runs unmapped, low 16-bit).
  JMP/JSR to a register operand is illegal and currently ignored (P2 trap).

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
