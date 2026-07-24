# FINDINGS — oracle campaigns

One row per probe/verification campaign: ours vs the oracle (SimH for
architectural state; DEC docs for timing), status, and the story. Nothing is
"fixed" on reasoning alone (emulator-setup-guide.md §8).

Status values: `open` · `confirmed` (matches oracle) · `fixed` · `divergence`
(deliberate, hardware-truer than the oracle — cite evidence) · `deferred`.

| Campaign | Ours | Oracle | Status | Notes |
|----------|------|--------|--------|-------|
| `add3` first-slice (MOV/ADD/HALT, immediate mode) | R0=3, PC=001012, PSW=0 | SimH 11/70 identical | confirmed | The whole harness loop proven end-to-end: `tools/simh-oracle/run_oracle.py` → golden `tests/goldens/add3.golden`, diffed by `regress.py` in CTest. |
| `alu` (P1a): ~90-instruction battery over the full single/double-operand set incl. byte variants, flag edges (0/-1/0100000/0077777) | R0=0 R1=2 R2=1 R5=0200 PSW=4 | SimH 11/70 identical | confirmed | Validates every condition-code rule — ADC/SBC/NEG carry, ROR/ROL/ASR/ASL (V=N^C), MOVB sign-extension, SWAB, SXT, CMP-vs-SUB operand order. `gen_alu_probe.py` → `tests/goldens/alu.golden`. |
| `flow` (P1b): SOB loop summing 5..1, untaken BNE, JSR/RTS subroutine doubling the result | R1=020 R3=040 R6=2000, stack[001776]=001030 | SimH 11/70 identical | confirmed | Validates branch conditions, SOB backward branch, and JSR/RTS stack linkage incl. the pushed return address. `gen_flow_probe.py` via `asm.py`. |
| `trap` (P2a): TRAP → handler → RTI | R0=1 R1=1 R2=0100, stack PC=001014 PSW=004 | SimH 11/70 identical | confirmed | Validates do_trap push order (PSW then PC) and RTI pop order. |
| `trace` (P2a): T-bit set via RTI, one traced instruction | stack PC=001016 PSW=020 | **initially diverged** (we pushed 001020/024) → now identical | **fixed** | Oracle caught it: SimH (KB11-C) takes an RTI-restored T-bit trace trap *immediately*, before the next instruction; RTT defers it. We were tracing *after* the instruction. Read `pdp11_cpu.c:1076` ("RTI immed trap"); restructured to service the pending trace at the top of the loop. The documented RTI-vs-RTT difference. |

| `eis` (P2c): MUL/DIV/ASH/ASHC/XOR over 14 flag-critical cases, PSW snapshotted after each via @#177776 | results + PSW all identical | SimH 11/70 identical | confirmed | Implemented directly from SimH case 007 (MUL carry, /0 → N=0 Z=V=C=1, /overflow → V=1, ASH/ASHC shift-out & overflow). Semantics matched first try. |
| MFPS/MTPS on the 11/70 | first attempt implemented them as valid | SimH traps them (illegal) | **fixed** (removed) | The EIS probe used MFPS to snapshot the PSW; SimH 11/70 *trapped* (MFPS/MTPS are gated behind HAS_MXPS — LSI/34-class only, not the 11/70). Removed them; read the PSW via its memory-mapped address 0177776 instead. Re-enable per-model at P10. |

| `faults` (P2b): odd-address word write (→vec 4) and MFPS illegal instruction (→vec 10), each handler RTIs back | R0=1 R2=1 R3=1, both handlers ran and resumed | SimH 11/70 identical | confirmed | Validates the setjmp/longjmp abort path, the odd-address and reserved-instruction vectors, and RTI resumption after a mid-instruction fault. |

| `intr` (P2d): PIR7 requested via PIRQ at priority 0, handler at priority 7 clears it and RTIs | R0=1 R2=1, pushed frame PC=001016 PSW=010 | SimH 11/70 identical | confirmed | Validates PIRQ encoding (put_PIRQ), the priority-gated grant (level > PSW<7:5>), and the vector-0240 dispatch. |

