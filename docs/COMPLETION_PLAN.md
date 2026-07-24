# COMPLETION PLAN — PDP-11 emulator

Phased road to done. Each item names its **verification**. Nothing is "done" on
reasoning alone (emulator-setup-guide.md §10/§12). Newly discovered tails go into
this file the moment they're found, in the same commit.

Verification legend: **[A]** = architectural diff vs SimH oracle;
**[T]** = timing golden derived from DEC docs (cites source in FINDINGS.md);
**[C]** = content/integration boot.

---

## P0 — Scaffold & oracle loop ✅ DONE
- [x] Tree, C23 + CMake/Ninja/Clang, presets, strict warnings.
- [x] `ext/` submodules: Unity, SDL3 (pending), SimH oracle built.
- [x] Headless frontend; SimH oracle harness; `regress.py` golden in CTest.
- [x] CPU seed: MOV/ADD/HALT + all addressing modes. **[A]** golden `add3`.
- [ ] GitHub Actions 4-target matrix green. *(committed; awaiting first push)*

## P1 — CPU integer core
- [x] **P1a** All single/double-operand instructions (CLR/COM/INC/DEC/NEG/TST/
      ROR/ROL/ASR/ASL/SWAB/ADC/SBC/SXT; MOV/CMP/BIT/BIC/BIS/ADD/SUB; byte
      variants) incl. the byte-to-register sign-extension quirk. **[A]** `alu`
      battery byte-identical to SimH + 18 unit tests.
- [x] **P1b** Branches (BR/BNE/BEQ/BPL/BMI/BCS/BCC/BVS/BVC/BGE/BLT/BGT/BLE/BHI/
      BLOS), JMP, JSR/RTS, SOB, condition-code ops (SEx/CLx). **[A]** `flow`
      probe (SOB loop + JSR/RTS subroutine + branches) byte-identical to SimH,
      plus a two-pass probe assembler (`tools/probes/asm.py`).
- [x] **P1c** Odd-address handling (done in P2b) and reserved-mode handling:
      JMP/JSR to a register is illegal on the 11/70 (no HAS_JREG4) and traps
      through vector 010. **[A]** `jmpreg` probe byte-identical to SimH.
- *Verify:* instruction-matrix probes **[A]**. Per-instruction cycle goldens
  **[T]** are deferred to P4 (they need the `tick()`/cache timing model).

## P2 — Traps, interrupts, EIS
- [x] **P2a** Trap mechanism + BPT/IOT/EMT/TRAP vectors, RTI/RTT, T-bit trace
      (incl. the RTI-immediate vs RTT-deferred distinction). **[A]** `trap` +
      `trace` probes byte-identical to SimH.
- [x] **P2b** Reserved/illegal-instruction trap (vec 10, 11/70 illegal opcodes)
      and odd-address / bus trap (vec 4), via a setjmp/longjmp abort path (reused
      for MMU aborts at P3). **[A]** `faults` probe byte-identical to SimH.
      Stack-limit (yellow/red) and full illegal-opcode coverage tighten as FP/MMU
      land.
- [x] **P2c** EIS: MUL, DIV, ASH, ASHC, XOR. **[A]** `eis` probe (14 cases,
      results + flags via the memory-mapped PSW at 0177776) byte-identical to
      SimH. (MFPS/MTPS confirmed illegal on the 11/70 — deferred to per-model P10.)
- [x] **P2d** Priority-gated interrupts + PIRQ (0177772, vector 0240) + RESET +
      WAIT + MARK. **[A]** `intr` and `mark` probes byte-identical to SimH; WAIT
      unit-tested (async device wake tested with the KW11-L clock at P6). Device
      BR4-7 interrupts join this same path at P6.

**P2 COMPLETE.** With P1, the 11/70 integer CPU is done: full instruction set,
addressing modes, EIS, traps, interrupts, the memory-mapped PSW/PIRQ — 10 SimH
goldens + 36 unit tests, green on debug and release.
- *Verify:* trap/interrupt probes **[A]**; EIS probes **[A]**. Timing **[T]** at P4.

## P3 — Memory management (KT11, 22-bit)
- [x] **P3a** 22-bit address relocation: MMR0<0> enable, MMR3<M22E>, the full
      PAR/PDR I/O-page register file (Kernel/Super/User × I/D), and the
      VA→PA translation, with all CPU accesses (fetch, operands, stack, vectors)
      routed through it. MMU-off stays identity (existing 11 goldens unchanged).
      **[A]** `mmu` probe (relocate a write and read it back through a second
      mapping) byte-identical to SimH.
