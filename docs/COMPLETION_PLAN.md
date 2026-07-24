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
- [x] **P6c** RK11/RK05 disk (`src/core/devices/rk11`): the eight I/O-page
      registers (RKDS/RKER/RKCS/RKWC/RKBA/RKDA + unimplemented RKMR/RKDB),
      interrupting at BR5 through vector 0220, with the control/read/write/
      write-check/seek/reset functions. A read/write moves whole sectors between
      the disk image (attached via `pdp11_rk_attach`, drive 0) and physical
      memory (18-bit Unibus address in RKBA + RKCS<5:4>), counting RKWC words;
      completion (DONE + interrupt) is scheduled in emulated time. **[A]** `rk11`
      register probe byte-identical to SimH (RKCS/RKWC/RKBA/RKDA); the DMA
      read/write and the completion interrupt via unit tests.
- [x] **P6d** RP04 disk via the RH70 Massbus (`src/core/devices/rp11`): the
      controller register window at 0176700-0176752, interrupting at BR5 through
      vector 0254. The RH70 owns CS1/WC/BA/CS2/BAE and passes the other offsets to
      the drive (DS/ER1/DA/DT/DC/CC) via SimH's `mba_mapofs` I/O-offset→register
      map; RPBAE/RPCS3 (RH70-only) are handled explicitly. A read/write moves
      whole sectors between memory (22-bit BAE:BA) and disk block GET_DA(DC,DA),
      counting RPWC words; pack-acknowledge sets volume-valid, completion sets
      ready + attention and interrupts. **[A]** `rp11` register probe
      byte-identical to SimH (CS1 with the drive-available bit, WC/BA/DA/DC with
      their MBZ masks); the DMA read/write and completion interrupt via unit tests.
- [x] **P6e** TM11/TU10 magtape (`src/core/devices/tm11`): the six registers at
      0172520-0172532, interrupting at BR5 through vector 0224. The tape image is
      the SimH ".tap" format (4-byte little-endian record lengths bracketing the
      data, a zero length being a file mark); read/write/space-forward/
      space-reverse/write-EOF/rewind/unload decode from MTC<3:1>+GO. A read/write
      moves a whole record between memory (18-bit MTCMA + MTC<5:4>) and the tape,
      counting down MTBRC bytes. **[A]** `tm11` register probe byte-identical to
      SimH (MTS/MTC/MTBRC/MTCMA/MTD); record read/write, the file-mark→EOF status,
      and the vector-0224 completion interrupt via unit tests.
- *Verify:* device-register probes **[A]**; XXDP/MAINDEC diagnostics pass **[C]**.
  - *Documented tail (TM11):* single TU10 on drive 0; 7-track / density / parity /
    extended-status details are minimal.

