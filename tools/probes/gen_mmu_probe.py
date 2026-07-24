#!/usr/bin/env python3
"""MMU relocation probe (P3a).

Sets up kernel I-space PARs so virtual page 1 (VA 020000) and page 2 (VA 040000)
both relocate to physical 0100000, enables 22-bit management, writes a value
through page 1, and reads it back through page 2 into R0. If relocation works,
R0 holds the value regardless of which virtual page addressed it.

  KI PAR[1] = 01000   ; VA 020000 -> PA 0100000
  KI PAR[2] = 01000   ; VA 040000 -> PA 0100000
  MMR3 = 020          ; 22-bit enable
  MMR0 = 1            ; management enable
  MOV #123456, @#020000
  MOV @#040000, R0    ; R0 <- PA 0100000 == 123456
  HALT

Diffed against SimH this validates the PAR-based translation. (Code runs in
page 0, PAR[0]=0 identity.)

Usage:  gen_mmu_probe.py > tests/images/mmu.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    # Kernel I PDR[0..2] = read/write, full length (077406) so SimH's access
    # checks pass; our P3a ignores the PDR (access control is P3c).
    a.two("MOV", (7, 2, 0o077406), (7, 3, 0o172300))  # KI PDR[0] (code page)
    a.two("MOV", (7, 2, 0o077406), (7, 3, 0o172302))  # KI PDR[1]
    a.two("MOV", (7, 2, 0o077406), (7, 3, 0o172304))  # KI PDR[2]
    a.two("MOV", (7, 2, 0o1000), (7, 3, 0o172342))    # KI PAR[1]
    a.two("MOV", (7, 2, 0o1000), (7, 3, 0o172344))    # KI PAR[2]
    a.two("MOV", (7, 2, 0o020), (7, 3, 0o172516))     # MMR3 = 22-bit enable
    a.two("MOV", (7, 2, 0o1), (7, 3, 0o177572))       # MMR0 = enable
    a.two("MOV", (7, 2, 0o123456), (7, 3, 0o020000))  # write via page 1
    a.two("MOV", (7, 3, 0o040000), (0, 0))            # read via page 2 into R0
    a.halt()
    a.emit(run=500)
    return 0


if __name__ == "__main__":
    sys.exit(main())
