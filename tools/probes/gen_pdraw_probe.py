#!/usr/bin/env python3
"""PDR A/W-flag write behaviour probe.

The PDR A (accessed, bit 7) and W (written, bit 6) flags are hardware-managed;
a program write to a PDR clears them (SimH: (data & pdr) & ~(PDR_A|PDR_W)). They
are only ever set by the MMU on an access.

  MOV #0306, @#172300  ; write Kernel-I PDR[0] = ACF 6 (rw) + A + W
  MOV @#172300, R0     ; read it back -> 006 (A/W cleared, ACF kept)
  HALT

Expect R0 = 006. Diffed against SimH. Runs unmapped (the PDR file is in the
I/O page and reachable without relocation).

Usage:  gen_pdraw_probe.py > tests/images/pdraw.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402


def main():
    a = Asm(org=0o1000)
    a.two("MOV", (7, 2, 0o306), (7, 3, 0o172300))  # PDR[0] = rw + A + W
    a.two("MOV", (7, 3, 0o172300), (0, 0))         # R0 = PDR[0] read-back
    a.halt()
    a.emit(run=50)
    return 0


if __name__ == "__main__":
    sys.exit(main())
