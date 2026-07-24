# DEC documentation — the timing oracle & architectural reference

Source: `bitsavers.org/pdf/dec/pdp11/`. These are the documents the plan and
`docs/PROJECT_STATUS.md` refer to. SimH is the oracle for architectural *state*;
these manuals are the oracle for *timing* (SimH models none) and the authority
for anything SimH's simplifications don't settle.

> Note: PDFs are large scanned binaries. Keep them here for reference; if the
> repo should stay lean, move to git-lfs or gitignore this folder — decide before
> committing (they total ~100+ MB).

## PDP-11/70 (KB11-C) — the first target
| File | Role |
|------|------|
| `EK-KB11C-TM-001_1170procMan.pdf` | **KB11-C Processor Manual** — flow charts, data paths, and instruction timing. THE cycle-timing oracle (P4). |
| `KB11-C_signalIndex.pdf` | KB11-C signal index — cross-reference for the data-path signals when deriving cycle timing. |
| `EK-11070-MM-002_May79.pdf` | PDP-11/70 Maintenance Manual — cache, Unibus map, Massbus/RH70, system behaviour. |
| `PDP-11_70_Handbook_1977-78.pdf` | PDP-11/70 Handbook — programmer's view, register maps. |
| `EY-D3054-HO-002_45-70hw.pdf` | 11/45–70 hardware course notes. |
| `EK-FP11C-MM-01_FP11C_May76.pdf` | **FP11-C** floating-point manual — formats, exceptions, timing (P5). |

## Architecture / instruction set (all models)
| File | Role |
|------|------|
| `EB-23657-18_PDP-11_Architecture_Handbook_1983.pdf` | Instruction set, addressing modes, PSW, traps — the P1/P2 reference. |
| `PDP11_Instruction_List.pdf` | Quick opcode list. |
| `PDP-11_Programming_Card_Jul75.pdf` | One-page opcode/mode/flag summary. |

## Bus & peripherals
| File | Role |
|------|------|
| `EB-26077-41_PDP-11_UNIBUS_Processor_Handbook_1985.pdf` | Unibus processor handbook. |
| `PDP-11_Bus_Handbook_1979.pdf` | Unibus/Q-bus electrical & protocol spec (arbitration, NPR/BR). |
| `PDP-11_PeripheralsHbk_1976.pdf` | Device register maps: RK11/RK05, RP, DL11, KW11-L, etc. (P6). |

## Still to fetch when their phase arrives
Device-specific EK manuals (RH70, RP04/06, RK11, DL11, KW11-L) and the KT11 MMU
detail live in other bitsavers dirs / within the maintenance manual; pull them at
P6 and add rows here. A bulk mirror of `pdf/dec/pdp11/` also lands under
`roms/bitsavers.org/pdf/` (gitignored).

**Mirror prune (2026-07-25).** The bulk mirror pulled the full `pdf/dec/pdp11/`
tree (145 GB), of which 130 GB was `microfiche/Diagnostic_Program_Listings/
Listings/` — raw scanned MAINDEC listing images, not reference documentation.
That `Listings/` directory was removed, leaving 15 GB of actual manuals,
handbooks, and the smaller `microfiche/{Manuals,Index}`. The KB11-C processor
manual, PDP-11/70 handbook, FP11-C prints, and device manuals are all retained.
Re-run the bitsavers mirror if the listings are ever needed; they are
re-downloadable.
