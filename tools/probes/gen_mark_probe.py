#!/usr/bin/env python3
"""MARK probe: the subroutine-return stack-cleanup instruction.

  R5 = cont
  MARK 0        ; i = PC_after = markpt+2;  PC = R5 (=cont);  R5 = mem[i];  SP = i+2
  .word 004321  ; mem[i] -> R5
  cont: HALT

Expect after execution: R5 = 004321, SP = cont, PC halted at cont+2. Diffed
against SimH this validates MARK's PC/R5/SP sequence.

Usage:  gen_mark_probe.py > tests/images/mark.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.mov_imm(6, 0o2000)
    a.mov_label(5, "cont")
    a.word(0o006400)   # MARK 0
    a.word(0o004321)   # data loaded into R5
    a.label("cont")
    a.halt()
    a.emit(run=500)
    return 0


if __name__ == "__main__":
    sys.exit(main())
