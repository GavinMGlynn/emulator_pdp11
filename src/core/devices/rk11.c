#include "devices/rk11.h"

#include <stddef.h>

#include "cpu/cpu.h"

// Faithful port of SimH pdp11_rk.c. The data transfer is functional (whole
// words copied between the disk buffer and physical memory); the seek/rotational
// delay is modelled only as the emulated time until DONE is set.

// RKCS bits
#define RKCS_GO   0000001u
#define RKCS_FUNC 0000016u // bits 3:1
#define RKCS_MEX  0000060u // memory-address extension, bits 5:4
#define RKCS_IE   0000100u
#define RKCS_DONE 0000200u
#define RKCS_FMT  0002000u
#define RKCS_INH  0004000u
#define RKCS_SCP  0020000u
#define RKCS_HERR 0040000u
#define RKCS_ERR  0100000u
#define RKCS_REAL 0026776u // bits kept in the register
#define RKCS_RW   0006576u // writable bits
#define RKCS_V_MEX 4

// Functions (RKCS<3:1>)
#define FUNC_CRESET 0
#define FUNC_WRITE  1
#define FUNC_READ   2
#define FUNC_WCHK   3
#define FUNC_SEEK   4
#define FUNC_RCHK   5
#define FUNC_DRESET 6
#define FUNC_WLK    7

// RKER bits
#define RKER_NXS  0000040u // nonexistent sector
#define RKER_NXC  0000100u // nonexistent cylinder
#define RKER_NXD  0000200u // nonexistent drive
#define RKER_NXM  0002000u // nonexistent memory
#define RKER_OVR  0040000u // overrun
#define RKER_HARD 0177740u // hard errors

#define RKBA_IMP  0177776u // implemented RKBA bits (even)

// RKDA fields
#define GET_SECT(x)  ((uint32_t)((x) & 017u))
#define GET_TRACK(x) ((uint32_t)(((x) >> 4) & 0777u))
#define GET_CYL(x)   ((uint32_t)(((x) >> 5) & 0377u))
#define RKDA_DRIVE   0160000u
#define RKDA_V_TRACK 4
#define RKDA_V_SECT  0

// Nominal transfer completion delay (paper-oracle timing; the exact value is not
// architecturally visible). ~1 ms base plus a little per word.
#define RK_BASE_NS 1000000u
#define RK_WORD_NS 100u

static void rk_set_error(pdp11_cpu *cpu, uint16_t err) {
    pdp11_rk11 *rk = &cpu->rk;
    rk->rker |= err;
    rk->rkcs |= RKCS_ERR;
    if (rk->rker & RKER_HARD) {
        rk->rkcs |= RKCS_HERR;
    }
}

// Set DONE and raise/lower the interrupt to match (DONE & IE).
static void rk_finish(pdp11_cpu *cpu) {
    pdp11_rk11 *rk = &cpu->rk;
    rk->busy = false;
    rk->rkcs |= RKCS_DONE;
    if (rk->rkcs & RKCS_IE) {
        pdp11_set_int(cpu, PDP11_INT_RK);
    } else {
        pdp11_clr_int(cpu, PDP11_INT_RK);
    }
}

// Perform the DMA transfer for a read/write/write-check, then update the
// registers exactly as SimH's rk_svc does.
static void rk_transfer(pdp11_cpu *cpu, int func) {
    pdp11_rk11 *rk = &cpu->rk;
    uint32_t ma = (((uint32_t)rk->rkcs & RKCS_MEX) << (16 - RKCS_V_MEX)) | rk->rkba;
    uint32_t da = (GET_TRACK(rk->rkda) * RK_NUMSC + GET_SECT(rk->rkda)) * RK_NUMWD;
    int32_t wc = (int32_t)(0200000u - rk->rkwc); // two's-complement word count

    if (da + (uint32_t)wc > rk->disk_words) { // overrun: trim
        wc = (int32_t)(rk->disk_words - da);
        rk_set_error(cpu, RKER_OVR);
    }
    for (int32_t i = 0; i < wc; ++i) {
        uint32_t mem = (ma + (uint32_t)(i * 2)) & 0777777u; // 18-bit Unibus addr
        if (func == FUNC_READ) {
            pdp11_mem_write_word(cpu->mem, mem, rk->disk[da + (uint32_t)i]);
        } else if (func == FUNC_WRITE) {
            rk->disk[da + (uint32_t)i] = pdp11_mem_read_word(cpu->mem, mem);
        } else { // write-check / read-check: no memory store
            if (func == FUNC_WCHK &&
                pdp11_mem_read_word(cpu->mem, mem) != rk->disk[da + (uint32_t)i]) {
                rk_set_error(cpu, 0000001u); // RKER_WCE (soft)
            }
        }
    }
    // Advance the word count, memory address, and disk address.
    rk->rkwc = (uint16_t)(rk->rkwc + (uint32_t)wc);
    if (!(rk->rkcs & RKCS_INH)) {
        ma = ma + (uint32_t)(wc * 2);
    }
    rk->rkba = (uint16_t)(ma & RKBA_IMP);
    rk->rkcs = (uint16_t)((rk->rkcs & ~RKCS_MEX)
                          | ((ma >> (16 - RKCS_V_MEX)) & RKCS_MEX));
    da = da + (uint32_t)wc + (RK_NUMWD - 1);
    uint32_t track = (da / RK_NUMWD) / RK_NUMSC;
    uint32_t sect = (da / RK_NUMWD) % RK_NUMSC;
    rk->rkda = (uint16_t)((rk->rkda & RKDA_DRIVE) | (track << RKDA_V_TRACK)
                          | (sect << RKDA_V_SECT));
}

