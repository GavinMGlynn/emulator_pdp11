#include "devices/rl11.h"

#include <stddef.h>

#include "cpu/cpu.h"

// Port of SimH pdp11_rl.c (register + command semantics). The data transfer is
// functional (whole words copied between the disk buffer and physical memory);
// seek/rotational delay is modelled only as the emulated time until DONE. Unlike
// SimH we treat the drive as always spun-up (lock-on) rather than modelling the
// multi-second load/spin/brush/lock state machine — the data path is identical.

// RLCS control/status bits
#define RLCS_DRDY   0000001u // drive ready
#define RLCS_FUNC   0000016u // function, bits 3:1
#define RLCS_V_FUNC 1
#define RLCS_MEX    0000060u // bus-address bits 17:16, bits 5:4
#define RLCS_V_MEX  4
#define RLCS_IE     0000100u // interrupt enable
#define RLCS_DONE   0000200u // controller ready (DONE)
#define RLCS_INCMP  0002000u // operation incomplete
#define RLCS_CRC    0004000u // CRC / data error
#define RLCS_DLT    0010000u // data late
#define RLCS_NXM    0020000u // non-existent memory
#define RLCS_DRE    0040000u // drive error
#define RLCS_ERR    0100000u // error summary
#define RLCS_RW     0001776u // program-writable bits
#define RLCS_ALLERR (RLCS_ERR | RLCS_DRE | RLCS_NXM | RLCS_DLT | RLCS_CRC | RLCS_INCMP)

// Function codes (RLCS<3:1>)
#define FUNC_NOP    0
#define FUNC_WCHK   1
#define FUNC_GSTA   2
#define FUNC_SEEK   3
#define FUNC_RHDR   4
#define FUNC_WRITE  5
#define FUNC_READ   6
#define FUNC_RNOHDR 7

// RLDA disk-address fields
#define RLDA_M_SECT  077u
#define RLDA_V_TRACK 6
#define RLDA_M_TRACK 01777u
#define RLDA_V_CYL   7
#define RLDA_M_CYL   0777u
#define RLDA_GS      0000002u // marker: get-status
#define RLDA_SK_DIR  0000004u // seek direction (1 = out)
#define RLDA_GS_CLR  0000010u // get-status: clear errors
#define RLDA_SK_HD   0000020u // seek head select
#define RLDA_HD1     (1u << RLDA_V_TRACK)

// RLDS drive-status bits reported by GET STATUS for a ready drive
#define RLDS_LOCK  5u        // lock-on (ready) state
#define RLDS_BHO   0000010u  // brushes home
#define RLDS_HDO   0000020u  // heads out
#define RLDS_HD    0000100u  // head select (reflects TRK)
#define RLDS_RL02  0000200u  // this is an RL02

#define GET_SECT(x)  ((uint32_t)((x) & RLDA_M_SECT))
#define GET_TRACK(x) ((uint32_t)(((x) >> RLDA_V_TRACK) & RLDA_M_TRACK))
#define GET_CYL(x)   ((uint32_t)(((x) >> RLDA_V_CYL) & RLDA_M_CYL))

// Nominal completion delay (paper-oracle; not architecturally visible).
#define RL_BASE_NS 2000000u
#define RL_WORD_NS 100u

static bool rl_is_rl02(const pdp11_rl11 *rl) { return rl->disk_words > RL01_WORDS; }

// Set DONE and raise/lower the interrupt to match (DONE & IE), OR-ing any error.
static void rl_finish(pdp11_cpu *cpu, uint16_t status) {
    pdp11_rl11 *rl = &cpu->rl;
    rl->busy = false;
    rl->rlcs |= status | RLCS_DONE | RLCS_DRDY;
    if (status & (RLCS_INCMP | RLCS_CRC | RLCS_DLT | RLCS_NXM | RLCS_DRE)) {
        rl->rlcs |= RLCS_ERR;
    }
    if (rl->rlcs & RLCS_IE) {
        pdp11_set_int(cpu, PDP11_INT_RL);
    } else {
        pdp11_clr_int(cpu, PDP11_INT_RL);
    }
}

