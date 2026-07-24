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
- [ ] All single/double-operand instructions (CLR/COM/INC/DEC/NEG/TST/ROR/ROL/
      ASR/ASL/SWAB/ADC/SBC/SXT; MOV/CMP/BIT/BIC/BIS/ADD/SUB; byte variants).
- [ ] Branches (BR/BNE/BEQ/BPL/BMI/BCS/BCC/BVS/BVC/BGE/BLT/BGT/BLE/BHI/BLOS/…),
      JMP, JSR/RTS, SOB, MARK, condition-code ops (SEx/CLx).
- [ ] Byte-to-register sign-extension quirk; odd-address & reserved-mode handling.
- *Verify:* instruction-matrix probes **[A]**; per-instruction cycle goldens **[T]**.

## P2 — Traps, interrupts, EIS
- [ ] Trap vectors (4 bus/illegal, 10 reserved, 14 BPT, 20 IOT, 30 EMT, 34 TRAP),
      RTI/RTT, T-bit trace, stack-limit, PIRQ/PIR.
- [ ] 7-level BR interrupt priority + arbitration; RESET/WAIT semantics.
- [ ] EIS: MUL, DIV, ASH, ASHC, XOR.
- *Verify:* trap/interrupt probes **[A]**; EIS probes **[A]**; timing **[T]**.

## P3 — Memory management (KT11, 22-bit)
- [ ] PAR/PDR per mode (Kernel/Super/User), I/D space, dual register sets.
- [ ] Address translation, page faults/aborts, SR0–SR2, 22-bit physical space.
- *Verify:* fault/abort + relocation probes **[A]**; SR semantics **[A]**.

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
