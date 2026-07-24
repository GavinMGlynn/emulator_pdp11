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

## Timing (DEC paper oracle) — none yet
Timing campaigns begin at P4. Each row will cite the KB11-C manual page/table it
derives from, since SimH cannot verify cycle counts.