| `mmu` (P3a): kernel PARs map VA pages 1 & 2 to PA 0100000; write via one, read via the other | R0=0123456 (relocation worked) | SimH 11/70 identical | confirmed | Oracle caught a gap first: with PDRs=0 SimH aborts every access (ACF=0 non-resident) and halted at the MMU-enable boundary; our P3a ignores the PDR (access control is P3c). Setting PDRs read/write (077406) let SimH run; the PAR-based translation then matched exactly. |

| `mmuabort` (P3c): write to a read-only kernel page → MMU abort (0250); handler reads MMR0 | R2=1 R3=0, MMR0=020205 | first attempt MMR0=020005 → now identical | **fixed** | Two findings: (1) a red-stack runaway — with PDRs unset the fetch aborts and the abort's own stack push aborts again, an infinite nested abort; added a depth guard that halts. (2) MMR0 bit 7 (MMR0_IC, "instruction complete") is set on trap dispatch (SimH pdp11_cpu.c:872); we now set it in do_trap, matching 020205. |

| `regset` (P3b): write R0 in set 0, switch to set 1 via PSW<11>, write R0, switch back | mem[4000]=2222 mem[4002]=1111 | SimH 11/70 identical | confirmed | Validates the R0-R5 register-set banking through put_psw (routed from RTI, trap dispatch, and PSW writes). |

| `mfp` (P3b): kernel (current) reads user (previous) VA 020000 via MFPI, user PAR maps it to PA 0100000 | R2=0123456 | SimH 11/70 identical | confirmed | Validates mode-generalized relocation (cpu_read_word_mode), MFPI's previous-mode read + push, and user-mode PAR mapping. |

| `idspace` (P3b): kernel D-space enabled, D page 2 -> PA 0100000 (0122222); a `(R1)` data read of VA 040000 | R0=0122222 | SimH 11/70 identical | confirmed | Validates data refs use D-space while fetches use I-space. Oracle caught a *probe* bug first: an initial value 0222222 exceeds 16 bits, which SimH's deposit rejects (stayed 0) while our loader masked it — a false divergence until the probe used a 16-bit value. |

| `fp` (P5a): FP control — LDFPS/STFPS move the FP status, SET* mode bits, CFCC copies FP CC to the PSW | mem[4000]=200 mem[4002]=17 PSW=17 | SimH 11/70 identical | confirmed | Validates the FP11 control group; the FP11 is standard on the 11/70. |

| `fpls` (P5b): LDF 1.0 into AC0, STF back, NEGF, STF -1.0 | mem[4010]=040200/0, mem[4014]=0140200/0 | SimH 11/70 identical | confirmed | Validates the 64-bit FAC packing (word0<<48..word3), LDF/STF, and NEGF sign flip. |

| `fpadd` (P5c): ADDF 1.0+1.0=2.0, SUBF 2.0-1.0=1.0 (single) | mem[4020]=040400, mem[4024]=040200 | SimH 11/70 identical | confirmed | src/core/fp is a faithful port of SimH addfp11/round_and_pack (guard bits, rounding); results match bit-for-bit. |

| `fparith` (P5c): MULF 2·3=6, DIVF 6/2=3, CMPF, MODF 3·0.5→int 1.0+frac 0.5, LDCIF/STCFI int round-trip, STEXP, LDEXP 1.0→4.0 (single) | mem[4100]=040700, [4104]=040500, [4110]=10, [4114]=040200, [4120]=040000, [4124]=5, [4134]=040600, PSW=4 | SimH 11/70 identical | confirmed | Ports mulfp11/frac_mulfp11/divfp11/modfp11/roundfp11 and the LDC/STC/LDEXP/STEXP conversions. Oracle discipline caught two of my own bugs: hand-encoded float octals were wrong (harmless — golden is self-consistent — but fixed for clarity), and STEXP/STCfi write the **CPU** PSW condition codes too (final PSW differed until replicated). |