// The DMA transfer for READ / RNOHDR / WRITE / WCHK. Returns any error status.
static uint16_t rl_transfer(pdp11_cpu *cpu, int func) {
    pdp11_rl11 *rl = &cpu->rl;
    // 18-bit Unibus address: RLCS<5:4> are bits 17:16, relocated through the
    // Unibus Map (the Qbus RLV12's 22-bit RLBAE path is a Qbus-model tail).
    uint32_t ma = (((uint32_t)rl->rlcs & RLCS_MEX) << (16 - RLCS_V_MEX)) | rl->rlba;
    uint32_t da = ((GET_TRACK(rl->rlda) * RL_NUMSC) + GET_SECT(rl->rlda)) * RL_NUMWD;
    int32_t wc = (int32_t)(0200000u - rl->rlmp); // two's-complement word count
    uint16_t status = 0;

    // A transfer may not cross a track boundary; SimH trims to the track end.
    uint32_t maxwc = (RL_NUMSC - GET_SECT(rl->rlda)) * RL_NUMWD;
    if ((uint32_t)wc > maxwc) {
        wc = (int32_t)maxwc;
    }
    if (da + (uint32_t)wc > rl->disk_words) { // off the end of the drive
        wc = (int32_t)(rl->disk_words > da ? rl->disk_words - da : 0);
        status |= RLCS_ERR | RLCS_INCMP;
    }
    for (int32_t i = 0; i < wc; ++i) {
        // The RL is an 18-bit Unibus device (RLV12 extends to 22); its DMA
        // address relocates through the Unibus Map (identity when disabled).
        uint32_t mem = pdp11_unibus_map(cpu, ma + (uint32_t)(i * 2));
        if (func == FUNC_READ || func == FUNC_RNOHDR) {
            pdp11_mem_write_word(cpu->mem, mem, rl->disk[da + (uint32_t)i]);
        } else if (func == FUNC_WRITE) {
            rl->disk[da + (uint32_t)i] = pdp11_mem_read_word(cpu->mem, mem);
        } else if (func == FUNC_WCHK
                   && pdp11_mem_read_word(cpu->mem, mem) != rl->disk[da + (uint32_t)i]) {
            status |= RLCS_ERR | RLCS_CRC; // write-check mismatch
        }
    }
    // Advance the word count, memory address, and disk address.
    rl->rlmp = (uint16_t)(rl->rlmp + (uint32_t)wc); // 0 when the request completed
    if (rl->rlmp != 0) {
        status |= RLCS_ERR | RLCS_INCMP;
    }
    ma += (uint32_t)(wc * 2);
    rl->rlba = (uint16_t)(ma & 0177777u);
    rl->rlcs = (uint16_t)((rl->rlcs & ~RLCS_MEX)
                          | (((ma >> 16) & 03u) << RLCS_V_MEX));
    // Advance RLDA by the sectors transferred; keep it as the read/write format.
    rl->rlda = (uint16_t)(rl->rlda + ((wc + (int32_t)RL_NUMWD - 1) / (int32_t)RL_NUMWD));
    rl->trk = rl->rlda;
    return status;
}

