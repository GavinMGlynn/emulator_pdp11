#!/usr/bin/env python3
"""Stack-limit yellow-zone trap probe (11/70, STKLIM register = 0).

A Kernel-mode push that drives SP below the yellow boundary (STKLIM + 0400 = 0400
with STKLIM=0) arms a warning trap through vector 4 after the instruction, and
sets the yellow bit (010) in CPUERR.

  MOV #0402, SP     ; SP above the yellow zone
  MOV #1, -(SP)     ; SP=0400 — exactly the boundary, no trap
  MOV #2, -(SP)     ; SP=0376 — yellow zone -> arm the trap (deferred)
  INC R0            ; preempted by the yellow trap before it runs
  HALT
  handler(4): INC R1; R3 = CPUERR; HALT

Expect R0=0 (preempted), R1=1 (the yellow trap ran), R3=010 (CPUERR yellow bit).
The handler HALTs immediately so the trap's own in-zone push does not re-arm.
Diffed against SimH. Runs unmapped (no MMU); the 11/70 is Kernel at reset.

Usage:  gen_stklim_probe.py > tests/images/stklim.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.mov_imm(6, 0o402)                    # SP above the yellow zone
    a.two("MOV", (7, 2, 0o1), (6, 4))      # MOV #1, -(SP) -> SP=0400 (boundary)
    a.two("MOV", (7, 2, 0o2), (6, 4))      # MOV #2, -(SP) -> SP=0376 (yellow)
    a.one("INC", (0, 0))                   # preempted
    a.halt()
    a.label("handler")
    a.one("INC", (1, 0))                   # the yellow trap ran
    a.two("MOV", (7, 3, 0o177766), (3, 0)) # R3 = CPUERR
    a.halt()
    a.emit(run=100)
    print(f"w 4 {a.labels['handler']:o}")
    print("w 6 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