| `fpdbl` (P5c): DIVD 2.0/3.0 (fills the 56-bit mantissa), MULD ·3.0 round-trip, CMPD (double) | mem[5100..5116] = SimH's exact 4-word 0.666… bits | SimH 11/70 identical | confirmed | Exercises the double-precision paths (frac_mulfp11 24×56 loop, divfp11 long quotient loop) the single-precision `fparith` probe does not reach. |

| `fpe` (P5c): DIVF by 0.0 → FPE trap; handler STST saves FEC/FEA; R0 stays 0 | mem[4100]=4 (FEC_DZRO), [4102]=01006 (FEA=DIVF addr), R0=0, R7=02006 | SimH 11/70 identical | confirmed | Validates the exception model: fpnotrap semantics (over/underflow suppressed unless enabled), FEC/FEA posting, FEA = opcode address (SimH backup_PC-2), and the FPE vector 0244 dispatch. Documented tail: the FEC_UNDFV/IUV "dirty zero" NOP is not yet posted (identical with IUV disabled). |

| `clk` (P6a): KW11-L LKS register at 0177546 — read initial, set IE, try IE\|DONE, clear | R0=0200 (DONE set at power-up), R1=0100, R2=0100 (DONE not writable), R3=0 | SimH 11/70 identical | confirmed | The 11/70 has the monitor bit (SimH HAS_LTCM). Oracle caught our reset: SimH's clk_reset powers up with DONE set (`clk_csr = CSR_DONE`), which we now match. Tick→interrupt timing is *not* golden-diffed — SimH's clock is wall-clock-calibrated; verified by unit tests instead, with the 60 Hz rate a **[T]** paper-oracle number (KW11-L manual). |

| `dl11` (P6b): DL11 console CSRs — read RCSR/XCSR initial, set/clear IE, read back (priority 7 masks interrupts) | R0=0 (rcv idle), R1=0200 (xmt ready), R2=0100, R3=0300, R4=0 | SimH 11/70 identical | confirmed | Receiver powers up idle, transmitter ready (SimH tti/tto_reset); only IE writable; reads expose DONE\|IE. The probe raises PSW to priority 7 first — the diagnosis being the finding: enabling the transmitter IE while DONE is set raises an immediate BR4 interrupt through the unset vector 064, which SimH survives (it runs the HALT at 0 before re-checking) but our grant-then-recheck model storms on. A pathological-vector edge case (documented tail); masked here so the probe tests just the CSR semantics. |

| `rk11` (P6c): RK11 registers — read RKCS initial, write/read-back RKWC, RKBA, RKDA | R0=0200 (RKCS DONE), R1=012345 (RKWC), R2=04000 (RKBA), R3=01234 (RKDA) | SimH 11/70 identical | confirmed | RK enabled by default on the SimH pdp11. Deterministic R/W registers only: RKDS returns a random sector count in SimH (rand()%12) so it is not probed, and the DMA transfer needs an attached image (unit-tested instead). Port of pdp11_rk.c; the read/write sector transfer and BR5/vector-0220 completion interrupt are covered by cpu_suite unit tests. |

| `rp11` (P6d): RH70/RP04 registers — RPCS1 initial, write/read-back RPWC, RPBA, RPDA, RPDC | R0=04200 (CS1 DONE+DVA), R1=012345, R2=04000, R3=012045 (RPDA & 037477), R4=01234 (RPDC & 01777) | SimH 11/70 identical | confirmed | Register access goes through SimH's mba_mapofs (I/O offset → RH-internal or drive register); RH70 BAE/CS3 bolted on. Oracle caught the CS1 drive-available bit (04000) we initially omitted, and the RPDA/RPDC MBZ masks (0140300 / 0176000). DMA read/write and the BR5/vector-0254 completion interrupt are unit-tested. |

| `tm11` (P6e): TM11 tape registers — MTS initial, MTC initial, write/read-back MTBRC, MTCMA, MTD | R0=01 (MTS unit-ready), R1=0200 (MTC DONE), R2=012345 (MTBRC), R3=04000 (MTCMA), R4=0123 (MTD) | SimH 11/70 identical | confirmed | TM enabled by default on the SimH pdp11. MTS reads STA_TUR (unit ready) with no tape attached. Port of pdp11_tm.c; record read/write (SimH .tap format), the file-mark→EOF status, and the BR5/vector-0224 completion interrupt are unit-tested. |