- [~] **P3b** Dual register sets (PSW<11> banking of R0-R5) done via a central
      put_psw() that also banks the per-mode SP; do_trap now sets the previous-
      mode field and pushes to the new mode's stack; RTI and PSW writes route
      through put_psw. **[A]** `regset` probe byte-identical to SimH.
- [x] **P3b** MFPI/MTPI/MFPD/MTPD via mode-generalized relocation (cpu_read/
      write_word_mode); `regset` (register sets) and `mfp` (kernel reads user
      space through the previous-mode mapping) probes byte-identical to SimH.
      **[A]** `mfp` probe byte-identical to SimH.
- [x] **P3b** Full I/D-space separation: instruction fetch + inline immediate/
      absolute words use I-space; data + stack + pointers use D-space (per-mode
      MMR3 enable); trap vectors read in Kernel D-space. **[A]** `idspace` probe
      byte-identical to SimH.

**P3 COMPLETE.** The KT11 MMU: 22-bit relocation, access-control/page-length
aborts, dual register sets, per-mode SP banking, MFPI/MTPI/MFPD/MTPD, and I/D
separation — 5 MMU goldens + unit tests. Small tails: MMR1/MMR2 (auto-inc/dec
recovery) and stack-limit yellow/red.
- [x] **P3c** Access control (PDR ACF) + page-length aborts → vector 0250, with
      MMR0 status capture (NR/RO/PL error, faulting page, IC bit) and PDR A/W
      flags. A red-stack runaway guard prevents an infinite nested-abort loop
      when the kernel stack page is not resident. **[A]** `mmuabort` probe
      (write to a read-only page; handler reads MMR0=020205) byte-identical to
      SimH. MMR1 (auto-inc/dec recovery) and MMR2 remain a small tail.
- *Verify:* fault/abort + relocation probes **[A]**; MMR status **[A]**.

## P4 — Cache + timing  ← the cycle-accuracy core
- [~] **P4a** Instruction timing model (src/core/timing): SRC/DST address time by
      mode + Execute/Fetch time per instruction class, in ns, from the PDP-11/70
      Handbook App. C tables; a per-instruction time accumulator (cpu->time_ns).
      **[T]** timing_suite checks computed times against the tables (8 cases).
- [x] **P4b** Timing for branches (taken .60 / not .30), SOB, JMP & JSR (by DST
      mode), MFPI/MTPI/MFPD/MTPD, MUL (3.30), XOR, and the note-(J) R7 penalty —
      all from Handbook App. C (PDF pp. 272-273). **[T]** timing_suite (15 cases)
      + branch-timing CPU test. Tail: DIV/ASH/ASHC (operand/shift-count
      dependent; the handbook gives only a range for DIV) and FP11 (P5).
- [x] **P4c** 11/70 cache (src/core/cache): two-way set-associative, 256 sets x
      2-word blocks (1K words), write-through, per KB11-C sec. 2.2. Every RAM read
      (fetch/operand/pointer/vector) checks the cache; each miss adds 1.02 us
      (total time = cpu->time_ns + cache.misses*1020). Hardware random replacement
      is modeled by a deterministic round-robin victim (documented). **[T]**
      cache_suite (4 cases); architectural goldens unchanged (cache is timing-only).
- [ ] **P4d → folded into P6** Reference `tick()` core for emergent DMA/bus
      contention, the Unibus map, and RH70 Massbus. These are device-coupled: the
      handbook times exclude NRP/BR serving (NOTE 2), so contention only exists
      once a device does DMA. They are therefore built with the devices in P6,
      where they can be exercised and verified — a dependency-driven re-sequence,
      not skipped work. Instruction + cache timing (P4a-c) gives cycle-accurate
      non-DMA timing now.

**P4 (CPU cycle timing) COMPLETE** for the non-DMA case: instruction timing
(SRC/DST/EF from the Handbook) + the KB11-C cache, every number cited in FINDINGS.
- *Verify:* timing checked against KB11-C / Handbook tables **[T]**; every number
  cited in FINDINGS.md.

## P5 — FP11-C floating point
- [x] **P5a** FP state (6 accumulators, FPS, FEC/FEA) + control instructions:
      CFCC, SETF/SETI/SETD/SETL, LDFPS, STFPS, STST. **[A]** `fp` probe
      byte-identical to SimH (which has the FP11 on the 11/70).
- [x] **P5b** Load/store LDF/LDD, STF/STD, and CLRf/TSTf/ABSf/NEGf, with the
      64-bit FAC packing (word0<<48..word3), FP-sized autoincrement addressing,
      and the FP condition codes (N=sign, Z=exp 0). **[A]** `fpls` probe (load
      1.0, store, negate, store -1.0) byte-identical to SimH.
