#!/usr/bin/env python3
"""Drive SimH as the architectural oracle (emulator-setup-guide.md §8).

Reads the same plain-text image the headless frontend consumes, generates a
deterministic SimH command script (DEPOSIT / STEP / EXAMINE), runs the vendored
`pdp11` simulator, and emits the canonical state dump on stdout in exactly the
format `pdp11_headless` writes — so the two diff byte-for-byte. Use it to mint
or refresh goldens under tests/goldens/.

Usage:
    run_oracle.py --simh ext/simh/BIN/pdp11 --image tests/images/add3.image \\
        [--model 11/70] [--mem 256k] > tests/goldens/add3.golden
"""
import argparse
import re
import subprocess
import sys
import tempfile


def parse_image(path):
    """Return (deposits, pc, run_limit, dump_regions) from an image file."""
    deposits, pc, run_limit, dumps = [], 0, 0, []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            op = parts[0]
            if op == "w" and len(parts) == 3:
                deposits.append((int(parts[1], 8), int(parts[2], 8)))
            elif op == "pc" and len(parts) >= 2:
                pc = int(parts[1], 8)
            elif op == "run" and len(parts) >= 2:
                run_limit = int(parts[1], 10)
            elif op == "dump" and len(parts) == 3:
                dumps.append((int(parts[1], 8), int(parts[2], 8)))
    return deposits, pc, run_limit, dumps


def build_script(deposits, pc, run_limit, dumps, model, mem):
    lines = [f"set cpu {model}", f"set cpu {mem}"]
    for addr, val in deposits:
        lines.append(f"d {addr:o} {val:o}")
    lines.append(f"d pc {pc:o}")
    lines.append(f"step {run_limit}")
    for reg in ("r0", "r1", "r2", "r3", "r4", "r5", "sp", "pc", "psw"):
        lines.append(f"e {reg}")
    for addr, count in dumps:
        hi = addr + (count - 1) * 2
        lines.append(f"e {addr:o}-{hi:o}")
    lines.append("quit")
    return "\n".join(lines) + "\n"


# SimH register name (as printed) -> our canonical register label.
REG_LABEL = {"R0": "R0", "R1": "R1", "R2": "R2", "R3": "R3", "R4": "R4",
             "R5": "R5", "SP": "R6", "PC": "R7", "PSW": "PSW"}

_REG_RE = re.compile(r"^(R[0-7]|SP|PC|PSW):\s+([0-7]+)")
_MEM_RE = re.compile(r"^([0-7]+):\s+([0-7]+)")


def parse_simh_output(text):
    """Turn SimH EXAMINE output into canonical dump lines."""
    regs, mem = {}, []
    for line in text.splitlines():
        m = _REG_RE.match(line)
        if m:
            regs[m.group(1)] = int(m.group(2), 8)
            continue
        m = _MEM_RE.match(line)
        if m:
            mem.append((int(m.group(1), 8), int(m.group(2), 8)))

    out = []
    for name in ("R0", "R1", "R2", "R3", "R4", "R5", "SP", "PC"):
        out.append(f"{REG_LABEL[name]} {regs.get(name, 0):06o}")
    out.append(f"PSW {regs.get('PSW', 0):06o}")
    for addr, val in mem:
        out.append(f"M {addr:06o} {val:06o}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--simh", required=True, help="path to the pdp11 binary")
    ap.add_argument("--image", required=True)
    ap.add_argument("--model", default="11/70")
    ap.add_argument("--mem", default="256k")
    args = ap.parse_args()

    deposits, pc, run_limit, dumps = parse_image(args.image)
    script = build_script(deposits, pc, run_limit, dumps, args.model, args.mem)

    with tempfile.NamedTemporaryFile("w", suffix=".ini", delete=False) as tf:
        tf.write(script)
        ini = tf.name

    result = subprocess.run([args.simh, ini], capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        return 1

    for line in parse_simh_output(result.stdout):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
