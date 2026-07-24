#!/usr/bin/env python3
"""FP11-C double-precision arithmetic probe (P5c).

Double precision (SETD): a 4-word accumulator whose fraction spans all four
words, exercising the 56-bit multiply/divide paths (frac_mulfp11's 24x56 loop
and divfp11's long quotient loop) rather than the 24-bit single-precision path.

  SETD
  DIV : 2.0 / 3.0 = 0.6666...  (fills the mantissa)     -> @#5100
  MUL : 0.6666... * 3.0 = ~2.0 (round-trip)             -> @#5110
  CMP : fac 3.0 vs fsrc 2.0 ; STFPS                      -> @#5120

Opcodes are precision-agnostic (LDF/STF etc.); the D bit in FPS selects double.

Usage:  gen_fpdbl_probe.py > tests/images/fpdbl.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.word(0o170011)                    # SETD (double)
    # DIV: 2.0 / 3.0 -> repeating fraction across all four words
    a.word(0o172437); a.word(0o005000)  # LDD  @#5000, AC0   (2.0)
    a.word(0o174437); a.word(0o005010)  # DIVD @#5010, AC0   (/ 3.0)
    a.word(0o174037); a.word(0o005100)  # STD  AC0, @#5100
    # MUL: (2/3) * 3.0 -> ~2.0 round trip
    a.word(0o172437); a.word(0o005100)  # LDD  @#5100, AC0   (2/3)
    a.word(0o171037); a.word(0o005010)  # MULD @#5010, AC0   (* 3.0)
    a.word(0o174037); a.word(0o005110)  # STD  AC0, @#5110
    # CMP: fac 3.0 vs fsrc 2.0
    a.word(0o172437); a.word(0o005010)  # LDD  @#5010, AC0   (3.0)
    a.word(0o173437); a.word(0o005000)  # CMPD @#5000, AC0   (vs 2.0)
    a.word(0o170237); a.word(0o005120)  # STFPS @#5120
    a.halt()
    a.emit(run=800, dumps=[(0o5100, 24)])
    # double operands: word0 holds sign/exp, words 1-3 the low fraction (0 here)
    print("w 5000 40400"); print("w 5002 0"); print("w 5004 0"); print("w 5006 0")  # 2.0
    print("w 5010 40500"); print("w 5012 0"); print("w 5014 0"); print("w 5016 0")  # 3.0
    return 0


if __name__ == "__main__":
    sys.exit(main())
