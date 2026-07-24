#include "devices/tm11.h"

#include <stddef.h>

#include "cpu/cpu.h"

// Functional port of SimH pdp11_tm.c. The tape image is the SimH .tap format;
// the data transfer is a functional byte-DMA between the record and memory.

// MTC (command) bits
#define MTC_GO   0000001u
#define MTC_FNC  0000016u // function, bits 3:1
#define MTC_EMA  0000060u // ext mem address, bits 5:4
#define MTC_IE   0000100u
#define MTC_DONE 0000200u
#define MTC_INIT 0010000u
#define MTC_ERR  0100000u
#define MTC_RW   0166576u // writable bits (DEN|LPAR|UNIT|IE|EMA|FNC)
#define GET_FNC(c) (((c) >> 1) & 07u)
#define GET_EMA(c) (((uint32_t)((c) & MTC_EMA)) << (16 - 4))

// Functions (MTC<3:1>)
#define FNC_UNLOAD 00u
#define FNC_READ   01u
#define FNC_WRITE  02u
#define FNC_WREOF  03u
#define FNC_SPACEF 04u
#define FNC_SPACER 05u
#define FNC_WREXT  06u
#define FNC_REWIND 07u

// MTS (status) bits
#define STA_TUR 0000001u // unit ready
#define STA_REW 0000002u // rewinding
#define STA_WLK 0000004u // write locked
#define STA_BOT 0000040u // start of tape
#define STA_ONL 0000100u // online
#define STA_NXM 0000200u // nonexistent memory
#define STA_RLE 0001000u // record length error
#define STA_EOT 0002000u // end of tape
#define STA_EOF 0040000u // file mark
#define STA_ILL 0100000u // illegal
#define STA_DYN (STA_EOF | STA_EOT | STA_ONL | STA_BOT | STA_WLK | STA_REW | STA_TUR)
#define STA_EFLGS (STA_ILL | STA_EOF | STA_RLE | STA_NXM)

#define TM_BASE_NS 4000000u
#define TM_BYTE_NS 100u

// ---- byte access to physical memory (tape transfers are byte streams) -------
// The TM11 is an 18-bit Unibus device: its DMA address relocates through the
// Unibus Map to physical (identity when the map is disabled).
static uint8_t mem_read_byte(pdp11_cpu *cpu, uint32_t a) {
    uint32_t pa = pdp11_unibus_map(cpu, a);
    uint16_t w = pdp11_mem_read_word(cpu->mem, pa & ~1u);
    return (uint8_t)((pa & 1u) ? (w >> 8) : (w & 0377u));
}
static void mem_write_byte(pdp11_cpu *cpu, uint32_t a, uint8_t b) {
    uint32_t pa = pdp11_unibus_map(cpu, a);
    uint16_t w = pdp11_mem_read_word(cpu->mem, pa & ~1u);
    if (pa & 1u) {
        w = (uint16_t)((w & 0377u) | ((unsigned)b << 8));
    } else {
        w = (uint16_t)((w & 0177400u) | (unsigned)b);
    }
    pdp11_mem_write_word(cpu->mem, pa & ~1u, w);
}

// ---- .tap image helpers -----------------------------------------------------
// Little-endian 32-bit length at the image position, or 0xFFFFFFFF past the end.
static uint32_t tap_len_at(const pdp11_tm11 *tm, uint32_t at) {
    if (at + 4 > tm->tape_len) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)tm->tape[at] | ((uint32_t)tm->tape[at + 1] << 8)
           | ((uint32_t)tm->tape[at + 2] << 16) | ((uint32_t)tm->tape[at + 3] << 24);
}

// The tape is at end-of-medium when positioned at/after the last byte.
static bool tm_len_at_is_eom(const pdp11_tm11 *tm) {
    return tm->tape && tm->pos >= tm->tape_len;
}

static void tm_set_done(pdp11_cpu *cpu) {
    pdp11_tm11 *tm = &cpu->tm;
    tm->busy = false;
    tm->cmd |= MTC_DONE;
    if (tm->cmd & MTC_IE) {
        pdp11_set_int(cpu, PDP11_INT_TM);
    } else {
        pdp11_clr_int(cpu, PDP11_INT_TM);
    }
}

