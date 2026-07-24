#!/usr/bin/env python3
"""I/D-space separation probe (P3b).

Enables kernel D-space (MMR3<KDS>) and maps kernel D-space page 2 (VA 040000) to
physical 0100000, which holds 0222222. Kernel I-space page 2 is left unmapped.
A *data* read of VA 040000 must use the D-space mapping and return 0222222 — if
I/D weren't separated it would use the I-space register (PAR=0 -> physical 0).

  KI PDR[0]=rw (code); KD PDR[2]=rw, KD PAR[2]->PA 0100000
  MMR3 = KDS | M22E ; MMR0 = enable
  MOV @#040000, R0     ; data read via D-space -> 0222222
  HALT

Usage:  gen_idspace_probe.py > tests/images/idspace.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.two("MOV", (7, 2, 0o077406), (7, 3, 0o172300))  # KI PDR[0] rw (code)
    a.two("MOV", (7, 2, 0o077406), (7, 3, 0o172324))  # KD PDR[2] rw
    a.two("MOV", (7, 2, 0o001000), (7, 3, 0o172364))  # KD PAR[2] -> PA 0100000
    a.two("MOV", (7, 2, 0o000024), (7, 3, 0o172516))  # MMR3 = KDS | M22E
    a.mov_imm(1, 0o040000)                            # R1 = VA page 2
    a.two("MOV", (7, 2, 0o000001), (7, 3, 0o177572))  # MMR0 = enable
    a.two("MOV", (1, 1), (0, 0))                      # MOV (R1), R0  (D-space read)
    a.halt()
    a.emit(run=500)
    print("w 100000 122222")   # 16-bit value in kernel D-space page 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
