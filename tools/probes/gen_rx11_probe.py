#!/usr/bin/env python3
"""RX11 register probe.

Reads the power-up state of the RX11 with no floppy attached. The controller's
power-up initialize reads track 1 / sector 1; with nothing loaded it fails, so
RXCS reports DONE + ERR (0100040) and RXDB holds RXES = init-done (004). A data
transfer needs an attached image and is covered by unit tests.

  MOV @#177170, R0     ; RXCS at power-up -> 0100040 (DONE | ERR)
  MOV @#177172, R1     ; RXDB -> RXES = 004 (init done, no drive ready)
  HALT

Usage:  gen_rx11_probe.py > tests/images/rx11.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402

RXCS = 0o177170
RXDB = 0o177172


def main():
    a = Asm(org=0o1000)
    a.two("MOV", (7, 3, RXCS), (0, 0))          # R0 = RXCS (power-up)
    a.two("MOV", (7, 3, RXDB), (1, 0))          # R1 = RXDB (RXES)
    a.halt()
    a.emit(run=40)
    return 0


if __name__ == "__main__":
    sys.exit(main())
