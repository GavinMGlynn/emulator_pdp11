#!/usr/bin/env python3
"""RH70/RP04 register probe (P6).

Exercises the deterministic RP read/write registers (RPDS/RPDT depend on attach
state and drive type; a data transfer needs an attached image and is covered by
unit tests). Results land in R0-R4, which the oracle dumps.

  MOV @#176700, R0     ; RPCS1 initial -> 200 (DONE/ready)
  MOV #12345, @#176702  ; RPWC = 012345
  MOV @#176702, R1     ; -> 012345
  MOV #4000, @#176704   ; RPBA = 04000
  MOV @#176704, R2     ; -> 04000
  MOV #12345, @#176706  ; RPDA (masked by DA_MBZ 0140300)
  MOV @#176706, R3     ; -> 012345 & 037477
  MOV #1234, @#176734   ; RPDC (masked by DC_MBZ 0176000)
  MOV @#176734, R4     ; -> 01234 & 01777
  HALT

Usage:  gen_rp11_probe.py > tests/images/rp11.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402

RPCS1 = 0o176700
RPWC = 0o176702
RPBA = 0o176704
RPDA = 0o176706
RPDC = 0o176734


def main():
    a = Asm(org=0o1000)
    a.two("MOV", (7, 3, RPCS1), (0, 0))           # RPCS1 initial
    a.two("MOV", (7, 2, 0o12345), (7, 3, RPWC))     # RPWC
    a.two("MOV", (7, 3, RPWC), (1, 0))
    a.two("MOV", (7, 2, 0o4000), (7, 3, RPBA))      # RPBA
    a.two("MOV", (7, 3, RPBA), (2, 0))
    a.two("MOV", (7, 2, 0o12345), (7, 3, RPDA))     # RPDA
    a.two("MOV", (7, 3, RPDA), (3, 0))
    a.two("MOV", (7, 2, 0o1234), (7, 3, RPDC))      # RPDC
    a.two("MOV", (7, 3, RPDC), (4, 0))
    a.halt()
    a.emit(run=50, dumps=[])
    return 0


if __name__ == "__main__":
    sys.exit(main())