// Assemble MTS with the dynamic bits, as SimH's tm_updcsta does.
static uint16_t tm_status(const pdp11_tm11 *tm) {
    uint16_t s = tm->sta & ~STA_DYN;
    if (tm->tape) {
        s |= STA_ONL;
        if (tm->pos == 0) {
            s |= STA_BOT;
        }
        if (tm->wrp) {
            s |= STA_WLK;
        }
        if (tm_len_at_is_eom(tm)) {
            s |= STA_EOT;
        }
    }
    if (!tm->busy) {
        s |= STA_TUR;
    }
    return s;
}

// Execute a tape command (record read/write/space/rewind), functionally.
static void tm_execute(pdp11_cpu *cpu, int func) {
    pdp11_tm11 *tm = &cpu->tm;
    uint32_t xma = GET_EMA(tm->cmd) | tm->ca;   // 18-bit byte address
    uint32_t cbc = (0200000u - tm->bc) & 0177777u; // byte count

    switch (func) {
    case FNC_READ: {
        uint32_t rl = tap_len_at(tm, tm->pos);
        if (rl == 0) {                          // tape mark
            tm->sta |= STA_EOF;
            tm->pos += 4;
            break;
        }
        if (rl == 0xFFFFFFFFu) {                // end of medium
            tm->sta |= STA_ILL;
            break;
        }
        uint32_t data = tm->pos + 4;
        uint32_t n = (rl < cbc) ? rl : cbc;
        if (rl > cbc) {
            tm->sta |= STA_RLE;                 // record longer than the request
        }
        for (uint32_t i = 0; i < n; ++i) {
            mem_write_byte(cpu, (xma + i) & 0777777u, tm->tape[data + i]);
        }
        tm->ca = (uint16_t)((xma + n) & 0177777u);
        tm->bc = (uint16_t)((tm->bc + n) & 0177777u);
        tm->pos += 4 + ((rl + 1) & ~1u) + 4;    // header + padded data + trailer
        break;
    }
    case FNC_WRITE:
    case FNC_WREXT: {
        // Write cbc bytes as a new record at the current position.
        uint32_t need = tm->pos + 4 + ((cbc + 1) & ~1u) + 4;
        if (tm->wrp || need > tm->tape_len) {
            tm->sta |= STA_ILL;
            break;
        }
        tm->tape[tm->pos + 0] = (uint8_t)(cbc & 0377u);
        tm->tape[tm->pos + 1] = (uint8_t)((cbc >> 8) & 0377u);
        tm->tape[tm->pos + 2] = tm->tape[tm->pos + 3] = 0;
        for (uint32_t i = 0; i < cbc; ++i) {
            tm->tape[tm->pos + 4 + i] = mem_read_byte(cpu, (xma + i) & 0777777u);
        }
        if (cbc & 1u) {
            tm->tape[tm->pos + 4 + cbc] = 0;
        }
        uint32_t trailer = tm->pos + 4 + ((cbc + 1) & ~1u);
        tm->tape[trailer + 0] = (uint8_t)(cbc & 0377u);
        tm->tape[trailer + 1] = (uint8_t)((cbc >> 8) & 0377u);
        tm->tape[trailer + 2] = tm->tape[trailer + 3] = 0;
        tm->ca = (uint16_t)((xma + cbc) & 0177777u);
        tm->bc = 0;
        tm->pos = trailer + 4;
        break;
    }
    case FNC_WREOF: { // write a tape mark (length 0)
        if (tm->wrp || tm->pos + 4 > tm->tape_len) {
            tm->sta |= STA_ILL;
            break;
        }
        tm->tape[tm->pos + 0] = tm->tape[tm->pos + 1] = 0;
        tm->tape[tm->pos + 2] = tm->tape[tm->pos + 3] = 0;
        tm->pos += 4;
        break;
    }
    case FNC_SPACEF: { // space forward one record (per remaining count)
        uint32_t rl = tap_len_at(tm, tm->pos);
        if (rl == 0) {
            tm->sta |= STA_EOF;
            tm->pos += 4;
        } else if (rl == 0xFFFFFFFFu) {
            tm->sta |= STA_ILL;
        } else {
            tm->pos += 4 + ((rl + 1) & ~1u) + 4;
            tm->bc = (uint16_t)((tm->bc + 1) & 0177777u);
        }
        break;
    }
    case FNC_SPACER: { // space reverse one record
        if (tm->pos == 0) {
            tm->sta |= STA_BOT;
            break;
        }
        uint32_t tl = tap_len_at(tm, tm->pos - 4); // trailing length
        if (tl == 0) {
            tm->pos -= 4;
        } else {
            tm->pos -= 4 + ((tl + 1) & ~1u) + 4;
        }
        break;
    }
    default:
        break;
    }
    tm_set_done(cpu);
}