// Decode and start a command (RKCS GO with DONE set). Errors finish at once;
// a valid transfer runs functionally now and its DONE is scheduled.
static void rk_go(pdp11_cpu *cpu) {
    pdp11_rk11 *rk = &cpu->rk;
    int func = (rk->rkcs & RKCS_FUNC) >> 1;

    if (func == FUNC_CRESET) { // control reset
        rk->rker = 0;
        rk->rkda = 0;
        rk->rkba = 0;
        rk->rkcs = RKCS_DONE;
        rk->busy = false;
        pdp11_clr_int(cpu, PDP11_INT_RK);
        return;
    }
    rk->rkcs &= (uint16_t)~RKCS_SCP;
    rk->rkcs &= (uint16_t)~RKCS_DONE; // clear done (busy)
    pdp11_clr_int(cpu, PDP11_INT_RK);

    if (rk->disk == NULL || (rk->rkda & RKDA_DRIVE)) { // only drive 0, attached
        rk_set_error(cpu, RKER_NXD);
        rk_finish(cpu);
        return;
    }
    if (func == FUNC_WLK) { // write lock — accepted, nothing to do
        rk_finish(cpu);
        return;
    }
    uint32_t sect = GET_SECT(rk->rkda);
    uint32_t cyl = GET_CYL(rk->rkda);
    if (func == FUNC_DRESET) {
        sect = cyl = 0;
        func = FUNC_SEEK;
    }
    if (sect >= RK_NUMSC) {
        rk_set_error(cpu, RKER_NXS);
        rk_finish(cpu);
        return;
    }
    if (cyl >= RK_NUMCY) {
        rk_set_error(cpu, RKER_NXC);
        rk_finish(cpu);
        return;
    }
    int32_t wc = (int32_t)(0200000u - rk->rkwc);
    if (func == FUNC_READ || func == FUNC_WRITE || func == FUNC_WCHK) {
        rk_transfer(cpu, func);
    }
    // Seek and read/write-check move no data. Schedule the completion.
    rk->busy = true;
    rk->done_ns = cpu->time_ns + RK_BASE_NS + (uint32_t)(wc > 0 ? wc : 0) * RK_WORD_NS;
}

void pdp11_rk_poll(pdp11_cpu *cpu) {
    if (cpu->rk.busy && cpu->time_ns >= cpu->rk.done_ns) {
        rk_finish(cpu);
    }
}

void pdp11_rk_attach(pdp11_cpu *cpu, uint16_t *disk, uint32_t words) {
    cpu->rk.disk = disk;
    cpu->rk.disk_words = words;
}

uint16_t pdp11_rk_read(pdp11_cpu *cpu, uint16_t addr) {
    pdp11_rk11 *rk = &cpu->rk;
    switch (addr) {
    case RK_RKER:
        return rk->rker;
    case RK_RKCS: {
        uint16_t cs = rk->rkcs & RKCS_REAL;
        if (rk->rker) {
            cs |= RKCS_ERR;
        }
        if (rk->rker & RKER_HARD) {
            cs |= RKCS_HERR;
        }
        return cs;
    }
    case RK_RKWC:
        return rk->rkwc;
    case RK_RKBA:
        return (uint16_t)(rk->rkba & RKBA_IMP);
    case RK_RKDA:
        return rk->rkda;
    case RK_RKDS:
        return rk->rkds; // drive status is largely non-deterministic; see notes
    default:
        return 0;
    }
}

void pdp11_rk_write(pdp11_cpu *cpu, uint16_t addr, uint16_t value) {
    pdp11_rk11 *rk = &cpu->rk;
    switch (addr) {
    case RK_RKCS:
        rk->rkcs &= RKCS_REAL;
        if ((value & RKCS_IE) == 0) {
            pdp11_clr_int(cpu, PDP11_INT_RK);
        } else if ((rk->rkcs & (RKCS_DONE | RKCS_IE)) == RKCS_DONE) {
            pdp11_set_int(cpu, PDP11_INT_RK);
        }
        rk->rkcs = (uint16_t)((rk->rkcs & ~RKCS_RW) | (value & RKCS_RW));
        if ((rk->rkcs & RKCS_DONE) && (value & RKCS_GO)) {
            rk_go(cpu);
        }
        break;
    case RK_RKWC:
        rk->rkwc = value;
        break;
    case RK_RKBA:
        rk->rkba = (uint16_t)(value & RKBA_IMP);
        break;
    case RK_RKDA:
        if (rk->rkcs & RKCS_DONE) { // only writable when idle
            rk->rkda = value;
        }
        break;
    default:
        break; // RKDS/RKER read-only; RKMR/RKDB unimplemented
    }
}
