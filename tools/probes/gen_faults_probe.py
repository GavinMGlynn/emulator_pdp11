#!/usr/bin/env python3
"""Fault probe: odd-address bus trap (vec 4) and reserved-instruction trap (10).

  MOV R1, @#1001   ; word write to an odd address -> trap 4  -> handler4 (INC R2; RTI)
  .word 106700     ; MFPS: illegal on the 11/70          -> trap 10 -> handler10 (INC R3; RTI)
  INC R0           ; runs after both handlers return      -> R0 = 1
  HALT

Each handler bumps a counter and RTIs back to the instruction after the fault.
Expect R0=1, R2=1, R3=1. Diffed against SimH this validates the abort/longjmp
path, the odd-address and reserved-instruction vectors, and RTI resumption.

Usage:  gen_faults_probe.py > tests/images/faults.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.mov_imm(6, 0o2000)
    a.one("CLR", (0, 0))
    a.one("CLR", (2, 0))
    a.one("CLR", (3, 0))
    a.mov_imm(1, 0)
    a.two("MOV", (1, 0), (7, 3, 0o1001))  # MOV R1, @#1001 -> odd address trap
    a.word(0o106700)                      # MFPS R0 -> reserved-instruction trap
    a.one("INC", (0, 0))
    a.halt()
    a.label("handler4")
    a.one("INC", (2, 0))
    a.word(0o000002)                      # RTI
    a.label("handler10")
    a.one("INC", (3, 0))
    a.word(0o000002)                      # RTI

    a.emit(run=500, dumps=[(0o1770, 4)])
    print(f"w 4 {a.labels['handler4']:o}")
    print("w 6 0")
    print(f"w 10 {a.labels['handler10']:o}")
    print("w 12 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
