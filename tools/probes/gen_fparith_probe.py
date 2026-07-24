#!/usr/bin/env python3
"""FP11-C arithmetic + conversion probe (P5c).

Single precision unless noted. Exercises the multiply/divide/compare/modulo
group and the integer/exponent conversions against SimH (whose FP11 arithmetic
this module is a faithful port of).

  SETF
  MUL : 2.0 * 3.0 = 6.0                 -> @#4100
  DIV : 6.0 / 2.0 = 3.0                 -> @#4104
  CMP : fac 3.0 vs fsrc 2.0 ; STFPS     -> @#4110  (condition codes)
  MOD : 3.0 * 0.5 = 1.5 -> int 1.0, frac 0.5
        STF AC1 (int)                   -> @#4114
        STF AC0 (frac)                  -> @#4120
  SETI
  LDCIF #5 ; STCFI  (word int round trip)-> @#4124
  SETF
  STEXP AC0 (exp of 0.5 = -1)           -> @#4130
  LDF 1.0,AC3 ; LDEXP #3 ; STF AC3      -> @#4134  (1.0 -> 2^3 * 0.5 = 4.0)

FP word = 0170000 | (major<<8) | (ac<<6) | spec.  major: LDF=5 STF=010 MUL=2
DIV=011 CMP=7 MOD=3 LDCIF=016 STCFI=013 STEXP=012 LDEXP=015.

Usage:  gen_fparith_probe.py > tests/images/fparith.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.word(0o170001)                    # SETF (single)
    # MUL: 2.0 * 3.0 = 6.0
    a.word(0o172437); a.word(0o004000)  # LDF  @#4000, AC0   (2.0)
    a.word(0o171037); a.word(0o004004)  # MULF @#4004, AC0   (* 3.0)
    a.word(0o174037); a.word(0o004100)  # STF  AC0, @#4100
    # DIV: 6.0 / 2.0 = 3.0
    a.word(0o172437); a.word(0o004010)  # LDF  @#4010, AC0   (6.0)
    a.word(0o174437); a.word(0o004000)  # DIVF @#4000, AC0   (/ 2.0)
    a.word(0o174037); a.word(0o004104)  # STF  AC0, @#4104
    # CMP: fac 3.0 vs fsrc 2.0
    a.word(0o172437); a.word(0o004004)  # LDF  @#4004, AC0   (3.0)
    a.word(0o173437); a.word(0o004000)  # CMPF @#4000, AC0   (vs 2.0)
    a.word(0o170237); a.word(0o004110)  # STFPS @#4110
    # MOD: 3.0 * 0.5 = 1.5 -> int 1.0 (AC1), frac 0.5 (AC0)
    a.word(0o172437); a.word(0o004004)  # LDF  @#4004, AC0   (3.0)
    a.word(0o171437); a.word(0o004014)  # MODF @#4014, AC0   (* 0.5)
    a.word(0o174137); a.word(0o004114)  # STF  AC1, @#4114   (integer part)
    a.word(0o174037); a.word(0o004120)  # STF  AC0, @#4120   (fraction)
    # integer conversions (word)
    a.word(0o170002)                    # SETI (word integer)
    a.word(0o177237); a.word(0o004020)  # LDCIF @#4020, AC2  (int 5 -> float)
    a.word(0o175637); a.word(0o004124)  # STCFI AC2, @#4124  (float -> int)
    # exponent conversions
    a.word(0o170001)                    # SETF
    a.word(0o175037); a.word(0o004130)  # STEXP AC0, @#4130  (exp of 0.5)
    a.word(0o172737); a.word(0o004024)  # LDF  @#4024, AC3   (1.0)
    a.word(0o176737); a.word(0o004022)  # LDEXP @#4022, AC3  (exp <- 3)
    a.word(0o174337); a.word(0o004134)  # STF  AC3, @#4134
    a.halt()
    a.emit(run=800, dumps=[(0o4100, 32)])
    # data operands
    print("w 4000 40400"); print("w 4002 0")   # 2.0
    print("w 4004 40500"); print("w 4006 0")   # 3.0
    print("w 4010 40700"); print("w 4012 0")   # 6.0
    print("w 4014 40000"); print("w 4016 0")   # 0.5
    print("w 4024 40200"); print("w 4026 0")   # 1.0
    print("w 4020 5")                           # integer 5
    print("w 4022 3")                           # exponent 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
