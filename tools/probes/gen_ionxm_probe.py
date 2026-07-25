#!/usr/bin/env python3
"""I/O-page CPU registers + non-existent-memory trap probe (11/70).

Two behaviours diffed against SimH:
  1. A modelled 11/70 CPU register reads its real value: SYSID (0177764) = 011064.
  2. A read of an unclaimed I/O-page address is a bus timeout -> NXM trap through
     vector 4, not a silent read of 0.

  MOV @#177764, R0   ; R0 = SYSID = 011064
  MOV @#164000, R1   ; unclaimed I/O address -> NXM trap to vector 4
  INC R2             ; skipped by the trap
  HALT
  handler(4): INC R3; HALT

Expect R0=011064 (SYSID), R1=0 (the faulting read leaves R1 untouched), R2=0
(skipped), R3=1 (the NXM handler ran). Runs unmapped (no MMU).

Usage:  gen_ionxm_probe.py > tests/images/ionxm.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.mov_imm(6, 0o4000)                              # a real stack
    a.two("MOV", (7, 3, 0o177764), (0, 0))           # R0 = SYSID
    a.two("MOV", (7, 3, 0o164000), (1, 0))           # R1 = @#164000 -> NXM
    a.one("INC", (2, 0))                             # skipped
    a.halt()
    a.label("handler")
    a.one("INC", (3, 0))                             # R3 = 1: the NXM handler ran
    a.halt()
    a.emit(run=100)
    print(f"w 4 {a.labels['handler']:o}")
    print("w 6 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