- [x] **P5c** Arithmetic and conversions, via a bit-exact port of SimH's
      pdp11_fp.c (src/core/fp): ADDF/SUBF, MULF, DIVF, MODF, CMPF, the LDC/STC
      float↔float and int↔float conversions (LDCff'/STCff', LDCif/STCfi), and
      LDEXP/STEXP — all sharing the guard-bit/round_and_pack machinery. The
      exception model mirrors fpnotrap: over/underflow/conversion errors are
      silently suppressed unless their FPS enable is set, otherwise FEC/FEA are
      posted and the FPE trap (vector 0244) is taken unless FPS_ID; divide-by-zero
      always traps. STEXP/STCfi write the CPU condition codes as well as the FPS.
      **[A]** `fparith` (MUL/DIV/CMP/MOD + all conversions, single), `fpdbl`
      (double-precision 56-bit multiply/divide paths), and `fpe` (divide-by-zero
      → FPE trap, FEC/FEA verified) probes byte-identical to SimH; unit tests.
- *Verify:* FP arithmetic probes **[A]**; FP timing **[T]**.
  - *Documented tail:* the undefined-variable trap (FEC_UNDFV / FPS_IUV) on a
    "dirty zero" operand read is not yet posted; with IUV disabled (the default,
    and what the probes use) behaviour is identical to SimH. FP instruction
    timing (**[T]**, FP11-C manual) is folded into the P4 timing tail.

## P6 — Devices for the Unix boot
- [x] **P6a** Device-interrupt infrastructure + KW11-L line clock. The CPU now
      has a device-interrupt controller (`int_req` bitmask → BR level/vector
      table; `pdp11_set_int`/`pdp11_clr_int`), granted at an instruction boundary
      against the PSW priority and interleaved with PIR (a Unibus request wins a
      same-level tie). The KW11-L (`src/core/clk`) models the LKS status register
      at 0177546 with the 11/70 monitor bit (powers up set; writable IE, DONE set
      by the tick and cleared by writing 0), and ticks once per line-frequency
      period of emulated time (60 Hz default), requesting BR6/vector 0100 when
      enabled. WAIT is released by the clock. **[A]** `clk` register probe
      byte-identical to SimH; tick→interrupt path via unit tests (SimH's clock is
      wall-clock-calibrated, so the tick *timing* can't be golden-diffed — the
      60 Hz rate cites the KW11-L manual, a **[T]** paper-oracle number).
- [x] **P6b** DL11 console SLU (`src/core/console`): the receiver (RCSR 0177560 /
      RBUF 0177562, vector 060) and transmitter (XCSR 0177564 / XBUF 0177566,
      vector 064) register pairs, both BR4. Reading RBUF clears the receiver DONE;
      writing XBUF emits the character to the frontend sink and models a
      character-time transmit-busy window before DONE returns. Received input is
      injected via `pdp11_console_input`. **[A]** `dl11` register probe
      byte-identical to SimH (CSR IMP/RW masks, reset states); input/output and
      the receiver/transmitter interrupts via unit tests (the transmit-busy timing
      is wall-clock-calibrated in SimH, so it is not golden-diffed).
- [ ] **P6c** RK11/RK05; **P6d** RP04/06 via RH70; **P6e** TM11/TU tape.
- *Verify:* device-register probes **[A]**; XXDP/MAINDEC diagnostics pass **[C]**.
  - *Documented tail (interrupt model):* our interrupt grant dispatches and ends
    the step, re-checking interrupts before the handler's first instruction runs.
    With a normal handler (whose vector PSW raises priority to the device level or
    above) this is indistinguishable; only a pathological handler at priority 0
    with the source still asserted storms where SimH would execute one handler
    instruction first. Frontend TTY capture (wiring the console sink to stdout for
    a boot-stream diff) lands with P7.

## P7 — Content boot (thermometer, not a goal)
- [ ] Boot XXDP diagnostics, then Unix V6/V7 to a shell.
- *Verify:* console TTY stream diffed vs SimH booting the same image **[A]/[C]**.

## P8 — Interactive SDL3 frontend
- [ ] VT terminal + console/switch-register panel; `--frames` bounded smoke test.

## P9 — Verified fast mode
- [ ] Exact-skip scheduler (`next_event()`/`skip(n)`), proven bit-identical vs the
      full probe+golden suite and long boot-state hashes before it ships.

## P10 — Broaden to the full model range
- [ ] Subset CPU options / MMU / bus off the 11/70 superset for each model:
      11/20, 11/03, 11/04, 11/05, 11/23(+), 11/24, 11/34, 11/40, 11/44, 11/53,
      11/60, 11/73(B), 11/83, 11/84, 11/93, 11/94.
- *Verify:* per-model probes **[A]**; model-specific timing **[T]** where documented.
