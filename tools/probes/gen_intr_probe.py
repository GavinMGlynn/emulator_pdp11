#!/usr/bin/env python3
"""Interrupt/PIRQ probe.

At CPU priority 0, request a level-7 program interrupt by writing PIR7 to the
PIRQ register (0177772). The interrupt is granted before the next instruction,
vectoring through 0240 to a handler that runs at priority 7 (so the request is
masked while it is serviced), clears PIRQ, and RTIs back.

  MOV #2000,R6; CLR R0; CLR R2
  MOV #100000,@#177772   ; request PIR7
  INC R0                 ; runs only after the handler returns -> R0 = 1
  HALT
  handler(240,psw=340): INC R2; CLR @#177772; RTI

Expect R0=1, R2=1. Diffed against SimH this validates PIRQ encoding, the
priority-gated interrupt grant, and the vector-0240 dispatch.

Usage:  gen_intr_probe.py > tests/images/intr.image
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
    a.two("MOV", (7, 2, 0o100000), (7, 3, 0o177772))  # PIRQ <- PIR7
    a.one("INC", (0, 0))
    a.halt()
    a.label("handler")
    a.one("INC", (2, 0))
    a.one("CLR", (7, 3, 0o177772))                    # clear PIRQ
    a.word(0o000002)                                  # RTI

    a.emit(run=500, dumps=[(0o1770, 4)])
    print(f"w 240 {a.labels['handler']:o}")
    print("w 242 340")   # handler PSW: priority 7
    return 0


if __name__ == "__main__":
    sys.exit(main())
