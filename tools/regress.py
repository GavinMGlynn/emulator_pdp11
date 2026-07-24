#!/usr/bin/env python3
"""No-reference golden regression checker (emulator-setup-guide.md §8).

Runs the headless frontend on an image and diffs its canonical state dump
against a checked-in golden. The golden is produced from the SimH oracle by
tools/simh-oracle/run_oracle.py; here we only compare, so CTest needs no oracle
and no copyrighted media.
"""
import argparse
import difflib
import subprocess
import sys


def normalize(text):
    """Canonical form: strip trailing whitespace, drop blank lines."""
    return [line.rstrip() for line in text.splitlines() if line.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--headless", required=True, help="path to pdp11_headless")
    ap.add_argument("--image", required=True, help="image file to run")
    ap.add_argument("--golden", required=True, help="expected canonical dump")
    args = ap.parse_args()

    result = subprocess.run(
        [args.headless, args.image], capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(
            f"headless exited {result.returncode}\n{result.stderr}")
        return 1

    got = normalize(result.stdout)
    with open(args.golden, encoding="utf-8") as f:
        want = normalize(f.read())

    if got == want:
        return 0

    sys.stderr.write(f"golden mismatch for {args.image}\n")
    diff = difflib.unified_diff(
        want, got, fromfile="golden", tofile="headless", lineterm="")
    for line in diff:
        sys.stderr.write(line + "\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
