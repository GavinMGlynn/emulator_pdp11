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
      Remaining tail: full I/D-space separation (distinct I/D registers when MMR3
      enables D-space; V6/V7-nonsep don't use it) and kernel-forced vector reads
      for user-mode traps.
- [x] **P3c** Access control (PDR ACF) + page-length aborts → vector 0250, with
      MMR0 status capture (NR/RO/PL error, faulting page, IC bit) and PDR A/W
      flags. A red-stack runaway guard prevents an infinite nested-abort loop
      when the kernel stack page is not resident. **[A]** `mmuabort` probe
      (write to a read-only page; handler reads MMR0=020205) byte-identical to
      SimH. MMR1 (auto-inc/dec recovery) and MMR2 remain a small tail.
- *Verify:* fault/abort + relocation probes **[A]**; MMR status **[A]**.

## P4 — Cache + Unibus/Massbus timing  ← the cycle-accuracy core
- [ ] 11/70 cache (hit/miss), the primary driver of instruction cycle counts.
- [ ] Unibus map + NPR/BR arbitration; RH70 Massbus fast path.
- [ ] Reference `tick()` core: one tick per processor clock, contention emergent.
- *Verify:* cycle goldens vs KB11-C cache/bus formulas **[T]**; every number
  cited in FINDINGS.md.

## P5 — FP11-C floating point
- [ ] FP accumulators, FPS/FEC/FEA, single & double, rounding & exceptions.
- *Verify:* FP arithmetic probes **[A]**; FP timing **[T]**.

## P6 — Devices for the Unix boot
- [ ] KW11-L line clock; DL11 console SLU; RK11/RK05; RP04/06 via RH70;
      TM11/TU tape for install media.
- *Verify:* device-register probes **[A]**; XXDP/MAINDEC diagnostics pass **[C]**.

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
