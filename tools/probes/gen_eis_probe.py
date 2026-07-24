#!/usr/bin/env python3
"""EIS probe: MUL/DIV/ASH/ASHC/XOR over flag-critical operands.

For each case it seeds R0 (and R1 for the 32-bit pair ops), runs the EIS
instruction on the R0/R1 pair, then records R0, R1 and the PSW low byte (via
MFPS) into a results buffer at 04000. Diffed against SimH this validates the
full EIS result + condition-code behaviour, including MUL carry, divide-by-zero,
divide overflow, and the ASH/ASHC shift-out / overflow rules.

Usage:  gen_eis_probe.py > tests/images/eis.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402

CASES = [
    # (seed_r0, seed_r1, op, src)   src is an immediate operand
    (0o000010, 0, "MUL", 5),        # 8 * 5 = 40
    (0o100000, 0, "MUL", 2),        # -32768 * 2 -> C set (won't fit)
    (0o177777, 0, "MUL", 3),        # -1 * 3 = -3
    (0o000000, 0o000144, "DIV", 7), # 100 / 7 = 14 r 2
    (0o000000, 0o000012, "DIV", 0), # divide by zero
    (0o000010, 0, "DIV", 1),        # (8<<16)/1 -> quotient overflow
    (0o000005, 0, "ASH", 3),        # left 3  -> 40
    (0o000020, 0, "ASH", 0o76),     # right 2 -> 4
    (0o000003, 0, "ASH", 0o17),     # left 15
    (0o001234, 0, "ASH", 0),        # no shift
    (0o000000, 0o000001, "ASHC", 4),   # left 4
    (0o000001, 0o000000, "ASHC", 0o74),# right 4
    (0o100000, 0o000000, "ASHC", 0o40),# shift 32 (= -32)
    (0o125252, 0, "XOR", 0o052525),    # XOR to all ones
]


def main():
    a = Asm(org=0o1000)
    a.mov_imm(5, 0o4000)   # results buffer pointer
    for r0, r1, op, src in CASES:
        a.mov_imm(0, r0)
        a.mov_imm(1, r1)
        a.eis(op, 0, (7, 2, src))                 # op #src, R0
        a.two("MOV", (0, 0), (5, 2))              # store R0 -> (R5)+
        a.two("MOV", (1, 0), (5, 2))              # store R1 -> (R5)+
        a.two("MOV", (7, 3, 0o177776), (5, 2))    # store PSW (@#177776) -> (R5)+
    a.halt()
    a.emit(run=2000, dumps=[(0o4000, len(CASES) * 3)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
