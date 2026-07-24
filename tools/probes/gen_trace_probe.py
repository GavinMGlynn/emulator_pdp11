#!/usr/bin/env python3
"""T-bit trace-trap probe.

Arms the T bit by RTI-ing to a PSW with T set, then runs one instruction — which
must take the trace trap (vector 014) *after* completing. The instruction after
it must NOT run.

  MOV #2000,R6; push PSW=020 (T); push PC=cont; RTI
  cont:  CLR R0        ; runs with T set -> trace trap to 014
         INC R5        ; must be skipped
         HALT
  handler(014): INC R1; HALT

Expect R0=0 (CLR ran), R1=1 (trace handler ran), R5=0 (skipped). Diffed against
SimH this pins the T-bit timing.

Usage:  gen_trace_probe.py > tests/images/trace.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.mov_imm(6, 0o2000)
    a.push_imm(0o020)       # PSW with T set
    a.push_label("cont")    # return PC
    a.word(0o000002)        # RTI -> T armed, jump to cont
    a.label("cont")
    a.one("CLR", (0, 0))    # traced instruction
    a.one("INC", (5, 0))    # must be skipped by the trap
    a.halt()
    a.label("handler")
    a.one("INC", (1, 0))
    a.halt()

    a.emit(run=500, dumps=[(0o1764, 8)])
    print(f"w 14 {a.labels['handler']:o}")
    print("w 16 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
