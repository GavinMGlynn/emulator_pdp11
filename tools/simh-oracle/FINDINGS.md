# FINDINGS — oracle campaigns

One row per probe/verification campaign: ours vs the oracle (SimH for
architectural state; DEC docs for timing), status, and the story. Nothing is
"fixed" on reasoning alone (emulator-setup-guide.md §8).

Status values: `open` · `confirmed` (matches oracle) · `fixed` · `divergence`
(deliberate, hardware-truer than the oracle — cite evidence) · `deferred`.

| Campaign | Ours | Oracle | Status | Notes |
|----------|------|--------|--------|-------|
| `add3` first-slice (MOV/ADD/HALT, immediate mode) | R0=3, PC=001012, PSW=0 | SimH 11/70 identical | confirmed | The whole harness loop proven end-to-end: `tools/simh-oracle/run_oracle.py` → golden `tests/goldens/add3.golden`, diffed by `regress.py` in CTest. |

## Timing (DEC paper oracle) — none yet
Timing campaigns begin at P4. Each row will cite the KB11-C manual page/table it
derives from, since SimH cannot verify cycle counts.
