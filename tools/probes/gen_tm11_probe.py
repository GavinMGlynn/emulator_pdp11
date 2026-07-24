#!/usr/bin/env python3
"""TM11 tape register probe (P6).

Exercises the deterministic TM11 registers (a tape operation needs an attached
image and is covered by unit tests; MTS drive-status bits depend on attach
state). Results land in R0-R4, which the oracle dumps.

  MOV @#172520, R0     ; MTS initial (unit ready)
  MOV @#172522, R1     ; MTC initial -> 200 (DONE)
  MOV #12345, @#172524  ; MTBRC
  MOV @#172524, R2     ; -> 012345
  MOV #4000, @#172526   ; MTCMA
  MOV @#172526, R3     ; -> 04000
  MOV #123, @#172530    ; MTD
  MOV @#172530, R4     ; -> 0123
  HALT

Usage:  gen_tm11_probe.py > tests/images/tm11.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402

MTS = 0o172520
MTC = 0o172522
MTBRC = 0o172524
MTCMA = 0o172526
MTD = 0o172530


def main():
    a = Asm(org=0o1000)
    a.two("MOV", (7, 3, MTS), (0, 0))           # MTS initial
    a.two("MOV", (7, 3, MTC), (1, 0))           # MTC initial
    a.two("MOV", (7, 2, 0o12345), (7, 3, MTBRC))  # MTBRC
    a.two("MOV", (7, 3, MTBRC), (2, 0))
    a.two("MOV", (7, 2, 0o4000), (7, 3, MTCMA))   # MTCMA
    a.two("MOV", (7, 3, MTCMA), (3, 0))
    a.two("MOV", (7, 2, 0o123), (7, 3, MTD))      # MTD
    a.two("MOV", (7, 3, MTD), (4, 0))
    a.halt()
    a.emit(run=50, dumps=[])
    return 0


if __name__ == "__main__":
    sys.exit(main())
