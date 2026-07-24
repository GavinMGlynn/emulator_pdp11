#!/usr/bin/env python3
"""FP11-C add/subtract probe (P5c).

Single precision. Verifies ADDF and SUBF against SimH (whose FP arithmetic this
module is a faithful port of).

  LDF  @#4000, AC0    ; 1.0
  ADDF @#4004, AC0    ; + 1.0  -> 2.0    -> @#4020
  LDF  @#4010, AC0    ; 2.0
  SUBF @#4000, AC0    ; - 1.0  -> 1.0    -> @#4024
  HALT

FP word = 0170000 | (major<<8) | (ac<<6) | spec. LDf=5, ADDf=4, SUBf=6, STf=010.
Expect @#4020 = 040400 (2.0), @#4024 = 040200 (1.0).

Usage:  gen_fpadd_probe.py > tests/images/fpadd.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.word(0o170001)                    # SETF (single)
    a.word(0o172437); a.word(0o004000)  # LDF  @#4000, AC0
    a.word(0o172037); a.word(0o004004)  # ADDF @#4004, AC0
    a.word(0o174037); a.word(0o004020)  # STF  AC0, @#4020
    a.word(0o172437); a.word(0o004010)  # LDF  @#4010, AC0
    a.word(0o173037); a.word(0o004000)  # SUBF @#4000, AC0
    a.word(0o174037); a.word(0o004024)  # STF  AC0, @#4024
    a.halt()
    a.emit(run=500, dumps=[(0o4020, 4)])
    print("w 4000 40200"); print("w 4002 0")   # 1.0
    print("w 4004 40200"); print("w 4006 0")   # 1.0
    print("w 4010 40400"); print("w 4012 0")   # 2.0
    return 0


if __name__ == "__main__":
    sys.exit(main())
