#include "devices/rp11.h"

#include <stddef.h>

#include "cpu/cpu.h"

// Functional port of SimH pdp11_rh.c (RH70 Massbus adapter) + pdp11_rp.c (RP04).
// The register file is addressed through SimH's mba_mapofs; the data transfer is
// whole-word DMA between the disk buffer and physical memory.

// CS1 bits (RH70)
#define CS1_GO   0000001u
#define CS1_FNC  0000076u // function, bits 5:1
#define CS1_IE   0000100u
#define CS1_DONE 0000200u // "ready"
#define CS1_UAE  0001400u // Unibus addr ext, bits 9:8 (superseded by BAE)
#define CS1_DVA  0004000u // drive available
#define CS1_TRE  0040000u // transfer error
#define CS1_SC   0100000u // special condition
#define CS1_RW   0000076u // writable function bits
#define GET_FNC(c) (((c) >> 1) & 037u)

// Functions (CS1<5:1>)
#define FNC_DCLR   0004u // drive clear
#define FNC_PRESET 0010u // read-in preset
#define FNC_PACK   0011u // pack acknowledge
#define FNC_SEARCH 0014u
#define FNC_WCHK   0024u
#define FNC_WRITE  0030u
#define FNC_READ   0034u

// CS2 bits
#define CS2_UAI 0000010u // address inhibit
#define CS2_CLR 0000040u // controller clear

// DS bits (drive status)
#define DS_VV  0000100u // volume valid
#define DS_RDY 0000200u // drive ready
#define DS_DPR 0000400u // drive present
#define DS_MOL 0010000u // medium online
#define DS_ATA 0100000u // attention active

// DA/DC fields
#define GET_SC(x) ((uint32_t)((x) & 077u))       // sector
#define GET_SF(x) ((uint32_t)(((x) >> 8) & 077u)) // track (surface)
#define GET_CY(x) ((uint32_t)((x) & 01777u))      // cylinder

#define RP04_DT 020020u // RP04 drive-type value

// SimH mba_mapofs: I/O word offset (0-31 from RP_CSR) -> register selector.
// High bit = external (drive) register, low 5 bits = register number.
#define RH_EXT 0x100
static const int rp_mapofs[32] = {
    0x000, 0x001, 0x002, RH_EXT | 5, 0x003, RH_EXT | 1, RH_EXT | 2, RH_EXT | 4,
    RH_EXT | 7, 0x004, RH_EXT | 3, RH_EXT | 6, RH_EXT | 8, RH_EXT | 9,
    RH_EXT | 10, RH_EXT | 11, RH_EXT | 12, RH_EXT | 13, RH_EXT | 14, RH_EXT | 15,
    RH_EXT | 16, RH_EXT | 17, RH_EXT | 18, RH_EXT | 19, RH_EXT | 20, RH_EXT | 21,
    RH_EXT | 22, RH_EXT | 23, RH_EXT | 24, RH_EXT | 25, RH_EXT | 26, RH_EXT | 27,
};

// Nominal completion delay (paper-oracle timing; not architecturally visible).
#define RP_BASE_NS 2000000u
#define RP_WORD_NS 100u

static void rp_finish(pdp11_cpu *cpu) {
    pdp11_rp11 *rp = &cpu->rp;
    rp->busy = false;
    rp->cs1 |= CS1_DONE;
    rp->ds |= DS_ATA;
    if (rp->cs1 & CS1_IE) {
        pdp11_set_int(cpu, PDP11_INT_RP);
    } else {
        pdp11_clr_int(cpu, PDP11_INT_RP);
    }
}

// The current drive-status word: present + online + (valid) + ready when idle.
static uint16_t rp_drive_status(const pdp11_rp11 *rp) {
    uint16_t ds = rp->ds | DS_DPR;
    if (rp->disk) {
        ds |= DS_MOL;
        if (!rp->busy) {
            ds |= DS_RDY;
        }
    }
    return ds;
}

static void rp_transfer(pdp11_cpu *cpu, int func) {
    pdp11_rp11 *rp = &cpu->rp;
    uint32_t ma = (((uint32_t)rp->bae & 077u) << 16) | rp->ba; // 22-bit address
    uint32_t da = ((GET_CY(rp->dc) * RP_SURF + GET_SF(rp->da)) * RP_SECT
                   + GET_SC(rp->da)) * RP_NUMWD;
    int32_t wc = (int32_t)(0200000u - rp->wc);

    if (da >= rp->disk_words) {
        wc = 0;
    } else if (da + (uint32_t)wc > rp->disk_words) {
        wc = (int32_t)(rp->disk_words - da);
    }
    for (int32_t i = 0; i < wc; ++i) {
        uint32_t mem = ma & 03777777u; // 22-bit physical
        if (func == FNC_READ) {
            pdp11_mem_write_word(cpu->mem, mem, rp->disk[da + (uint32_t)i]);
        } else if (func == FNC_WRITE) {
            rp->disk[da + (uint32_t)i] = pdp11_mem_read_word(cpu->mem, mem);
        }
        if (!(rp->cs2 & CS2_UAI)) {
            ma += 2;
        }
    }
    rp->wc = (uint16_t)(rp->wc + (uint32_t)wc);
    rp->ba = (uint16_t)(ma & 0177777u);
    rp->bae = (uint16_t)((ma >> 16) & 077u);
    // Advance the disk address by the sectors transferred.
    uint32_t blk = (da / RP_NUMWD) + ((uint32_t)wc + RP_NUMWD - 1) / RP_NUMWD;
    uint32_t sc = blk % RP_SECT;
    uint32_t sf = (blk / RP_SECT) % RP_SURF;
    uint32_t cy = (blk / RP_SECT) / RP_SURF;
    rp->da = (uint16_t)((sf << 8) | sc);
    rp->dc = (uint16_t)cy;
    rp->cc = (uint16_t)cy;
}