| `fuzz` (P7c): differential CPU fuzzer — 120 random register-mode instructions (two-op/one-op/EIS) over edge + random operands, PSW+dst stored per case | seed 1 image byte-identical to SimH | SimH 11/70 identical | confirmed | `tools/probes/gen_fuzz_probe.py <seed>`; a sweep of seeds vs SimH caught a real bug: **SBC set V unconditionally when dst==0100000**, but SimH sets V only with a carry-in (`V = C && result==077777`). Fixed in cpu.c; seed 1 checked in as a permanent golden. (Also flushed out a fuzzer self-collision — result table overlapping code past ~48 tests — now placed at 020000.) The register-mode fuzzer is clean across seeds 1-60; the V6 boot still hangs, so the remaining bug(s) are in a memory addressing mode the fuzzer doesn't yet cover. |

| `memfuzz` (P7c): differential fuzzer — memory addressing modes ((R3), (R3)+, -(R3), X(R3)) for two-op/one-op, capturing the touched window + pointer reg + PSW | seed 1 image byte-identical to SimH | SimH 11/70 identical | confirmed | Clean across seeds 1-15: our effective-address, autoincrement/decrement side-effect, and read/write paths match SimH. Seed 1 checked in as a golden. Combined with the register `fuzz`, the CPU is exercised broadly; the V6 boot bug is not a plain instruction/addressing error. |

**P7c trace comparison (2026-07-25):** anchored our and SimH's kernel PC traces on
the first `csv` (022272) after "unix" and diffed forward — the first **64000
kernel instructions are byte-identical** (via SimH `break 22272; step; show cpu
history`). So the CPU is very accurate; the boot divergence is deeper than
SimH's 65536-entry history reaches, and (since the fuzzers are clean) most likely
tied to device I/O or interrupt interaction once the kernel starts real work.
Next: a spin-tolerant trace-aligner (or a state-transfer oracle) to search past
the first kernel disk I/O.

**P7c bug #2 — SPL not implemented (2026-07-25).** Localised the V6 boot
divergence with a two-stage device+trace method: (1) logged every RK disk read in
both emulators — the first **117 reads are identical**, then ours hangs where SimH
continues, bracketing the divergence to the `main()`/`binit` gap (SimH read #117 →
#118 spans instr 200089→943306 with *no* disk I/O, so instruction alignment holds
there); (2) binary-searched that gap comparing kernel PC/SP/PSW traces anchored on
the first `csv` (022272). The first divergence: at `SPL 6` (0000236) our PSW
priority stayed 0 while SimH's went to 6 — **we never decoded SPL (000230-000237)
at all**, silently no-op'ing a core 11/70 instruction. Implemented it (kernel-mode
only sets PSW<7:5>); unit-tested. This advances the divergence (a second, cascaded
Z-flag difference in the idle/swtch loop remains — likely interrupt/clock-timing
related — the next target).

**P7c bug #3 — PSW-write clobbers its own condition codes (2026-07-25).** After
SPL, the next divergence was at the very first idle/`swtch` iteration:
`MOV (SP)+, @#177776` restores a saved PSW, but our MOV then recomputed Z from the
value (nonzero → Z=0), clobbering the written Z bit. On the 11/70 (and SimH) an
explicit PSW store is the last operation and its condition codes win. Fixed with a
per-instruction `cc_frozen` flag: writing the PSW via 0177776 freezes the codes so
the instruction's own N/Z/V/C update is skipped. (This is why the register fuzzer
missed it — it always overwrites the PSW with the *next* instruction's codes.)
With the fix the first 65000 kernel instructions now match SimH in PC+SP+PSW; the
boot divergence advanced again.

