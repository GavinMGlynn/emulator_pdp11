#!/usr/bin/env python3
"""RK11 register probe (P6).

Exercises the deterministic RK11 read/write registers (RKDS is read-only and
returns a random sector count in SimH, so it is not probed; a data transfer
needs an attached image and is covered by unit tests). Results land in R0-R3,
which the oracle dumps.

  MOV @#177404, R0     ; RKCS initial -> 200 (DONE)
  MOV #12345, @#177406  ; RKWC = 012345
  MOV @#177406, R1     ; -> 012345
  MOV #4000, @#177410   ; RKBA = 04000
  MOV @#177410, R2     ; -> 04000
  MOV #1234, @#177412   ; RKDA = 01234 (writable while DONE)
  MOV @#177412, R3     ; -> 01234
  HALT

Usage:  gen_rk11_probe.py > tests/images/rk11.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402

RKCS = 0o177404
RKWC = 0o177406
RKBA = 0o177410
RKDA = 0o177412


def main():
    a = Asm(org=0o1000)
    a.two("MOV", (7, 3, RKCS), (0, 0))          # MOV @#RKCS, R0 (initial)
    a.two("MOV", (7, 2, 0o12345), (7, 3, RKWC))   # MOV #12345, @#RKWC
    a.two("MOV", (7, 3, RKWC), (1, 0))          # MOV @#RKWC, R1
    a.two("MOV", (7, 2, 0o4000), (7, 3, RKBA))    # MOV #4000, @#RKBA
    a.two("MOV", (7, 3, RKBA), (2, 0))          # MOV @#RKBA, R2
    a.two("MOV", (7, 2, 0o1234), (7, 3, RKDA))    # MOV #1234, @#RKDA
    a.two("MOV", (7, 3, RKDA), (3, 0))          # MOV @#RKDA, R3
    a.halt()
    a.emit(run=50, dumps=[])
    return 0


if __name__ == "__main__":
    sys.exit(main())
