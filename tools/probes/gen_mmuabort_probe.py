#!/usr/bin/env python3
"""MMU access-control abort probe (P3c).

Marks kernel page 2 read-only, enables management, and writes to it — which must
abort through vector 0250 (memory management). The handler records that it ran
and reads MMR0 (the frozen status: read-only error + faulting page).

  SP in page 0; PDR[0]=rw (code+stack), PDR[2]=read-only, PDR[7]=rw + PAR[7]->I/O
  MMR3=22-bit; MMR0=enable
  MOV #123, @#040000   ; write to read-only page 2 -> abort 0250
  INC R3               ; skipped
  HALT
  handler(250): INC R2; R0 = MMR0; HALT

Expect R2=1, R3=0, and R0 = MMR0 with the read-only bit and page 2 recorded.
Diffed against SimH this validates the access-control abort and MMR0 capture.

Usage:  gen_mmuabort_probe.py > tests/images/mmuabort.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.mov_imm(6, 0o17000)                               # SP in page 0
    a.two("MOV", (7, 2, 0o077406), (7, 3, 0o172300))   # KI PDR[0] rw
    a.two("MOV", (7, 2, 0o077402), (7, 3, 0o172304))   # KI PDR[2] read-only
    a.two("MOV", (7, 2, 0o077406), (7, 3, 0o172316))   # KI PDR[7] rw (I/O)
    a.two("MOV", (7, 2, 0o177600), (7, 3, 0o172356))   # KI PAR[7] -> I/O page
    a.two("MOV", (7, 2, 0o020), (7, 3, 0o172516))      # MMR3 = 22-bit
    a.two("MOV", (7, 2, 0o1), (7, 3, 0o177572))        # MMR0 = enable
    a.two("MOV", (7, 2, 0o123), (7, 3, 0o040000))      # write to read-only page
    a.one("INC", (3, 0))                               # must be skipped
    a.halt()
    a.label("handler")
    a.one("INC", (2, 0))
    a.two("MOV", (7, 3, 0o177572), (0, 0))             # R0 = MMR0
    a.halt()
    a.emit(run=500)
    print(f"w 250 {a.labels['handler']:o}")
    print("w 252 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
