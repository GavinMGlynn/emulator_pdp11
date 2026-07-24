#!/usr/bin/env python3
"""KW11-L line-clock register probe (P6).

Exercises the LKS status register at 0177546 without relying on a tick (which is
wall-clock-calibrated in SimH and can't be golden-diffed): only IE (bit 6) is
directly writable; the DONE/monitor bit (bit 7) is set by the tick and cleared
by writing a 0 to it, never set by a write. Results land in R0-R3, which the
oracle dumps.

  MOV @#177546, R0    ; initial LKS (0)
  MOV #100, @#177546   ; set IE
  MOV @#177546, R1    ; -> 100
  MOV #300, @#177546   ; IE|DONE: DONE is not writable, stays clear
  MOV @#177546, R2    ; -> 100
  MOV #0, @#177546     ; clear IE
  MOV @#177546, R3    ; -> 0
  HALT

Usage:  gen_clk_probe.py > tests/images/clk.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402

LKS = 0o177546


def main():
    a = Asm(org=0o1000)
    a.two("MOV", (7, 3, LKS), (0, 0))       # MOV @#LKS, R0   (initial)
    a.two("MOV", (7, 2, 0o100), (7, 3, LKS))  # MOV #100, @#LKS (set IE)
    a.two("MOV", (7, 3, LKS), (1, 0))       # MOV @#LKS, R1
    a.two("MOV", (7, 2, 0o300), (7, 3, LKS))  # MOV #300, @#LKS (IE|DONE)
    a.two("MOV", (7, 3, LKS), (2, 0))       # MOV @#LKS, R2
    a.two("MOV", (7, 2, 0o0), (7, 3, LKS))    # MOV #0, @#LKS   (clear)
    a.two("MOV", (7, 3, LKS), (3, 0))       # MOV @#LKS, R3
    a.halt()
    a.emit(run=50, dumps=[])
    return 0


if __name__ == "__main__":
    sys.exit(main())
