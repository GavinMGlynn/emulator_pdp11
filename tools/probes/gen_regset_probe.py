#!/usr/bin/env python3
"""Dual-register-set probe (P3b).

Writes R0 in register set 0, switches to set 1 (PSW<11>) via the PSW at 0177776,
writes R0 in set 1, then switches back — proving R0 is banked. Each set's R0 is
saved to memory (memory isn't banked) for comparison.

  MOV #1111, R0          ; set 0 R0
  MOV #4000, @#177776    ; PSW <- register set 1
  MOV #2222, R0          ; set 1 R0
  MOV R0, @#4000         ; save set 1 R0
  MOV #0, @#177776       ; PSW <- register set 0
  MOV R0, @#4002         ; save set 0 R0  (should be 1111 again)
  HALT

Expect mem[4000]=2222, mem[4002]=1111, and R0=1111 at the end.

Usage:  gen_regset_probe.py > tests/images/regset.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.mov_imm(0, 0o1111)
    a.two("MOV", (7, 2, 0o4000), (7, 3, 0o177776))  # PSW <- set 1
    a.mov_imm(0, 0o2222)
    a.two("MOV", (0, 0), (7, 3, 0o4000))            # save set 1 R0
    a.two("MOV", (7, 2, 0o0), (7, 3, 0o177776))     # PSW <- set 0
    a.two("MOV", (0, 0), (7, 3, 0o4002))            # save set 0 R0
    a.halt()
    a.emit(run=500, dumps=[(0o4000, 2)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