void pdp11_tm_poll(pdp11_cpu *cpu) {
    pdp11_tm11 *tm = &cpu->tm;
    if (!tm->busy || cpu->time_ns < tm->done_ns) {
        return;
    }
    int func = (int)GET_FNC(tm->cmd);
    if (func == FNC_REWIND || func == FNC_UNLOAD) {
        tm->pos = 0;
        tm->sta &= (uint16_t)~STA_REW;
        tm_set_done(cpu);
        return;
    }
    tm_execute(cpu, func);
}

// Start a command (MTC written with GO and DONE set).
static void tm_go(pdp11_cpu *cpu) {
    pdp11_tm11 *tm = &cpu->tm;
    int func = (int)GET_FNC(tm->cmd);
    bool wr = (func == FNC_WRITE || func == FNC_WREOF || func == FNC_WREXT);
    if (tm->tape == NULL || (wr && tm->wrp)) { // not attached, or write-locked
        tm->sta |= STA_ILL;
        tm_set_done(cpu);
        return;
    }
    tm->sta = 0; // clear errors
    if (func == FNC_REWIND) {
        tm->sta |= STA_REW;
    }
    tm->cmd &= (uint16_t)~MTC_DONE;
    pdp11_clr_int(cpu, PDP11_INT_TM);
    tm->busy = true;
    uint32_t cbc = (0200000u - tm->bc) & 0177777u;
    tm->done_ns = cpu->time_ns + TM_BASE_NS + cbc * TM_BYTE_NS;
}

void pdp11_tm_attach(pdp11_cpu *cpu, uint8_t *tape, uint32_t len, bool wrp) {
    cpu->tm.tape = tape;
    cpu->tm.tape_len = len;
    cpu->tm.wrp = wrp;
    cpu->tm.pos = 0;
}

uint16_t pdp11_tm_read(pdp11_cpu *cpu, uint16_t addr) {
    pdp11_tm11 *tm = &cpu->tm;
    switch (addr) {
    case TM_MTS:
        return tm_status(tm);
    case TM_MTC: {
        uint16_t c = tm->cmd & ~MTC_ERR;
        if (tm_status(tm) & STA_EFLGS) {
            c |= MTC_ERR;
        }
        return c;
    }
    case TM_MTBRC: return tm->bc;
    case TM_MTCMA: return tm->ca;
    case TM_MTD:   return tm->db;
    case TM_MTRD:  tm->rdl ^= 0100000u; return tm->rdl; // 10 kHz "clock" toggles
    default:       return 0;
    }
}

void pdp11_tm_write(pdp11_cpu *cpu, uint16_t addr, uint16_t value) {
    pdp11_tm11 *tm = &cpu->tm;
    switch (addr) {
    case TM_MTC:
        if (value & MTC_INIT) { // controller init
            tm->cmd = MTC_DONE;
            tm->sta = tm->bc = tm->ca = tm->db = 0;
            tm->busy = false;
            pdp11_clr_int(cpu, PDP11_INT_TM);
            return;
        }
        if ((value & MTC_IE) == 0) {
            pdp11_clr_int(cpu, PDP11_INT_TM);
        } else if ((tm->cmd & (MTC_DONE | MTC_IE)) == MTC_DONE) {
            pdp11_set_int(cpu, PDP11_INT_TM);
        }
        tm->cmd = (uint16_t)((tm->cmd & ~MTC_RW) | (value & MTC_RW));
        if ((tm->cmd & MTC_DONE) && (value & MTC_GO)) {
            tm_go(cpu);
        }
        break;
    case TM_MTBRC: tm->bc = value; break;
    case TM_MTCMA: tm->ca = value; break;
    case TM_MTD:   tm->db = (uint16_t)(value & 0377u); break;
    default:       break; // MTS / MTRD read-only
    }
}
