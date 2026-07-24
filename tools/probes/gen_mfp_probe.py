#!/usr/bin/env python3
"""MFPI probe (P3b): read the previous mode's memory.

Running in kernel mode with previous mode = user, MFPI @#020000 reads user
virtual 020000 — which the user PAR maps to physical 0100000 (holding 0123456),
a different place than the kernel mapping — and pushes it on the kernel stack. A
following pop into R2 recovers the value.

  set kernel PDR[0]/PDR[7]/PAR[7]; user PDR[1]/PAR[1] -> PA 0100000
  MMR3=22-bit; MMR0=enable; PSW=030000 (kernel current, user previous)
  MFPI @#020000        ; kernel reads USER 020000 -> 0123456, push
  MOV (R6)+, R2        ; R2 = 0123456
  HALT

Physical 0100000 is preloaded with 0123456. Expect R2=0123456. Diffed against
SimH this validates previous-mode access via MFPI.

Usage:  gen_mfp_probe.py > tests/images/mfp.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.mov_imm(6, 0o1000)                               # kernel SP
    a.two("MOV", (7, 2, 0o077406), (7, 3, 0o172300))  # KI PDR[0]
    a.two("MOV", (7, 2, 0o077406), (7, 3, 0o172316))  # KI PDR[7]
    a.two("MOV", (7, 2, 0o177600), (7, 3, 0o172356))  # KI PAR[7] -> I/O
    a.two("MOV", (7, 2, 0o077406), (7, 3, 0o177602))  # UI PDR[1]
    a.two("MOV", (7, 2, 0o001000), (7, 3, 0o177642))  # UI PAR[1] -> PA 0100000
    a.two("MOV", (7, 2, 0o000020), (7, 3, 0o172516))  # MMR3 = 22-bit
    a.two("MOV", (7, 2, 0o000001), (7, 3, 0o177572))  # MMR0 = enable
    a.two("MOV", (7, 2, 0o030000), (7, 3, 0o177776))  # PSW: cm=K, pm=U
    a.word(0o006537)                                  # MFPI @#020000
    a.word(0o020000)
    a.two("MOV", (6, 2), (2, 0))                      # MOV (R6)+, R2
    a.halt()
    a.emit(run=500)
    print("w 100000 123456")   # physical value the user page maps to
    return 0


if __name__ == "__main__":
    sys.exit(main())
