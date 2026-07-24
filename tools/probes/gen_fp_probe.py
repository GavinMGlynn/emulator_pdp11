#!/usr/bin/env python3
"""FP11-C control-instruction probe (P5a).

Exercises the floating-point control group without any float arithmetic:
LDFPS/STFPS move the FP status word, SETD/SETI/SETL set mode bits, and CFCC
copies the FP condition codes into the PSW.

  LDFPS #200 ; STFPS @#4000     ; FPS = D bit -> mem[4000] = 200
  SETL ; SETI                    ; set then clear the long bit
  LDFPS #17 ; CFCC               ; FPS CC = NZVC, copy to PSW
  STFPS @#4002                   ; mem[4002] = 17
  HALT

Expect mem[4000]=200, mem[4002]=17, PSW=17. Diffed against SimH (which has the
FP11 on the 11/70) this validates the control instructions.

Usage:  gen_fp_probe.py > tests/images/fp.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.mov_imm(6, 0o2000)
    a.word(0o170127); a.word(0o000200)  # LDFPS #200
    a.word(0o170237); a.word(0o004000)  # STFPS @#4000
    a.word(0o170012)                    # SETL
    a.word(0o170002)                    # SETI
    a.word(0o170127); a.word(0o000017)  # LDFPS #17
    a.word(0o170000)                    # CFCC
    a.word(0o170237); a.word(0o004002)  # STFPS @#4002
    a.halt()
    a.emit(run=500, dumps=[(0o4000, 2)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
