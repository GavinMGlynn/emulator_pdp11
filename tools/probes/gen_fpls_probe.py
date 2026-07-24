#!/usr/bin/env python3
"""FP11-C load/store probe (P5b).

Single precision. mem[4000] holds 1.0 (040200 000000). LDF loads it into AC0,
STF stores it back to 4010, NEGF flips the sign, and STF stores the negation to
4014.

  SETF                      ; single mode
  LDF @#4000, AC0
  STF AC0, @#4010           ; mem[4010] = 1.0
  NEGF AC0
  STF AC0, @#4014           ; mem[4014] = -1.0 (sign bit set: 0140200)
  HALT

Expect mem[4010]=040200/0, mem[4014]=0140200/0. Diffed against SimH this
validates the FAC packing and LDF/STF/NEGF.

Usage:  gen_fpls_probe.py > tests/images/fpls.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.word(0o170001)                    # SETF (single)
    a.word(0o172437); a.word(0o004000)  # LDF @#4000, AC0
    a.word(0o174037); a.word(0o004010)  # STF AC0, @#4010
    a.word(0o170700)                    # NEGF AC0
    a.word(0o174037); a.word(0o004014)  # STF AC0, @#4014
    a.halt()
    a.emit(run=500, dumps=[(0o4000, 8)])
    print("w 4000 40200")   # 1.0 high word
    print("w 4002 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
