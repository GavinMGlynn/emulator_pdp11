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

## Timing (DEC paper oracle) — none yet
Timing campaigns begin at P4. Each row will cite the KB11-C manual page/table it
derives from, since SimH cannot verify cycle counts.
