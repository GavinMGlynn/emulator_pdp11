# TEST SOFTWARE — the shelf, organized by subsystem stressed

Real content finds what unit tests can't (emulator-setup-guide.md §11). Boot
these as **integration checks, never goals**. Media lives under `roms/`
(gitignored — large and license-bound).

## Where it comes from

| Source | Reachable from build sandbox | Notes |
|--------|------------------------------|-------|
| `pdp-11.trailing-edge.com` | yes | Classic archive; mirrored to `roms/pdp-11.trailing-edge.com/` |
| `bitsavers.org/bits/DEC/pdp11/` | yes (needs browser User-Agent; default wget UA → 403) | XXDP, Diagnostics, discimages, magtapes, v7m, rt-11, rsx11m, firmware; mirrored to `roms/bitsavers.org/` |
| `opensimh.org/pdp-11_sw/` | yes | Index page; media links out to trailing-edge / tuhs / bitsavers |
| `www.tuhs.org` | yes | Research Unix V6/V7 (Caldera BSD-ish license) |
| `php-11.org.ru` | **NO — does not resolve** in this sandbox | Retry from a host with open DNS |

Licensing: RT-11/RSTS/RSX under DEC hobbyist terms; Unix under the Caldera
"ancient Unix" BSD-ish license. Fine for personal/hobby verification use.

## Shelf by subsystem

| Software | Stresses | Phase it serves |
|----------|----------|-----------------|
| **XXDP + MAINDEC diagnostics** (`bitsavers .../xxdp`, `.../Diagnostics`) | CPU, EIS, traps, MMU, FP11, each device — the hardware's own self-test | P1–P6, the amidog/n64-systemtest analogue; run constantly |
| **RT-11 V5.3** | console, RK/RL disk, line clock — smallest boot | early P6 bring-up |
| **Unix V6 / V7 (V7m)** | MMU, RK/RP disk, DL11 console — first content target | P7 |
| **RSX-11M V4.6** | interrupts/priorities, MMU, multitasking | later P7 |
| **RSTS/E V4B** | timesharing, heavy device + clock use | later P7 |

## Standing rule
Copy protection, region/CIC-style checks, and firmware quirks look like emulator
bugs — characterise against SimH before "fixing". Record every campaign in
`tools/simh-oracle/FINDINGS.md`.
