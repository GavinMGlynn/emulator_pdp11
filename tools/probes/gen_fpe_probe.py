#!/usr/bin/env python3
"""FP11-C floating-exception trap probe (P5c).

A divide-by-zero posts FEC_DZRO and, with interrupts enabled (FPS_ID clear),
traps through the FPE vector 0244. The handler stores the FP exception status
(STST: FEC then FEA) so the diff confirms the trap fired, the code is right, and
FEA points at the faulting DIVF instruction. R0 stays 0 to prove the instruction
after the fault was skipped.

  @1000  SETF ; LDF 2.0,AC0 ; DIVF @#4004 (0.0)   -> FPE trap to 0244
         MOV #1,R0 ; HALT                          (must NOT run)
  @2000  STST @#4100 ; HALT                        (handler)

FEC_DZRO = 4.  FEA = address of the DIVF opcode (01006).

Usage:  gen_fpe_probe.py > tests/images/fpe.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.word(0o170001)                    # SETF
    a.word(0o172437); a.word(0o004000)  # LDF  @#4000, AC0   (2.0)
    a.word(0o174437); a.word(0o004004)  # DIVF @#4004, AC0   (/ 0.0) -> trap
    a.mov_imm(0, 0o1)                   # MOV #1, R0         (skipped if trap)
    a.halt()
    # FPE handler at 2000
    a.org(0o2000)
    a.word(0o170337); a.word(0o004100)  # STST @#4100   (FEC -> 4100, FEA -> 4102)
    a.halt()
    a.emit(run=200, dumps=[(0o4100, 2)])
    # FPE trap vector 244 -> handler 2000, PSW 0
    print("w 244 2000"); print("w 246 0")
    print("w 4000 40400"); print("w 4002 0")   # 2.0
    print("w 4004 0"); print("w 4006 0")       # 0.0 (divisor)
    return 0


if __name__ == "__main__":
    sys.exit(main())
