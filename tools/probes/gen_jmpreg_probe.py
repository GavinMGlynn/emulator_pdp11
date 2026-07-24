#!/usr/bin/env python3
"""Reserved-mode probe: JMP to a register is illegal on the 11/70.

A register operand has no memory address, so JMP/JSR to mode 0 traps through
vector 010 (the 11/70 lacks HAS_JREG4, which would send it to vector 4).

  CLR R0
  JMP R0        ; illegal -> trap 10 -> handler (INC R2; HALT)
  INC R0        ; skipped
  HALT

Expect R0=0 (INC skipped), R2=1 (handler ran), pushed PC=001010. Diffed against
SimH this validates the illegal-addressing trap.

Usage:  gen_jmpreg_probe.py > tests/images/jmpreg.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.mov_imm(6, 0o2000)
    a.one("CLR", (0, 0))
    a.word(0o000100)   # JMP R0 (mode 0, reg 0)
    a.one("INC", (0, 0))
    a.halt()
    a.label("handler")
    a.one("INC", (2, 0))
    a.halt()
    a.emit(run=500, dumps=[(0o1774, 2)])
    print(f"w 10 {a.labels['handler']:o}")
    print("w 12 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
