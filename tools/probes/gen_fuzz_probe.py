#!/usr/bin/env python3
"""Differential CPU fuzz probe (P7c bring-up).

Emits a probe of many register-mode instructions over edge-case + pseudo-random
operands. Each test clears the PSW, loads two registers, runs one instruction,
and stores the resulting PSW and destination register to a result table. Run the
same image on our headless core and on the SimH oracle and diff the table: any
mismatch is a CPU result/flag bug (the kind that sends a kernel branch the wrong
way). Register-mode only, so nothing faults.

Usage:  gen_fuzz_probe.py [seed] > tests/images/fuzz.image
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402

PSW = 0o177776
VALUES = [0, 1, 2, 0o177777, 0o100000, 0o077777, 0o000200, 0o177600, 0o000377,
          0o177400, 0o125252, 0o052525, 0o000010, 0o000100, 0o007777, 0o170000]

TWO = ["ADD", "SUB", "CMP", "BIT", "BIC", "BIS", "MOV",
       "MOVB", "CMPB", "BITB", "BICB", "BISB", "SUB"]
ONE = ["CLR", "COM", "INC", "DEC", "NEG", "TST", "ROR", "ROL", "ASR", "ASL",
       "SXT", "ADC", "SBC", "SWAB"]
EIS = ["MUL", "DIV", "ASH", "ASHC"]


def rv(rng):
    return rng.choice(VALUES) if rng.random() < 0.7 else rng.randint(0, 0o177777)


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    rng = random.Random(seed)
    a = Asm(org=0o1000)
    res = 0o20000  # result table base, kept well clear of the growing code
    ntest = 120
    off = 0
    for _ in range(ntest):
        va, vb = rv(rng), rv(rng)
        kind = rng.choice(["two", "one", "eis"])
        a.mov_imm(0, va)                       # R0 = va (src)
        a.mov_imm(1, vb)                       # R1 = vb (dst)
        a.mov_imm(4, vb)                       # R4 = vb (eis dst pair)
        a.two("MOV", (7, 2, 0), (7, 3, PSW))   # clear PSW
        if kind == "two":
            op = rng.choice(TWO)
            a.two(op, (0, 0), (1, 0))          # op R0, R1
            dst = 1
        elif kind == "one":
            op = rng.choice(ONE)
            a.one(op, (1, 0))                  # op R1
            dst = 1
        else:
            op = rng.choice(EIS)
            a.eis(op, 4, (0, 0))               # op R0 -> R4[:R5]
            dst = 4
        a.two("MOV", (7, 3, PSW), (2, 0))      # R2 = PSW
        a.two("MOV", (2, 0), (7, 3, res + off))       # store PSW
        a.two("MOV", (dst, 0), (7, 3, res + off + 2)) # store dst reg
        off += 4
    a.halt()
    a.emit(run=4000, dumps=[(res, ntest * 2)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
