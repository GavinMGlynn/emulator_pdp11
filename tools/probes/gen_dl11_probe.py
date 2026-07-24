#!/usr/bin/env python3
"""DL11 console register probe (P6).

Exercises the deterministic DL11 CSR semantics (no XBUF transmit, whose busy
window is wall-clock-timed in SimH and can't be golden-diffed): the receiver
powers up idle, the transmitter ready (DONE); only IE is writable; reads expose
DONE|IE. Results land in R0-R4, which the oracle dumps.

  MOV @#177560, R0     ; RCSR initial -> 0    (receiver idle)
  MOV @#177564, R1     ; XCSR initial -> 200  (transmitter ready)
  MOV #100, @#177560    ; set receiver IE
  MOV @#177560, R2     ; -> 100
  MOV #100, @#177564    ; set transmitter IE
  MOV @#177564, R3     ; -> 300  (DONE|IE)
  MOV #0, @#177560      ; clear receiver IE
  MOV @#177560, R4     ; -> 0
  HALT

Usage:  gen_dl11_probe.py > tests/images/dl11.image
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm import Asm  # noqa: E402

RCSR = 0o177560
XCSR = 0o177564
PSW = 0o177776


def main():
    a = Asm(org=0o1000)
    # Mask interrupts (priority 7): enabling the transmitter IE while its DONE is
    # set would otherwise raise an immediate BR4 interrupt through the unset
    # vector 064. With interrupts masked this exercises just the CSR semantics.
    a.two("MOV", (7, 2, 0o340), (7, 3, PSW))    # MOV #340, @#177776 (PSW pri 7)
    a.two("MOV", (7, 3, RCSR), (0, 0))        # MOV @#RCSR, R0 (initial)
    a.two("MOV", (7, 3, XCSR), (1, 0))        # MOV @#XCSR, R1 (initial)
    a.two("MOV", (7, 2, 0o100), (7, 3, RCSR))   # MOV #100, @#RCSR (rcv IE)
    a.two("MOV", (7, 3, RCSR), (2, 0))        # MOV @#RCSR, R2
    a.two("MOV", (7, 2, 0o100), (7, 3, XCSR))   # MOV #100, @#XCSR (xmt IE)
    a.two("MOV", (7, 3, XCSR), (3, 0))        # MOV @#XCSR, R3
    a.two("MOV", (7, 2, 0o0), (7, 3, RCSR))     # MOV #0, @#RCSR (clear IE)
    a.two("MOV", (7, 3, RCSR), (4, 0))        # MOV @#RCSR, R4
    a.halt()
    a.emit(run=50, dumps=[])
    return 0


if __name__ == "__main__":
    sys.exit(main())
