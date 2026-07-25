#!/usr/bin/env python3
"""RL11 register probe.

Exercises the deterministic RL11 registers (like the rk11 probe; a data transfer
needs an attached image and is covered by unit tests). With no drive attached the
controller reads RLCS = 000200 (DONE, drive-ready clear). RLBA/RLDA/RLMP are
plain read/write registers. On a Unibus machine RLBAE (0174410) does not exist,
so it is not probed.

  MOV @#174400, R0     ; RLCS initial -> 000200 (DONE, no DRDY: nothing attached)
  MOV #4000, @#174402  ; RLBA = 04000
  MOV @#174402, R1     ; -> 04000
  MOV #1234, @#174404  ; RLDA = 01234
  MOV @#174404, R2     ; -> 01234
  MOV #12345, @#174406 ; RLMP = 012345
  MOV @#174406, R3     ; -> 012345
  HALT

Usage:  gen_rl11_probe.py > tests/images/rl11.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402

RLCS = 0o174400
RLBA = 0o174402
RLDA = 0o174404
RLMP = 0o174406


def main():
    a = Asm(org=0o1000)
    a.two("MOV", (7, 3, RLCS), (0, 0))            # MOV @#RLCS, R0 (initial)
    a.two("MOV", (7, 2, 0o4000), (7, 3, RLBA))    # MOV #4000, @#RLBA
    a.two("MOV", (7, 3, RLBA), (1, 0))            # MOV @#RLBA, R1
    a.two("MOV", (7, 2, 0o1234), (7, 3, RLDA))    # MOV #1234, @#RLDA
    a.two("MOV", (7, 3, RLDA), (2, 0))            # MOV @#RLDA, R2
    a.two("MOV", (7, 2, 0o12345), (7, 3, RLMP))   # MOV #12345, @#RLMP
    a.two("MOV", (7, 3, RLMP), (3, 0))            # MOV @#RLMP, R3
    a.halt()
    a.emit(run=50)
    return 0


if __name__ == "__main__":
    sys.exit(main())
