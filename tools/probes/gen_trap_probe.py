#!/usr/bin/env python3
"""Trap probe: TRAP -> handler -> RTI, validating the trap/return stack sequence.

  vector 034 -> handler PC, 036 -> handler PSW (0)
  main:    MOV #2000,R6; CLR R0; CLR R1; TRAP; INC R1; HALT
  handler: INC R0; ADD #100,R2; RTI       (returns to INC R1)

After it runs: R0=1 (handler ran once), R1=1 (resumed after TRAP), R2=0100.
The stack dump shows the PSW and PC the TRAP pushed. Diffed against SimH this
validates do_trap's push order and RTI's pop order.

Usage:  gen_trap_probe.py > tests/images/trap.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    # Vector 034/036 must be set before the program runs; place them with raw
    # words via a second tiny Asm pass is awkward, so patch them by absolute
    # deposits appended after emit(). Simplest: build the program, then print
    # the vector deposits ourselves.
    a.mov_imm(6, 0o2000)          # SP
    a.one("CLR", (0, 0))
    a.one("CLR", (1, 0))
    a.one("CLR", (2, 0))
    a.ccop(0o104400)              # TRAP
    a.one("INC", (1, 0))          # resume point
    a.halt()
    a.label("handler")
    a.one("INC", (0, 0))
    a.two("ADD", (7, 2, 0o100), (2, 0))  # ADD #100, R2 (immediate src)
    a.word(0o000002)              # RTI

    # Emit program, then the trap vector (034 -> handler, 036 -> PSW 0).
    a.emit(run=500, dumps=[(0o1770, 6)])
    print(f"w 34 {a.labels['handler']:o}")
    print("w 36 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
