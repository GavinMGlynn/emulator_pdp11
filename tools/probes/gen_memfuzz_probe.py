#!/usr/bin/env python3
"""Differential CPU fuzz probe — memory addressing modes (P7c bring-up).

Like gen_fuzz_probe but the destination operand uses a memory addressing mode off
a pointer register (R3): (R3), (R3)+, -(R3), X(R3). Each test seeds a small data
window, points R3 into it, runs one instruction, then stores R3, the window, and
the PSW to a result table. Diffing the table against the SimH oracle catches any
effective-address, autoincrement/decrement side effect, or read/write bug — the
kind the register-only fuzzer can't reach.

Usage:  gen_memfuzz_probe.py [seed] > tests/images/memfuzz.image
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402

PSW = 0o177776
WIN = 0o6074          # data-window base (6 words: 06074..06106)
PTR = 0o6100          # R3 points here (WIN + 4)
NWIN = 6
VALUES = [0, 1, 2, 0o177777, 0o100000, 0o077777, 0o000200, 0o177600, 0o000377,
          0o177400, 0o125252, 0o052525]
TWO = ["ADD", "SUB", "CMP", "BIT", "BIC", "BIS", "MOV",
       "MOVB", "CMPB", "BISB", "BICB", "SUB"]
ONE = ["CLR", "COM", "INC", "DEC", "NEG", "TST", "ROR", "ROL", "ASR", "ASL",
       "SXT", "ADC", "SBC"]
# dst addressing modes on R3: (R3), (R3)+, -(R3), X(R3)
MODES = [(3, 1), (3, 2), (3, 4), (3, 6, 4)]


def rv(rng):
    return rng.choice(VALUES) if rng.random() < 0.7 else rng.randint(0, 0o177777)


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    rng = random.Random(seed)
    a = Asm(org=0o1000)
    res = 0o20000
    ntest = 60
    off = 0
    for _ in range(ntest):
        two = rng.random() < 0.6
        op = rng.choice(TWO if two else ONE)
        mode = rng.choice(MODES)
        for j in range(NWIN):                     # seed the window
            a.two("MOV", (7, 2, rv(rng)), (7, 3, WIN + j * 2))
        a.mov_imm(3, PTR)                          # R3 -> window
        a.mov_imm(0, rv(rng))                      # R0 = src
        a.two("MOV", (7, 2, 0), (7, 3, PSW))       # clear PSW
        if two:
            a.two(op, (0, 0), mode)
        else:
            a.one(op, mode)
        a.two("MOV", (3, 0), (7, 3, res + off))    # store R3
        for j in range(NWIN):                      # store the window
            a.two("MOV", (7, 3, WIN + j * 2), (7, 3, res + off + 2 + j * 2))
        a.two("MOV", (7, 3, PSW), (7, 3, res + off + 2 + NWIN * 2))  # store PSW
        off += 2 + NWIN * 2 + 2                     # R3 + window + PSW
    a.halt()
    a.emit(run=8000, dumps=[(res, off // 2)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