// Decode and start a command (a write to RLCS with DONE clear / GO implied).
static void rl_go(pdp11_cpu *cpu) {
    pdp11_rl11 *rl = &cpu->rl;
    int func = (rl->rlcs & RLCS_FUNC) >> RLCS_V_FUNC;

    rl->rlcs &= (uint16_t)~RLCS_DONE; // busy
    pdp11_clr_int(cpu, PDP11_INT_RL);

    if (rl->disk == NULL) { // no drive attached -> drive error
        rl_finish(cpu, RLCS_INCMP | RLCS_DRE);
        return;
    }

    switch (func) {
    case FUNC_NOP:
        rl_finish(cpu, 0);
        return;
    case FUNC_GSTA: { // get status -> RLMP = drive status
        if (!(rl->rlda & RLDA_GS)) { // the GS marker must be set
            rl_finish(cpu, RLCS_INCMP);
            return;
        }
        uint16_t ds = (uint16_t)(RLDS_LOCK | RLDS_BHO | RLDS_HDO
                                 | (rl->trk & RLDS_HD));
        if (rl_is_rl02(rl)) {
            ds |= RLDS_RL02;
        }
        rl->rlmp = ds;
        rl_finish(cpu, 0);
        return;
    }
    case FUNC_SEEK: { // move the positioner by RLDA's difference/direction
        uint32_t curr = GET_CYL(rl->trk);
        uint32_t offs = GET_CYL(rl->rlda);
        uint32_t maxc = rl_is_rl02(rl) ? RL_NUMCY * 2 : RL_NUMCY;
        int32_t newc = (rl->rlda & RLDA_SK_DIR) ? (int32_t)(curr + offs)
                                                : (int32_t)(curr - offs);
        if (newc >= (int32_t)maxc) {
            newc = (int32_t)maxc - 1;
        }
        if (newc < 0) {
            newc = 0;
        }
        rl->trk = (uint16_t)(((uint32_t)newc << RLDA_V_CYL)
                             | ((rl->rlda & RLDA_SK_HD) ? RLDA_HD1 : 0u));
        break; // schedule completion
    }
    case FUNC_RHDR: // read header -> RLMP = current position
        rl->rlmp = rl->trk;
        break;
    case FUNC_READ:
    case FUNC_RNOHDR:
    case FUNC_WRITE:
    case FUNC_WCHK: {
        uint16_t st = rl_transfer(cpu, func);
        rl->busy = true;
        int32_t wc = (int32_t)(0200000u - rl->rlmp);
        rl->done_ns = cpu->time_ns + RL_BASE_NS + (uint32_t)(wc > 0 ? wc : 0) * RL_WORD_NS;
        rl->rlcs |= (uint16_t)(st & ~RLCS_DONE); // remember error; DONE at poll
        return;
    }
    default:
        rl_finish(cpu, RLCS_INCMP);
        return;
    }
    // Seek / read-header move no data. Schedule the completion.
    rl->busy = true;
    rl->done_ns = cpu->time_ns + RL_BASE_NS;
}

uint16_t pdp11_rl_read(pdp11_cpu *cpu, uint16_t addr) {
    pdp11_rl11 *rl = &cpu->rl;
    switch (addr) {
    case RL_RLCS:
        // DRDY reflects the drive: ready only when attached, locked-on (not
        // seeking/transferring). Refresh the error-summary bit too (SimH rl_rd).
        if (rl->disk != NULL && !rl->busy) {
            rl->rlcs |= RLCS_DRDY;
        } else {
            rl->rlcs &= (uint16_t)~RLCS_DRDY;
        }
        if (rl->rlcs & (RLCS_DRE | RLCS_NXM | RLCS_DLT | RLCS_CRC | RLCS_INCMP)) {
            rl->rlcs |= RLCS_ERR;
        }
        return rl->rlcs;
    case RL_RLBA:  return rl->rlba;
    case RL_RLDA:  return rl->rlda;
    case RL_RLMP:  return rl->rlmp;
    default:       return 0;
    }
}

void pdp11_rl_write(pdp11_cpu *cpu, uint16_t addr, uint16_t value) {
    pdp11_rl11 *rl = &cpu->rl;
    switch (addr) {
    case RL_RLCS:
        rl->rlcs = (uint16_t)((rl->rlcs & ~RLCS_RW) | (value & RLCS_RW));
        rl->rlbae = (uint16_t)((rl->rlbae & ~03u) | ((rl->rlcs >> RLCS_V_MEX) & 03u));
        // A write with DONE (bit 7) SET is not a command — just re-evaluate the
        // interrupt (SimH: enabling IE while DONE is set raises it).
        if (value & RLCS_DONE) {
            if (!(value & RLCS_IE)) {
                pdp11_clr_int(cpu, PDP11_INT_RL);
            } else if (rl->rlcs & RLCS_DONE) {
                pdp11_set_int(cpu, PDP11_INT_RL);
            }
            return;
        }
        // DONE clear = GO: clear the interrupt and prior errors, run the command.
        pdp11_clr_int(cpu, PDP11_INT_RL);
        rl->rlcs &= (uint16_t)~RLCS_ALLERR;
        rl_go(cpu);
        return;
    case RL_RLBA:  rl->rlba = (uint16_t)(value & 0177776u); return; // even
    case RL_RLDA:  rl->rlda = value; return;
    case RL_RLMP:  rl->rlmp = value; return;
    default:       return;
    }
}

void pdp11_rl_attach(pdp11_cpu *cpu, uint16_t *disk, uint32_t words) {
    cpu->rl.disk = disk;
    cpu->rl.disk_words = words;
}

void pdp11_rl_poll(pdp11_cpu *cpu) {
    if (cpu->rl.busy && cpu->time_ns >= cpu->rl.done_ns) {
        rl_finish(cpu, 0);
    }
}
