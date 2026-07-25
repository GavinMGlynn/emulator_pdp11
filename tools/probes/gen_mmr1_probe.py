#!/usr/bin/env python3
"""MMR1 / MMR2 memory-management status probe (P3d).

Exercises the two KT11 status registers used for page-fault recovery, in the
way real software depends on them: an instruction that modifies a register by
auto-increment and *then* faults on its destination. The MMU freezes with the
source register-delta captured in MMR1 and the faulting instruction's address
in MMR2.

  SP in page 0; PDR[0]=rw, PDR[2]=read-only, PDR[7]=rw + PAR[7]->I/O
  MMR3=22-bit; R1=02000 (a rw source pointer); MMR0=enable
  MOV (R1)+, @#040000  ; src (R1)+ records MMR1 = (2<<3)|1 = 021, then the write
                       ; to read-only page 2 aborts -> MMR0/MMR1/MMR2 freeze
  INC R5               ; skipped
  HALT
  handler(250): R2 = MMR1; R3 = MMR2; R0 = MMR0; HALT

Expect R1=02002 (incremented once), R2=021 (the frozen delta log), R3 = the VA
of the MOV (MMR2), and R0 = MMR0 with the read-only error + faulting page. The
register dump diffed against SimH validates the MMR1 encoding, the MMR2 saved
PC, and the freeze-on-abort semantics.

Usage:  gen_mmr1_probe.py > tests/images/mmr1.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.mov_imm(6, 0o17000)                              # SP in page 0
    a.two("MOV", (7, 2, 0o077406), (7, 3, 0o172300))  # KI PDR[0] rw
    a.two("MOV", (7, 2, 0o077402), (7, 3, 0o172304))  # KI PDR[2] read-only
    a.two("MOV", (7, 2, 0o077406), (7, 3, 0o172316))  # KI PDR[7] rw (I/O)
    a.two("MOV", (7, 2, 0o177600), (7, 3, 0o172356))  # KI PAR[7] -> I/O page
    a.two("MOV", (7, 2, 0o020), (7, 3, 0o172516))     # MMR3 = 22-bit
    a.mov_imm(1, 0o2000)                              # R1 = a rw source pointer
    a.two("MOV", (7, 2, 0o1), (7, 3, 0o177572))       # MMR0 = enable
    a.two("MOV", (1, 2), (7, 3, 0o40000))            # MOV (R1)+, @#040000 -> abort
    a.one("INC", (5, 0))                             # must be skipped
    a.halt()
    a.label("handler")
    a.two("MOV", (7, 3, 0o177574), (2, 0))           # R2 = MMR1
    a.two("MOV", (7, 3, 0o177576), (3, 0))           # R3 = MMR2
    a.two("MOV", (7, 3, 0o177572), (0, 0))           # R0 = MMR0
    a.halt()
    a.emit(run=500)
    print(f"w 250 {a.labels['handler']:o}")
    print("w 252 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
