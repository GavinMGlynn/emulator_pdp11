#!/usr/bin/env python3
"""Control-flow probe: exercises SOB loops, conditional branches, and JSR/RTS.

  R0 = 5
  R1 = 0
  loop: R1 += R0; SOB R0,loop      -> R1 = 5+4+3+2+1 = 017
  set up SP; TST R2 (==0, Z set); BNE notzero (not taken); INC R1  -> R1 = 020
  notzero: JSR PC,dbl              -> subroutine doubles R1 into R3
  HALT
  dbl: MOV R1,R3; ASL R3; RTS PC

Diffed whole-state against SimH, this validates branch conditions, SOB, and the
JSR/RTS stack linkage (the dumped stack shows the pushed return address).

Usage:  gen_flow_probe.py > tests/images/flow.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.mov_imm(0, 5)
    a.one("CLR", (1, 0))
    a.label("loop")
    a.two("ADD", (0, 0), (1, 0))
    a.sob(0, "loop")
    a.mov_imm(6, 0o2000)          # stack pointer
    a.one("TST", (2, 0))          # R2 == 0 -> Z set
    a.branch("BNE", "notzero")    # not taken
    a.one("INC", (1, 0))          # executed -> R1 = 020
    a.label("notzero")
    a.jsr(7, "dbl")               # JSR PC, dbl
    a.halt()
    a.label("dbl")
    a.two("MOV", (1, 0), (3, 0))
    a.one("ASL", (3, 0))          # R3 = R1 * 2
    a.rts(7)
    a.emit(run=500, dumps=[(0o1770, 8)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
