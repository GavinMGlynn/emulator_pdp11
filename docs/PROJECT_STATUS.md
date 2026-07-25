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
| MMU relocation (KT11, 22-bit, PAR/PDR) | **working** (P3a) | `mmu` probe byte-identical to SimH; unit test |
| Unibus Map (18-bit DMA → 22-bit physical) | **working** (P7c) | 31 registers at 0170200-0170377; RK/TM DMA relocates through it when MMR3<BME> set (mirrors SimH `Map_Addr`); unit tests; carries the V6 boot to `login:` |
| Non-existent-memory abort (vector 4) | **working** (P7c) | relocated PA ≥ installed memory (256 KB) aborts through vector 4, matching SimH; unit test |
| Dual register sets (PSW<11>) | **working** (P3b) | `regset` probe byte-identical to SimH; unit test |
| MFPI/MTPI/MFPD/MTPD (previous-mode access) | **working** (P3b) | `mfp` probe byte-identical to SimH; unit test |
| Full I/D-space separation | **working** (P3b) | `idspace` probe byte-identical to SimH; unit test |
| **MMU (KT11) — P3 COMPLETE** | **done** | relocation, aborts, banking, MFP*, I/D space |
| MMU access control / aborts / MMR0 status | **working** (P3c) | `mmuabort` probe byte-identical to SimH; unit test |
| Instruction timing (KB11-C tables) | **in progress** (P4a) | timing_suite vs Handbook App. C tables |
| 11/70 cache (hit/miss timing) | **working** (P4c) | cache_suite; KB11-C sec 2.2 |
| Per-cycle tick() core + Unibus/Massbus | not started (P4d) | — |
| FP11-C control (CFCC/SET*/LDFPS/STFPS/STST) | **working** (P5a) | `fp` probe byte-identical to SimH; unit tests |
| FP11-C load/store + CLR/TST/ABS/NEG | **working** (P5b) | `fpls` probe byte-identical to SimH; unit tests |
| FP11-C ADD/SUB (bit-exact port) | **working** (P5c) | `fpadd` probe byte-identical to SimH; unit test |
| FP11-C MUL/DIV/MOD/CMP | **working** (P5c) | `fparith`+`fpdbl` probes byte-identical to SimH; unit tests |
| FP11-C conversions (LDC/STC, LDEXP/STEXP) | **working** (P5c) | `fparith` probe byte-identical to SimH |
| FP11-C exception model (FEC/FEA, FPE trap) | **working** (P5c) | `fpe` probe (divide-by-zero → vector 0244) byte-identical to SimH; unit test |
| Device interrupt controller (BR levels/vectors) | **working** (P6a) | unit tests; exercised by the KW11-L path |
| KW11-L line clock | **working** (P6a) | `clk` register probe byte-identical to SimH; tick→interrupt unit tests |
| DL11 console SLU | **working** (P6b) | `dl11` register probe byte-identical to SimH; input/output + interrupt unit tests |
| RK11/RK05 disk (DMA) | **working** (P6c) | `rk11` register probe byte-identical to SimH; DMA read/write + interrupt unit tests |
| RP04 disk via RH70 Massbus (DMA) | **working** (P6d) | `rp11` register probe byte-identical to SimH; DMA read/write + interrupt unit tests |
| TM11/TU10 magtape (.tap) | **working** (P6e) | `tm11` register probe byte-identical to SimH; record read/write + file-mark + interrupt unit tests |
| Content boot (Unix V6/V7) | **boots to an interactive root shell** (P7a-d) | headless `--boot-rk` boots V6 from `@unix` to multi-user `login:`, logs in as `root`, and runs shell commands (`echo`, `ls /`) — the full console session is **byte-identical to SimH** (`ls /` → `bin dev etc lib mnt mnt2 rkunix rkunix.40 tmp unix usr usr2`). Required six real CPU/bus bugs found+fixed vs SimH (SBC-V, SPL, PSW-CC, NXM abort, interrupt-acknowledge, **Unibus Map**); the last step to the shell was a **frontend** fix — pacing the `--dialog` input in emulated time so a typed reply waits for the reader's `read()`/sleep instead of racing it (the CPU/kernel were already correct) |
| Interactive SDL frontend | **working** (P8a/b) | `pdp11_sdl`: SDL3 window, VT terminal on the DL11 console + KY11 console (address/data/switch-register lamps, RUN light, and mouse-driven LOAD-ADDR/EXAM/DEP/START/HALT/CONT/STEP switches); keyboard→DL11; boots V6 to an interactive shell. Optional (pkg-config SDL3, skipped if absent). Two headless CTests: `sdl_frontend_smoke` (dummy driver, `--frames`) and `sdl_console_panel` (`--selftest`) |
| Verified fast mode — identity harness | **working** (P9a) | `pdp11_state_hash` — a 64-bit FNV-1a over all architectural + timing state (registers, banked files, MMU, FP, interrupt/clock/console, device registers, cache, instruction/time accounting, all of physical memory, and attached disk/tape media), excluding host pointers; unit tests prove it is deterministic across equal runs and sensitive to a single-word change. This is the oracle a fast mode must match bit-for-bit |
| Verified fast mode — idle-skip scheduler | **working** (P9b) | `pdp11_next_event_ns` returns the earliest scheduled event of **any** subsystem (line clock, console transmit, RK/RP/TM completion); a WAIT jumps emulated time straight to it and services it — matching SimH's event-queue semantics (the prior skip advanced only to the clock). Unit tests prove the skip lands on exactly the earliest deadline across subsystems; the full V6 boot console stream stays **byte-identical to SimH**, and its end-of-boot machine-state hash is reproducible across runs (`e9389aa0b4b3da86` over ~49.9 M instructions) |
| **Verified fast mode — P9 COMPLETE** | **done** | identity harness + exact idle-skip, boot stream byte-identical to SimH, boot-state hash reproducible |
| Model range — descriptor + option gating | **in progress** (P10a) | `pdp11_model` enum + `pdp11_model_info` table for all 20 models (11/03…11/94), transcribed from SimH `cpu_tab`; `pdp11_cpu_create_model()` (create() = the full 11/70, so goldens/boot unchanged — boot hash still `e9389aa0b4b3da86`). Optional instruction sets gated by model: **EIS** (MUL/DIV/ASH/ASHC) and **FP11** trap through vector 10 on models that lack them (11/20 EIS+FP, 11/34 FP); memory ceiling per address width (64 KiB/256 KiB/4 MiB). Unit tests vs the SimH model table |
| Model range — base-instruction subsetting | **working** (P10b) | SXT/SOB/XOR (`has_sxs`), MARK (`has_mark`), RTT (`has_rtt`), SPL (`has_spl`) and MFPT gated per model from SimH's `HAS_*` masks: the extended base set traps through vector 10 on the earliest machines (11/04/05/20), RTT only on 11/05/20, SPL only on 11/44/45/70 + J-class, MFPT returns the model type code (F=3, 44=1, J=5) where present and is reserved elsewhere incl. the 11/70. Per-model unit tests |
| Model range — PSW-mask subsetting | **working** (P10c) | `put_psw` masks writes to the model's `psw_mask` (SimH `put_PSW: val &= cpu_tab.psw`): the modeless low-end machines (11/20 etc., mask 0000377) drop the current/previous-mode and register-set fields, so a program can never enter a mode the CPU lacks; the reserved MBZ bits (8-10) are unwritable on every model. Per-model unit tests; V6 boot hash unchanged |
| Model range — MMU-register masks | **working** (P10d) | MMR0/MMR3/PAR/PDR writes masked to the model's bit-widths (SimH `cpu_tab` mm0/mm3/par/pdr): MMR3 absent (mask 0, reads 0) on the 11/34/40/60 and no-MMU machines; PAR narrows to 12 bits on the 18-bit models; on a no-MMU model all four read 0. The 11/70 masks touch only unused bits, so the boot is unchanged (hash `e9389aa0b4b3da86`). Per-model unit tests. (Full I/O-page NXM-on-access to an absent register is a documented Unibus tail) |
| Other models — remaining detail (Q-bus device set) | not started (P10e) | — |

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