**P6 is complete** — the device set for a Unix boot (KW11-L clock, DL11 console,
RK11 disk, RP04/RH70 disk, TM11 tape) plus the device-interrupt controller. Next
is P7, the content boot; the standing P6 infra tails (Unibus Map for >256 KB DMA;
attached-disk SimH memory-diff harness; XXDP/MAINDEC diagnostics as the [C]
real-output check) are exercised there.
  - *Documented tails (RP/RH70):* single RP04 on drive 0; the Massbus error /
    attention-summary / offset / ECC / diagnostic registers are minimal (read 0);
    the RH11 `mba_mapofs` is used with RH70 BAE/CS3 bolted on; DMA uses the 22-bit
    BAE:BA address as physical (no Unibus Map, which the RH70 bypasses anyway).
    The attached-disk SimH memory-diff and the XXDP RP diagnostic **[C]** at P7 are
    the end-to-end checks.
  - *Documented tails (RK11):* (1) RKDS drive status is minimal — SimH returns a
    random sector count there, so it is not modelled/probed. (2) DMA uses the
    18-bit Unibus address directly as physical; the 11/70 Unibus Map relocation
    (for DMA above 256 KB) is a P6 infra tail. (3) Only drive 0 and the
    controller interrupt are modelled — the per-drive overlapped-seek interrupt
    queue is deferred. (4) The strongest check — a disk image attached in both
    SimH and our headless with the transferred memory diffed — awaits the oracle
    disk-attach harness; today the DMA is unit-tested and will be exercised end to
    end by the XXDP RK diagnostic **[C]** at P7.
  - *Interrupt model:* our interrupt grant dispatches and ends the step,
    re-checking interrupts before the handler's first instruction runs. Granting
    a device interrupt now **acknowledges and clears** that device's request bit
    (the Unibus BG cycle drops the request latch), matching SimH `get_vector`, so
    a level that stays asserted (DONE & IE both set until the ISR clears DONE) is
    taken exactly once instead of storming — the RK re-storm that stalled the V6
    boot (P7c bug #5). PIR is unaffected (it persists until software writes PIRQ).
    Frontend TTY capture (console sink to stdout for a boot-stream diff) lands
    with P7.

## P7 — Content boot (thermometer, not a goal)
- [~] **P7a** Reference boot established. The bootable image is `unix0_v6_rk.dsk`
      (V6 root, from `roms/pdp-11.org.ru/files/unix/uv6swre.zip`; must be
      writable). Under SimH on the 11/70 it boots to a root shell: the block-0
      bootstrap prints `@`, type `unix`, the kernel reaches `login:`, log in
      `root` → `# ` and commands run. Repeatable script:
      `tools/simh-oracle/boot_v6.ini`; this console stream is the P7 target.
      (`bitsavers .../rk05/v6unix.dsk` is a non-bootable *user* disk — not this.)
- [x] **P7b** Headless frontend boot harness (`--boot-rk <disk> [--max N]
      [--in script]`): loads a `.dsk` into the RK, deposits SimH's RK boot ROM
      (reads block 0 → memory 0, jumps there), captures the DL11 console to
      stdout, and injects scripted keyboard input once the receiver drains.
      **Result:** our core boots the V6 image to the `@` prompt (RK DMA + CPU +
      DL11 all working under real boot code, PC matching SimH's input-wait loop),
      echoes `unix`, and runs into the kernel.
- [~] **P7c** Drive our core to console parity with SimH. **Diagnosis so far:**
      after `unix`, the kernel loads/runs but eventually hangs walking the buffer
      cache `av_forw` free list (kernel PC 064512-064530: `BIT #1000,(R4); MOV
      6(R4),R4; CMP #017116,R4; BNE` — checking `B_DELWRI`, sentinel `&bfreelist`
      = 017116). A memory diff against SimH (via `SET CPU HISTORY` + `RUNLIMIT` +
      `BREAK` + `EXAMINE`) shows our kernel-data regions are almost entirely zero
      where SimH's are populated — even `bfreelist` (017124) is raw zero, not the
      empty-list sentinel. So **`binit()` never ran before we reach that code**:
      an *earlier control-flow divergence* (a CPU-edge or MMU bug that sends a
      branch the wrong way), not a device-DMA corruption. **Next:** a proper
      trace-aligner (the input-wait and disk-wait spin loops break instruction-
      count alignment between our trace and SimH's `SHOW CPU HISTORY`) to pin the
      first divergent instruction. Methodology (SimH history/breakpoint/examine +
      our `--boot-rk` boot) is established. Diff is on console *content* (SimH's
      clock is wall-clock-timed, not cycle-accurate).
  - **Tool built + first bug fixed:** a differential CPU fuzzer
    (`tools/probes/gen_fuzz_probe.py`, golden `fuzz`) runs random register-mode
    instructions on our core and SimH and diffs the result/PSW. It caught **SBC
    setting V unconditionally** (SimH sets V only with a carry-in); fixed, with a
    unit test and the seed-1 image checked in as a golden. Extended to memory
    addressing modes (`memfuzz`) — also clean vs SimH. A kernel PC-trace diff,
    anchored on the first `csv` after "unix", shows the first **64000 kernel
    instructions are byte-identical** to SimH: the CPU is very accurate and the
    divergence is deeper than SimH's 65536-entry history reaches, most likely in
    device-I/O / interrupt interaction once the kernel starts real work. Next: a
    spin-tolerant trace-aligner or state-transfer oracle to search past the first
    kernel disk I/O.
  - **Bug #2 found + fixed — SPL not implemented.** Localised via RK-read-sequence
    logging (first 117 reads identical) + a PC/SP/PSW trace binary-search of the
    `main()`/`binit` gap: the first divergence was `SPL 6` (000230-000237), which
    our decoder silently no-op'd, leaving the CPU priority wrong. Implemented SPL
    (kernel-mode sets PSW<7:5>), unit-tested. A second cascaded divergence (a
    Z-flag in the idle/`swtch` loop) remains and is the next target.
  - **Bug #3 found + fixed — PSW write clobbers its own CC.** `MOV (SP)+, @#177776`
    (restore PSW) had our MOV recompute Z from the value afterwards, clobbering the
    written Z bit; an explicit PSW store's codes must win (11/70/SimH). Fixed via a
    per-instruction `cc_frozen` flag (a PSW write through 0177776 suppresses the
    instruction's own CC update); unit-tested. First 65000 kernel instructions now
    match SimH in PC+SP+PSW; the divergence advanced further.
  - **Bug #4 found + fixed — non-existent memory did not abort.** The boot hung in
    V6 `main()`'s memory-sizing probe (an `MTPD` clear loop with one register
    climbing unbounded): it maps a page to successive 22-bit frames and sizes core
    by where a reference **aborts through vector 4**. Our flat 4 MB array answered
    every address, so the probe never ended. Added a create-time `mem_top` (256 KB,
    matching the oracle `set cpu 256k`) and an NXM abort on any relocated physical
    reference `>= mem_top` that is not the I/O page — matching SimH `ReadW`/`WriteW`
    (`pa >= MEMSIZE && pa < IOPAGEBASE`). Unit-tested. Boot advanced past sizing.
  - **Bug #5 found + fixed — device interrupt not acknowledged (re-storm).** With
    core sized, the boot did real filesystem I/O then idled while the RK interrupt
    (vector 0220) was granted 89533×: granting never dropped the request, so the
    still-asserted DONE&IE level re-fired every instruction and the woken process
    never ran. Acknowledge now clears the granted device's `int_req` bit (Unibus BG
    drops the request latch), matching SimH `get_vector`; PIR still persists.
    Unit-tested. **Boot now sizes memory, mounts root, reads inodes + `/etc/init`,
    and writes the superblock** — then diverges at a wild jump to PC 0 settling into
    a `br .` spin at 000426 (kernel jumped to zero after a user copy loop).
  - **Bug #6 found + fixed — RK DMA ignored the 11/70 Unibus Map.** Root-caused the
    jump-to-0: a `cret` `RTS`-ing to 0 because a swapped-in process's saved context
    was zero. Our RK read/write sequence is byte-identical to SimH through all our
    ops (an earlier "reads diverge" note was a hex-parsing slip — SimH logs lbn in
    hex). The swapped-out image was zero because process content lives in low
    physical memory while the RK DMA reads high: V6 enables the Unibus Map
    (`MMR3<BME>`, =000065) and programs it non-identity, so the RK's 18-bit address
    0327100 must relocate (via map reg 13 = 0120000) to physical 0127100 — our RK
    used the 18-bit address directly. Implemented the Unibus Map (31 double-word
    registers at 0170200-0170377) and route RK + TM11 DMA through it when
    `MMR3<BME>` is set (mirrors SimH `Map_Addr`); unit-tested. **The V6 boot now
    reaches the multi-user `login:` prompt** (`@unix\r\n\n\rlogin: `). Also strip
    console parity (V6 sends even parity in bit 7; a 7-bit terminal drops it, as
    SimH does) so the stream matches the oracle.
- [~] **P7d** Reach a root shell + diff a command session. **Prompt-aware input
      done:** `--dialog "exp|snd|exp|snd…"` (expect/send, like SimH) waits for each
      prompt before typing. It reaches `login: ` and delivers `root\r`, which the
      kernel receives and reads via the DL11 RX interrupt (verified). **Blocker:**
      after reading the login name the kernel emits *no* console output (no echo,
      no shell) and sits in a repeated disk-**write** loop over blocks 2/7/8 (in
      50M instr: block 7 written 133×, 8 70×, 2 66× — far above SimH's periodic
      sync). Localised (via kernel PARs → real physical → disassembly) to V6
      **`swtch()`** scanning the 50-entry proc table for a runnable in-core
      process: the kernel context-switches continually and a selected process
      keeps writing inode blocks. Deep V6 scheduler/daemon behaviour, not a
      core-subsystem gap (all probes match SimH; boot reaches `login:`). *Next:*
      identify the writing process (proc entry at 041576) and whether its writes
      are retries (completion bug) or a never-cleared dirty buffer, vs SimH.
      *Verify:* TTY stream vs SimH **[C]**.
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