static void rp_go(pdp11_cpu *cpu) {
    pdp11_rp11 *rp = &cpu->rp;
    int func = GET_FNC(rp->cs1);

    rp->cs1 &= (uint16_t)~CS1_DONE; // clear ready (busy)
    pdp11_clr_int(cpu, PDP11_INT_RP);

    switch (func) {
    case FNC_DCLR: // drive clear
        rp->er1 = 0;
        rp->ds &= (uint16_t)~DS_ATA;
        rp_finish(cpu);
        return;
    case FNC_PRESET:
    case FNC_PACK: // pack acknowledge -> volume valid
        rp->ds |= DS_VV;
        rp_finish(cpu);
        return;
    case FNC_SEARCH: // seek/search: reposition only
        rp->cc = rp->dc;
        rp->busy = true;
        rp->done_ns = cpu->time_ns + RP_BASE_NS;
        return;
    case FNC_READ:
    case FNC_WRITE:
    case FNC_WCHK: {
        int32_t wc = (int32_t)(0200000u - rp->wc);
        if (func != FNC_WCHK) {
            rp_transfer(cpu, func);
        }
        rp->busy = true;
        rp->done_ns =
            cpu->time_ns + RP_BASE_NS + (uint32_t)(wc > 0 ? wc : 0) * RP_WORD_NS;
        return;
    }
    default: // other functions complete immediately
        rp_finish(cpu);
        return;
    }
}

void pdp11_rp_poll(pdp11_cpu *cpu) {
    if (cpu->rp.busy && cpu->time_ns >= cpu->rp.done_ns) {
        rp_finish(cpu);
    }
}

void pdp11_rp_attach(pdp11_cpu *cpu, uint16_t *disk, uint32_t words) {
    cpu->rp.disk = disk;
    cpu->rp.disk_words = words;
}

// Map an I/O address to its register selector, or -1 if outside the window.
static int rp_reg_sel(uint16_t addr) {
    if (addr < RP_CSR || addr > RP_END) {
        return -1;
    }
    return rp_mapofs[(addr - RP_CSR) >> 1];
}

uint16_t pdp11_rp_read(pdp11_cpu *cpu, uint16_t addr) {
    pdp11_rp11 *rp = &cpu->rp;
    if (addr == 0176750u) { // RPBAE (RH70 bus-address extension)
        return (uint16_t)(rp->bae & 077u);
    }
    if (addr == 0176752u) { // RPCS3 (RH70) — minimal
        return 0;
    }
    int sel = rp_reg_sel(addr);
    if (sel < 0) {
        return 0;
    }
    if (sel & RH_EXT) { // drive register
        switch (sel & 0xFF) {
        case 1:  return rp_drive_status(rp); // RPDS
        case 2:  return rp->er1;             // RPER1
        case 5:  return rp->da;              // RPDA
        case 6:  return RP04_DT;             // RPDT
        case 10: return rp->dc;              // RPDC
        case 11: return rp->cc;              // RPCC
        default: return 0;
        }
    }
    switch (sel) { // RH70 register
    case 0:  return (uint16_t)(rp->cs1 | CS1_DVA); // CS1 (drive available)
    case 1:  return rp->wc;
    case 2:  return rp->ba;
    case 3:  return rp->cs2;
    case 5:  return rp->bae;
    default: return 0;
    }
}

void pdp11_rp_write(pdp11_cpu *cpu, uint16_t addr, uint16_t value) {
    pdp11_rp11 *rp = &cpu->rp;
    if (addr == 0176750u) { // RPBAE (RH70 bus-address extension)
        rp->bae = (uint16_t)(value & 077u);
        return;
    }
    if (addr == 0176752u) { // RPCS3 (RH70) — minimal
        return;
    }
    int sel = rp_reg_sel(addr);
    if (sel < 0) {
        return;
    }
    if (sel & RH_EXT) { // drive register (writable subset)
        switch (sel & 0xFF) {
        case 5:  rp->da = (uint16_t)(value & ~0140300u); break; // RPDA (DA_MBZ)
        case 10: rp->dc = (uint16_t)(value & ~0176000u); break; // RPDC (DC_MBZ)
        default: break;                                         // read-only here
        }
        return;
    }
    switch (sel) { // RH70 register
    case 0: // CS1
        if ((value & CS1_IE) == 0) {
            pdp11_clr_int(cpu, PDP11_INT_RP);
        } else if ((rp->cs1 & (CS1_DONE | CS1_IE)) == CS1_DONE) {
            pdp11_set_int(cpu, PDP11_INT_RP);
        }
        rp->cs1 = (uint16_t)((rp->cs1 & ~(CS1_RW | CS1_IE))
                             | (value & (CS1_RW | CS1_IE)));
        if ((rp->cs1 & CS1_DONE) && (value & CS1_GO)) {
            rp_go(cpu);
        }
        break;
    case 1: rp->wc = value; break;
    case 2: rp->ba = value; break;
    case 3: // CS2
        if (value & CS2_CLR) { // controller clear
            rp->cs1 = CS1_DONE;
            rp->er1 = 0;
            rp->busy = false;
            pdp11_clr_int(cpu, PDP11_INT_RP);
        }
        rp->cs2 = value;
        break;
    case 5: rp->bae = (uint16_t)(value & 077u); break;
    default: break;
    }
}