**P7c bug #4 — non-existent memory does not abort (2026-07-25).** After the PSW
fix the boot hung in a loop (022046-052, an `MTPD` clear) whose surrounding code
ran ~15000 times with a single register (R4) climbing without bound and every
other register constant — the signature of a *probe*, not a busy-wait. It is V6
`main()`'s memory-sizing loop: it maps a page to successive 22-bit frames and
reads each, sizing core by the address at which the reference **aborts through
vector 4** (non-existent memory). SimH does this in `ReadW`/`WriteW`: after
relocation, `pa >= MEMSIZE && pa < IOPAGEBASE` sets CPUERR and `ABORT (TRAP_NXM)`
(`pdp11_cpu.c`); `256K` = 262144 = 2^18, `IOPAGEBASE` = 017760000. Our memory was
a flat 4 MB array that answered every address, so the probe never terminated.
Added a create-time `mem_top` (256 KB, matching the oracle's `set cpu 256k`) and
an NXM check on every relocated physical reference that is not the I/O page.
Unit-tested (`test_a_word_access_to_nonexistent_memory_traps_through_vector_4`).
The 22-bit path is what matters: in 18-bit mode every address is RAM or I/O, so
(as in SimH) only 22-bit relocation reaches the NXM region the probe needs.

**P7c bug #5 — device interrupt not acknowledged, re-storms (2026-07-25).** With
core sized, the boot did real filesystem I/O then hung in a priority-5 idle/`swtch`
loop. Interrupt logging showed vector 0220 (RK) granted **89533 times**: the RK
read completed and asserted BR5, but granting it never dropped the request. On the
Unibus the BG/acknowledge cycle clears the request latch, so a level that stays
asserted (DONE & IE both set until the ISR clears DONE) is taken *once*. SimH does
this in `get_vector` (`pdp11_io.c`): `int_req[i] &= ~(1u << j)` on the acknowledged
device. Our grant path (`do_trap` on `int_tab[dev].vec`) omitted the clear, so the
still-DONE RK re-interrupted every instruction and the woken process never ran.
Fixed by clearing the granted device's `int_req` bit on acknowledge (PIR is
unaffected — it persists until software writes PIRQ). Unit-tested
(`test_a_granted_device_interrupt_is_acknowledged_and_not_restormed`). The boot now
sizes memory, mounts root, reads inodes and `/etc/init`, and writes the superblock
— then hits the **next** divergence: a wild transfer to PC 0 that settles into a
`br .` spin at 000426 (kernel jumped to zero after a user-space copy loop). That
deeper crash is the next P7c target.

## Timing (DEC paper oracle)
| Campaign | Ours | DEC source | Status | Notes |
|----------|------|-----------|--------|-------|
| P4c cache: two-way set-assoc, 256 sets x 2-word blocks, 1K words | cold read misses, same block hits, 3rd tag evicts | KB11-C Proc. Manual sec. 2.2 | confirmed vs manual | Write-through, and **hardware uses random replacement** (non-deterministic); we use a deterministic round-robin victim as a documented stand-in (hit/miss is insensitive to policy for localized access). Miss adds 1.02us. |
| P4b branch/jump/EIS timing: branches .60/.30, JMP/JSR by mode, MTPI/MTPD, MFPI/MFPD +1.50, MUL 3.30, XOR, note (J) | JMP (R0)=900ns; MUL=3300ns; BEQ taken=600/not=300 | Handbook App. C.1.7 / C-4 / C-5 (PDF pp. 272-273) | confirmed vs tables | Branch/SOB timing computed at execution (flags unchanged by a branch); DIV/ASH/ASHC operand-dependent times are a tail. |
| P4a instruction timing: SRC/DST address time + Execute/Fetch time | ADD R/R=300ns; CLR (R0)=1500ns; MOV to PC=600ns; etc. | PDP-11/70 Handbook 1977-78, App. C.1.5-C.1.7 (PDF pp. 269-271) | confirmed vs tables | Times in ns (handbook us x1000). **Manual self-inconsistency found:** the ADD mode-6/6 *worked example* (C.1.3) totals 2.55 us (EF 1.35), but the *tables* give SRC .60 + DST .60 + EF 1.20 = 2.40 us. We implement the tables and treat the example as an errata. |
