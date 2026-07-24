# CLAUDE.md — working conventions

Cycle-accurate PDP-11 emulator. 11/70 (KB11-C) first, broadened to SimH's full
model range. Principles from `../emulator-setup-guide.md`; this file is the
short operational version.

## The two oracles (internalise this)
- **Architectural state → SimH** (`ext/simh`, MIT/X11). Registers, PSW, memory,
  device behaviour. Drive it via `tools/simh-oracle/run_oracle.py`.
- **Cycle timing → DEC docs** (KB11-C tables/flows). SimH models *no* timing and
  the 11/70 has no software cycle counter, so timing is a **paper oracle**:
  every cycle number cites its DEC source in `tools/simh-oracle/FINDINGS.md`.

## Discipline
- **Reference-first.** Resolve doubt from SimH source or DEC docs — never
  trial-and-error on our own parameters. Characterise a discrepancy before fixing.
- **Complete modules, don't chase the PC.** Finish one subsystem with tests
  before moving on. Boots (XXDP, Unix) are thermometers, not milestones.
- **One item at a time, landing with its test.** Keep `ctest` green — a red tree
  is the stop-everything condition.
- **Test behaviour as hardware facts**, named as sentences
  (`test_a_word_write_to_an_odd_address_faults`).
- **Measure, don't guess** — timing from DEC tables, on the release build only.
  Placeholder numbers are marked PROVISIONAL in code *and* status doc.
- **Verify on real output** — booted machine / captured TTY, diffed vs SimH.
- **Temporary instrumentation is always reverted** before commit (ours *and* the
  SimH checkout) — edit-revert-restore, never `git checkout` over live work.
- **Optimization only under an identity harness** — probe goldens + long-run
  state hash, byte-identical or it doesn't ship.

## Every commit that lands an item updates both living docs in the same commit
`docs/PROJECT_STATUS.md` (what now works + its verification) and
`docs/COMPLETION_PLAN.md` (tick the item; add tails discovered while implementing).
Co-author trailer on commits; PRs via the platform CLI.

## Build & test
```
cmake --preset linux-debug   && cmake --build --preset linux-debug
ctest --preset linux-debug                 # must be green
ctest --preset linux-release               # goldens must be bit-identical
```
Refresh a golden from the oracle:
```
python3 tools/simh-oracle/run_oracle.py --simh ext/simh/BIN/pdp11 \
    --image tests/images/<name>.image > tests/goldens/<name>.golden
```

## Layout
`src/core` is a static lib with **zero** frontend deps (one dir per subsystem:
`cpu memory mmu cache fp11 unibus massbus clk console devices bus`). Frontends
(`headless`, `sdl`) depend on the core, never the reverse. `ext/` (Unity, SDL3,
SimH) never gets our warning set. `roms/` and `images/` are gitignored media.
